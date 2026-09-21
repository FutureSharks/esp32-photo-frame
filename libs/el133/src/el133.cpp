/*
 * el133.cpp: EL133UF1 13.3" Spectra 6 driver. See el133.h.
 *
 * Init register values, per-command setup delay and the rotate/split paint are
 * ported from other-repos/projets/el133-pico-driver, which matches Pimoroni's
 * inky_el133uf1.py.
 */
#include "el133.h"

#include "el133_pins.h"
#include <SPI.h>
#include <soc/soc_caps.h>

/* Which SPI peripheral to drive the panel from. The S3 has a spare general
 * purpose bus (SPI3 / HSPI); the C6 and the other single-bus parts have only
 * SPI2, where HSPI is out of range and spiStartBus() refuses it at runtime -
 * the build succeeds and the panel simply never receives anything. */
#if SOC_SPI_PERIPH_NUM > 2
#define EL133_SPI_BUS HSPI
#else
#define EL133_SPI_BUS FSPI
#endif

/* The panel's SPI clock. The Pico reference runs the 13.3" at 4 MHz; Pimoroni
 * uses 10 MHz on the Pi. 4 MHz costs ~2 s extra on a 960 KB frame, which is
 * noise next to a 35 s refresh. */
static const uint32_t EL133_SPI_HZ = 4000000;

/* D/C-to-clock setup before every opcode. Shorter and the controller drops
 * commands - this is the single biggest difference from the Seeed driver. */
static const uint32_t EL133_CMD_SETUP_MS = 300;

/* How long to give the panel to assert BUSY after a refresh command before
 * concluding it ignored it, and how long to allow the refresh itself. */
static const uint32_t EL133_ASSERT_TIMEOUT_MS = 5000;
static const uint32_t EL133_REFRESH_TIMEOUT_MS = 60000;

static SPIClass el133_spi(EL133_SPI_BUS);
static bool el133_verbose = false;
static uint8_t el133_dtm_cs; /* CS held during the current DTM stream */

static const uint8_t cs_m = EL133_PIN_CS_M;      /* left half, columns 0..599     */
static const uint8_t cs_s = EL133_PIN_CS_S;      /* right half, columns 600..1199 */
static const uint8_t cs_both[] = {EL133_PIN_CS_M, EL133_PIN_CS_S};

#define EL133_LOG(...)                   \
    do                                   \
    {                                    \
        if (el133_verbose)               \
            Serial.printf(__VA_ARGS__);  \
    } while (0)

void el133_set_verbose(bool on)
{
    el133_verbose = on;
}

bool el133_is_busy()
{
    return digitalRead(EL133_PIN_BUSY) == LOW;
}

void el133_begin()
{
    pinMode(EL133_PIN_DC, OUTPUT);
    digitalWrite(EL133_PIN_DC, LOW);
    pinMode(EL133_PIN_RST, OUTPUT);
    digitalWrite(EL133_PIN_RST, HIGH);

    /* BUSY is open-drain; without a pull-up a released line reads as a
     * permanent busy. */
    pinMode(EL133_PIN_BUSY, INPUT_PULLUP);

    pinMode(EL133_PIN_CS_M, OUTPUT);
    digitalWrite(EL133_PIN_CS_M, HIGH);
    pinMode(EL133_PIN_CS_S, OUTPUT);
    digitalWrite(EL133_PIN_CS_S, HIGH);

    el133_spi.begin(EL133_PIN_SCLK, EL133_PIN_MISO, EL133_PIN_MOSI, -1);
}

static inline void cs_assert(const uint8_t *cs_pins, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
        digitalWrite(cs_pins[i], LOW);
}

static inline void cs_release(const uint8_t *cs_pins, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
        digitalWrite(cs_pins[i], HIGH);
}

/* Hardware reset: RST is active low. */
static void el133_reset(uint32_t low_ms, uint32_t high_ms, uint32_t settle_ms)
{
    digitalWrite(EL133_PIN_RST, LOW);
    delay(low_ms);
    digitalWrite(EL133_PIN_RST, HIGH);
    delay(high_ms);
    delay(settle_ms);
}

/* Send one command, with optional trailing data, to the given chip-select
 * line(s) - all asserted together, active low, for the whole transaction. */
static void el133_command(const uint8_t *cs_pins, uint8_t n_cs, uint8_t cmd,
                          const uint8_t *data, size_t len)
{
    cs_assert(cs_pins, n_cs);
    digitalWrite(EL133_PIN_DC, LOW); /* command */
    delay(EL133_CMD_SETUP_MS);

    el133_spi.beginTransaction(SPISettings(EL133_SPI_HZ, MSBFIRST, SPI_MODE0));
    el133_spi.transfer(cmd);
    if (data != nullptr && len > 0)
    {
        digitalWrite(EL133_PIN_DC, HIGH); /* data */
        el133_spi.writeBytes(data, len);
    }
    el133_spi.endTransaction();

    cs_release(cs_pins, n_cs);
}

