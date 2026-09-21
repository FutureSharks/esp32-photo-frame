# esp32-photo-frame

- Python Library: https://github.com/pimoroni/inky
- Tool for reducing, tone-mapping, and dithering images https://github.com/paperlesspaper/epdoptimize
- Google drive sync to https://github.com/myembeddedstuff/EInk_PictureFrame_GoogleDrive
- Waveshare module with example code: https://github.com/waveshareteam/ESP32-S3-ePaper-13.3E6
  - Docs https://docs.waveshare.com/ESP32-S3-ePaper-13.3E6
- Inkplate module:
  - https://docs.soldered.com/inkplate/13spectra/basics/printing-text/
  - https://docs.soldered.com/inkplate/13spectra/micropython/basics/drawing-graphics/
  - https://github.com/SolderedElectronics/Inkplate-Arduino-library/blob/master/examples/Inkplate13SPECTRA/Basic/Inkplate13SPECTRA_Simple/Inkplate13SPECTRA_Simple.ino



https://www.reddit.com/r/raspberry_pi/comments/1vxrca2/ported_pimoronis_inky_impression_133_driver_from/?share_id=lxACUFob_nR_Ou7qSxVQB

| Inky name        | Inky schematic pin (link) | RPI header pin               | Python pin | Seeed XIAO pin name (link) |
|------------------|---------------------------|------------------------------|------------|----------------------------|
| SCLK             | 11                        | 23                           | 11         | D8                         |
| MOSI             | 10                        | 19                           | 10         | D10                        |
| INKY_CS (CS0)    | 26                        | 37                           | 26         | D7                         |
| INKY_CDB_S (CS1) | 16                        | 36                           | 16         | 41                         |
| INKY_BUSY        | 17                        | 11                           | 17         | D3                         |
| INKY_RESET       | 27                        | 13                           | 27         | 38                         |
| INKY_D/C         | 22                        | 15                           | 22         | 10                         |
| 3.3 V            | -                         | 1, 17                        | -          | -                          |
| 5 V              | -                         | 2, 4                         | -          | -                          |
| GND              | -                         | 9, 25, 39, 30, 34, 14, 20, 6 | -          | -                          |





## Image processing

`tools/imgproc` converts source images into the exact byte stream the firmware
pushes over SPI, so the ESP32 does no decoding and needs no framebuffer.

Run it from the repository root; the default input and output paths are
relative to it.

```sh
go run ./tools/imgproc                    # images/art/*.jpg -> images/art/processed/
go run ./tools/imgproc -preset restore    # faded scans and paintings
go run ./tools/imgproc -preset posterscan # warm paper, strong flat colour
go run ./tools/imgproc -list-presets      # also -list-palettes, -list-kernels
```

Alongside each source image it writes `<name>.preview.jpg`: the dithered result
at the source's own resolution, in the palette's calibrated colours, so the two
can be flipped between in any viewer to see what the panel will make of it.
Previews are build output and are gitignored. `-preview-source panel` writes the
actual cropped 1200x1600 frame instead, which is the honest check - the default
full-resolution preview dithers at a finer pitch than the panel has.

The `.bin` files are always the cropped panel frame, 960,000 bytes each.

Settings that affect the packed output are recorded in `manifest.txt`, so
changing a flag repacks everything; otherwise only images newer than their
output are redone.

### Why the tone presets matter

E-paper is reflective and has perhaps a third of sRGB's brightness range, so
feeding it a photograph straight from a phone wastes most of what it can do:
shadows crush to black and highlights clip to the white ink. The presets
redistribute the image into the range the panel actually has - most importantly
by compressing LAB lightness into the palette's real endpoints - before any ink
is chosen. On a typical image that cuts the pixels lying outside the palette's
reach from ~19% to ~14%, and the error the dither has to throw away with them.

The tone pipeline, the calibrated palettes and the dither kernels are ported
from [paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize),
Copyright 2025 Robert Guehne, a browser library licensed under Apache-2.0, and
adapted to run offline in Go. Its "clarity" stage, its fast preview paths and
its automatic preset recommender are not ported.

`palette.go`, `tone.go`, `dither.go` and `preset.go` are therefore Apache-2.0
rather than MIT like the rest of this repository. `tools/imgproc/NOTICE` records
what was taken and how it was changed, and `tools/imgproc/LICENSE-APACHE-2.0`
is a copy of the licence.

Each palette entry carries two colours: the calibrated one, which is what the
ink actually looks like once it has settled, and the device primary the
controller is sent. Dithering against the measured colour and only then emitting
the device code is what stops the output looking like a 1990s GIF. `-saturation`
blends between the two if you want punch over accuracy.

# to do

- Maybe remove this cert bundle thing
- Remove C6 or S3 logic when all is working
- Display errors on screen

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
