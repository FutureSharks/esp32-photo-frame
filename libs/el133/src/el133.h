/*
 * el133.h: Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6) driver for
 * Arduino / ESP32.
 *
 * Ported from other-repos/projets/el133-pico-driver, whose init values and
 * timing match Pimoroni's inky_el133uf1.py - the verified reference for this
 * panel. Two details that the Seeed_GFX T133A01 driver gets wrong and that this
 * panel will not tolerate:
 *
 *   - every command needs a ~300 ms D/C-to-clock setup or the controller
 *     silently drops it;
 *   - BUSY is active LOW and open-drain, so it needs a host pull-up, and a
 *     refresh must be waited out by watching the line assert and then release.
 *
 * The panel is landscape-native 1600x1200, but its two controllers scan a
 * 1200x1600 PORTRAIT frame split at column 600: 300 bytes x 1600 rows each.
 * el133_show_frame() rotates a landscape frame 90 deg CW and splits it;
 * el133_show_pattern() generates portrait rows directly, with no 960 KB buffer.
 *
 * Pin assignments come from el133_pins.h.
 */
#pragma once

#include <Arduino.h>

/* Landscape-native geometry. Frames are packed 4bpp (2 px/byte, high nibble =
 * even column), 800 bytes/row, 1200 rows. */
#define EL133_W           1600
#define EL133_H           1200
#define EL133_FRAME_BYTES ((size_t)EL133_W * EL133_H / 2) /* 960000 */

#define EL133_ROW_BYTES 300  /* one portrait row, one controller (600 px / 2) */
#define EL133_PROWS     1600 /* portrait rows each controller scans */

/* Spectra 6 palette: the panel's 4-bit colour codes. Note 0x4 is unused. */
enum
{
    EL133_BLACK = 0x0,
    EL133_WHITE = 0x1,
    EL133_YELLOW = 0x2,
    EL133_RED = 0x3,
    EL133_BLUE = 0x5,
    EL133_GREEN = 0x6,
};

/* Fill one 300-byte portrait row for one controller.
 *   prow : portrait row, 0..1599
 *   half : 0 = left half  (CS_M, portrait columns 0..599)
 *          1 = right half (CS_S, portrait columns 600..1199)
 * out[k] packs columns (base+2k, base+2k+1) where base = half*600; the high
 * nibble is the even column. */
typedef void (*el133_row_fn)(int prow, int half, uint8_t out[EL133_ROW_BYTES], void *ctx);

/* Configure SPI and the control pins. Call once, before anything else. */
void el133_begin();

/* Bytes one controller expects, and the whole pre-packed stream: the CS_M half
 * followed by the CS_S half. A pre-packed frame is already rotated and split,
 * so it can be streamed straight through with no buffer. */
#define EL133_HALF_BYTES ((size_t)EL133_ROW_BYTES * EL133_PROWS) /* 480000 */
#define EL133_STREAM_BYTES (EL133_HALF_BYTES * 2)                /* 960000 */

/* Mirror progress and timings to Serial. Off by default. */
void el133_set_verbose(bool on);

/* Read the BUSY line: true when the panel is asserting busy (line low). */
bool el133_is_busy();

/* Reset + init the panel, stream a procedural pattern, and refresh. Blocks for
 * the whole sequence (~7 s of init, then a ~35 s refresh). Returns false if the
 * refresh did not complete - either the panel never asserted BUSY, meaning it
 * ignored the refresh command, or it never released the line before the
 * timeout. */
bool el133_show_pattern(el133_row_fn fn, void *ctx);

/* Same, but rotate + split a landscape 1600x1200 packed-4bpp frame.
 * `frame` must be EL133_FRAME_BYTES long. */
bool el133_show_frame(const uint8_t *frame);

/* ---- Streaming a pre-packed frame -------------------------------------
 *
 * For frames that arrive a chunk at a time (off the network, off a card) and
 * are already in the panel's byte order. The sequence is:
 *
 *     el133_init_panel();
 *     el133_stream_begin(0);  ... writes totalling EL133_HALF_BYTES ...
 *     el133_stream_end();
 *     el133_stream_begin(1);  ... writes totalling EL133_HALF_BYTES ...
 *     el133_stream_end();
 *     el133_refresh();
 *
 * Chip-select is held for the whole of one half but the SPI transaction is
 * not, so it is safe to block on I/O between writes. Nothing appears on the
 * panel until el133_refresh(), so an abandoned stream leaves the previous
 * image untouched - just skip the refresh and retry. */

/* Reset the panel and push the init sequence. Takes ~7 s: the controller needs
 * a 300 ms setup before each of the 21 commands. */
void el133_init_panel();

/* Open a data transfer to one controller. half: 0 = CS_M (portrait columns
 * 0..599), 1 = CS_S (600..1199). */
void el133_stream_begin(int half);

/* Push frame bytes to the controller opened by el133_stream_begin(). Call as
 * many times as needed; chunk size is up to the caller. */
void el133_stream_write(const uint8_t *data, size_t len);

/* Close the transfer and release chip-select. */
void el133_stream_end();

/* Power on, refresh, wait for BUSY to release, power off. Returns false if the
 * panel never asserted BUSY (it ignored the refresh) or never released it
 * before the timeout. Blocks for ~35 s. */
bool el133_refresh();
