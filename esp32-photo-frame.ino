/* esp32-photo-frame: an e-paper art frame.
 *
 * Once a day the device wakes, picks a random pre-processed image from this
 * repo's images/art/processed/ over HTTPS, streams it straight into the panel,
 * and goes back to sleep. The frame is never held in memory, so this runs
 * comfortably on parts without PSRAM.
 *
 * Hardware: Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6).
 * Wiring:   libs/el133/src/el133_pins.h
 * Panel:    libs/el133
 * Network:  libs/artfeed
 * Images:   tools/imgproc turns images/art/*.jpg into the panel's byte format.
 */

#include <Arduino.h>

#include "artfeed.h"
#include "el133.h"
#include "secrets.h"

/* Raw file access to this repo. The trailing slash matters. */
static const char *ART_BASE_URL =
    "https://raw.githubusercontent.com/FutureSharks/esp32-photo-frame/main/images/art/processed/";

/* One image a day. */
static const uint64_t SLEEP_INTERVAL_US = 24ULL * 60 * 60 * 1000000ULL;

/* If a wake fails - no WiFi, a bad download - try again sooner rather than
 * leaving the frame stale for a whole day. */
static const uint64_t RETRY_INTERVAL_US = 60ULL * 60 * 1000000ULL;

/* Paint colour blocks instead of fetching anything. Useful on the bench while
 * the hardware is still in pieces: it needs no network and exercises both
 * controllers. */
#define PAINT_TEST_PATTERN 0

#if PAINT_TEST_PATTERN
static const uint8_t PALETTE[6] = {
    EL133_BLACK, EL133_WHITE, EL133_YELLOW,
    EL133_RED, EL133_BLUE, EL133_GREEN};

/* 3 across x 2 down over the 1200x1600 portrait frame. A bad chip-select split
 * shows up immediately as a mirrored or duplicated half. */
static void blocks_row(int prow, int half, uint8_t out[EL133_ROW_BYTES], void *)
{
    const int base = half * 600;
    const int band = (prow * 2) / EL133_PROWS;

    for (int k = 0; k < EL133_ROW_BYTES; k++)
    {
        const int xe = base + 2 * k; /* even column -> high nibble */
        const uint8_t ce = PALETTE[band * 3 + (xe * 3) / 1200];
        const uint8_t co = PALETTE[band * 3 + ((xe + 1) * 3) / 1200];
        out[k] = (uint8_t)((ce << 4) | co);
    }
}
#endif

static void sleepFor(uint64_t us, const char *why)
{
    Serial.printf("[main] %s; sleeping %llu min\n", why, us / 60000000ULL);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
}

void setup()
{
    Serial.begin(115200);
    uint32_t waitStart = millis();
    while (!Serial && (millis() - waitStart) < 3000)
    {
        delay(10);
    }
    Serial.println("\n=== esp32-photo-frame ===");

    el133_begin();
    el133_set_verbose(true);
    artfeed_set_verbose(true);

#if PAINT_TEST_PATTERN
    Serial.println("[main] test pattern mode, no network");
    bool ok = el133_show_pattern(blocks_row, nullptr);
    Serial.printf("[main] %s\n", ok ? "done" : "REFRESH FAILED");
    sleepFor(SLEEP_INTERVAL_US, "test pattern painted");
#else
    ArtfeedConfig cfg = artfeed_default_config();
    cfg.ssid = WIFI_SSID;
    cfg.password = WIFI_PASSWORD;
    cfg.base_url = ART_BASE_URL;

    if (!artfeed_connect(cfg))
    {
        artfeed_disconnect();
        sleepFor(RETRY_INTERVAL_US, "network unavailable");
    }

    char name[128];
    if (!artfeed_pick(cfg, name, sizeof(name)))
    {
        artfeed_disconnect();
        sleepFor(RETRY_INTERVAL_US, "could not read the manifest");
    }

    /* Streams, then drops the radio before the refresh. On failure the panel
     * keeps the image it already had. */
    if (!artfeed_show(cfg, name))
    {
        artfeed_disconnect();
        sleepFor(RETRY_INTERVAL_US, "could not display the image");
    }

    Serial.printf("[main] showing \"%s\"\n", name);
    sleepFor(SLEEP_INTERVAL_US, "image painted");
#endif
}

void loop()
{
    /* Never reached: setup() ends in deep sleep, and waking restarts the sketch. */
}
