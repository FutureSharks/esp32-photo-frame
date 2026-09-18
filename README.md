# esp32-photo-frame

## Pin map

As wired and verified working. ESP32 pins are defined in
[`libs/el133/src/el133_pins.h`](libs/el133/src/el133_pins.h); the HAT header
column is the physical pin on the Inky's 40-pin Pi connector.

| Signal              | BCM | HAT header pin           | XIAO pin | ESP32-S3 GPIO |
|---------------------|-----|--------------------------|----------|---------------|
| SCLK                | 11  | 23                       | D8       | 7             |
| MOSI                | 10  | 19                       | D10      | 9             |
| MISO (unused)       | 9   | 21                       | D9       | 8             |
| INKY_CS (CS_M)      | 26  | 37                       | D1       | 2             |
| INKY_CSB_S (CS_S)   | 16  | 36                       | D5       | 6             |
| INKY_D/C            | 22  | 15                       | D3       | 4             |
| INKY_RESET          | 27  | 13                       | D0       | 1             |
| INKY_BUSY           | 17  | 11                       | D2       | 3             |
| 3V3                 | -   | 1, 17                    | 3V3      | -             |
| GND                 | -   | 6, 9, 14, 20, 25, 30, 34, 39 | GND  | -             |

Notes:

- **CS_M drives portrait columns 0..599, CS_S drives 600..1199.** Swapping them
  mirrors the two halves of the image.
- **BUSY is active low and open-drain**, so it needs a pull-up. The driver
  enables the ESP32's internal one; an external 10k to 3V3 is better for a
  permanent build, since the internal pull-up is ~45k and weak against noise on
  a jumper.
- **The HAT has no enable pin.** `EPD_3V3` is tied to 3V3 through a ferrite, so
  the panel powers up as soon as the rail does. All the high-voltage rails
  (VGH +28 V, VGL -21 V, VDDP/VDDN/VCOM) are generated on the HAT by boost
  converters the panel gates itself.
- **Power the HAT from a supply that can take the inrush.** Vdd is rated
  2.4-3.6 V, so a LiPo must be regulated, not connected directly.
