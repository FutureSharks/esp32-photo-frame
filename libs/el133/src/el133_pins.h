/* Hardware configuration for the Pimoroni Inky Impression 13.3" (PIM774,
 * EL133UF1 / Spectra 6) driven from a XIAO ESP32-S3 on the Seeed ePaper
 * Driver Board.
 *
 * Pin numbers are ESP32 GPIOs; the comment gives the HAT's 40-pin header
 * position. The panel is dual-COG: CS_M takes portrait columns 0..599, CS_S
 * takes 600..1199.
 *
 * The HAT is powered from 3V3 (header pin 1) and GND; there is no enable pin,
 * EPD_3V3 is tied to 3V3 through a ferrite. BUSY is open-drain and needs a
 * pull-up - the internal one works for bring-up, an external 10k is better for
 * a permanent build.
 */
#pragma once

#define EL133_PIN_SCLK D8  // GPIO7  <- header pin 23
#define EL133_PIN_MISO D9  // GPIO8  <- header pin 21 (unused, panel readback)
#define EL133_PIN_MOSI D10 // GPIO9  <- header pin 19
#define EL133_PIN_DC   D3  // GPIO4  <- header pin 15
#define EL133_PIN_RST  D0  // GPIO1  <- header pin 13
#define EL133_PIN_BUSY D2  // GPIO3  <- header pin 11 (active low)
#define EL133_PIN_CS_M D1  // GPIO2  <- header pin 37 (BCM26, left half)
#define EL133_PIN_CS_S D5  // GPIO6  <- header pin 36 (BCM16, right half)