/* Open a data-transfer command on one controller and leave CS asserted so the
 * frame can be streamed in pieces.
 *
 * The SPI transaction is closed again straight away, and each write opens its
 * own. Chip-select and D/C stay put, so the controller sees one uninterrupted
 * transfer either way - but the bus is not locked between writes, which makes
 * it safe to block on network or card I/O mid-frame. */
static void el133_dtm_begin(uint8_t cs_pin, uint8_t dtm_cmd)
{
    el133_dtm_cs = cs_pin;
    digitalWrite(cs_pin, LOW);
    digitalWrite(EL133_PIN_DC, LOW);
    delay(EL133_CMD_SETUP_MS);

    el133_spi.beginTransaction(SPISettings(EL133_SPI_HZ, MSBFIRST, SPI_MODE0));
    el133_spi.transfer(dtm_cmd);
    el133_spi.endTransaction();

    digitalWrite(EL133_PIN_DC, HIGH); /* data follows */
}

static void el133_dtm_write(const uint8_t *data, size_t len)
{
    el133_spi.beginTransaction(SPISettings(EL133_SPI_HZ, MSBFIRST, SPI_MODE0));
    el133_spi.writeBytes(data, len);
    el133_spi.endTransaction();
}

static void el133_dtm_end()
{
    digitalWrite(el133_dtm_cs, HIGH);
}

void el133_stream_begin(int half)
{
    el133_dtm_begin(half == 0 ? cs_m : cs_s, 0x10);
}

void el133_stream_write(const uint8_t *data, size_t len)
{
    el133_dtm_write(data, len);
}

void el133_stream_end()
{
    el133_dtm_end();
}

/* Wait out a refresh. BUSY is active low: the controller pulls it low while
 * refreshing and releases it when done. Watching for the assert first is what
 * stops us sending power-off mid-refresh, which can latch the panel into a
 * fault that only removing power clears. */
static bool el133_wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = millis();

    while (!el133_is_busy())
    {
        if (millis() - t0 > EL133_ASSERT_TIMEOUT_MS)
        {
            EL133_LOG("[el133] BUSY never asserted in %lu ms - panel ignored the refresh\n",
                      (unsigned long)EL133_ASSERT_TIMEOUT_MS);
            return false;
        }
        delay(5);
    }
    EL133_LOG("[el133] BUSY asserted after %lu ms, refreshing\n",
              (unsigned long)(millis() - t0));

    while (el133_is_busy())
    {
        if (millis() - t0 > timeout_ms)
        {
            EL133_LOG("[el133] BUSY still low after %lu ms - refresh timed out\n",
                      (unsigned long)(millis() - t0));
            return false;
        }
        delay(50);
    }
    EL133_LOG("[el133] BUSY released after %lu ms\n", (unsigned long)(millis() - t0));
    return true;
}

/* Reset + push the verified init sequence to the controllers. */
void el133_init_panel()
{
    el133_reset(30, 30, 300);

    static const uint8_t ANTM[] = {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55};
    static const uint8_t CMD66[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
    static const uint8_t PSR[] = {0xDF, 0x6B};
    static const uint8_t DCDC[] = {0x44, 0x54, 0x00};
    static const uint8_t PLL[] = {0x08};
    static const uint8_t CDI[] = {0x37};
    static const uint8_t TCON[] = {0x03, 0x03};
    static const uint8_t POFS0[] = {0x00, 0xC0, 0x03, 0xA8};
    static const uint8_t POFS1[] = {0x00, 0xC0, 0x03, 0x9A};
    static const uint8_t AGID[] = {0x10};
    static const uint8_t PWS[] = {0x22};
    static const uint8_t CCSET[] = {0x01};
    static const uint8_t TRES[] = {0x04, 0xB0, 0x03, 0x20}; /* 1200 x 800 per controller */
    static const uint8_t CMDA4[] = {0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t PWR[] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
    static const uint8_t ENBUF[] = {0x07};
    static const uint8_t BTSTP[] = {0xE0, 0x20};
    static const uint8_t BVDDP[] = {0x01};
    static const uint8_t BTSTN[] = {0xE0, 0x20};
    static const uint8_t BBVDN[] = {0x01};
    static const uint8_t VCOMP[] = {0x02};

    uint32_t t0 = millis();
    el133_command(&cs_m, 1, 0x74, ANTM, sizeof ANTM);
    el133_command(cs_both, 2, 0xF0, CMD66, sizeof CMD66);
    el133_command(cs_both, 2, 0x00, PSR, sizeof PSR);
    el133_command(&cs_m, 1, 0xA5, DCDC, sizeof DCDC);
    el133_command(cs_both, 2, 0x30, PLL, sizeof PLL);
    el133_command(cs_both, 2, 0x50, CDI, sizeof CDI);
    el133_command(cs_both, 2, 0x60, TCON, sizeof TCON);
    el133_command(&cs_m, 1, 0x03, POFS0, sizeof POFS0);
    el133_command(&cs_s, 1, 0x03, POFS1, sizeof POFS1);
    el133_command(cs_both, 2, 0x86, AGID, sizeof AGID);
    el133_command(cs_both, 2, 0xE3, PWS, sizeof PWS);
    el133_command(cs_both, 2, 0xE0, CCSET, sizeof CCSET);
    el133_command(cs_both, 2, 0x61, TRES, sizeof TRES);
    el133_command(&cs_m, 1, 0xA4, CMDA4, sizeof CMDA4);
    el133_command(&cs_m, 1, 0x01, PWR, sizeof PWR);
    el133_command(&cs_m, 1, 0xB6, ENBUF, sizeof ENBUF);
    el133_command(&cs_m, 1, 0x06, BTSTP, sizeof BTSTP);
    el133_command(&cs_m, 1, 0xB7, BVDDP, sizeof BVDDP);
    el133_command(&cs_m, 1, 0x05, BTSTN, sizeof BTSTN);
    el133_command(&cs_m, 1, 0xB0, BBVDN, sizeof BBVDN);
    el133_command(&cs_m, 1, 0xB1, VCOMP, sizeof VCOMP);
    EL133_LOG("[el133] init complete in %lu ms\n", (unsigned long)(millis() - t0));
}

/* Power on, trigger the refresh, wait for BUSY to release, power off. */
bool el133_refresh()
{
    static const uint8_t z = 0x00;

    el133_command(cs_both, 2, 0x04, nullptr, 0); /* power on */
    delay(300);

    el133_command(cs_both, 2, 0x12, &z, 1); /* refresh */
    bool ok = el133_wait_ready(EL133_REFRESH_TIMEOUT_MS);

    el133_command(cs_both, 2, 0x02, &z, 1); /* power off */
    delay(300);

    return ok;
}

bool el133_show_pattern(el133_row_fn fn, void *ctx)
{
    el133_init_panel();

    uint8_t row[EL133_ROW_BYTES];
    uint32_t t0 = millis();

    el133_dtm_begin(cs_m, 0x10);
    for (int i = 0; i < EL133_PROWS; i++)
    {
        fn(i, 0, row, ctx);
        el133_dtm_write(row, EL133_ROW_BYTES);
    }
    el133_dtm_end();

    el133_dtm_begin(cs_s, 0x10);
    for (int i = 0; i < EL133_PROWS; i++)
    {
        fn(i, 1, row, ctx);
        el133_dtm_write(row, EL133_ROW_BYTES);
    }
    el133_dtm_end();

    EL133_LOG("[el133] streamed both controllers in %lu ms, refreshing (~35 s)\n",
              (unsigned long)(millis() - t0));
    return el133_refresh();
}

/* Row generator that rotates a landscape 1600x1200 frame 90 deg CW into
 * portrait. Portrait pixel (i,j) maps to landscape (row = base - j, col = i);
 * base is 1199 for the left controller, 599 for the right.
 * L(r,c) = frame[r*800 + (c>>1)], high nibble for even c. */
static void frame_row(int prow, int half, uint8_t out[EL133_ROW_BYTES], void *ctx)
{
    const uint8_t *frame = (const uint8_t *)ctx;
    const uint8_t shift = (prow & 1) ? 0 : 4; /* nibble for portrait column */
    const size_t col = (size_t)(prow >> 1);
    const int base = (half == 0) ? 1199 : 599;

    for (int k = 0; k < EL133_ROW_BYTES; k++)
    {
        uint8_t hb = frame[(size_t)(base - 2 * k) * 800 + col];
        uint8_t lb = frame[(size_t)(base - 1 - 2 * k) * 800 + col];
        out[k] = (uint8_t)((((hb >> shift) & 0x0F) << 4) | ((lb >> shift) & 0x0F));
    }
}

bool el133_show_frame(const uint8_t *frame)
{
    return el133_show_pattern(frame_row, (void *)frame);
}
