# Hardware

## Bill of materials

The reference build is two off-the-shelf parts and a ribbon cable, with no
custom PCB.

| Part | Description | Notes |
| --- | --- | --- |
| Waveshare ESP32-S3-LCD-Driver-Board | ESP32-S3-WROOM-1-N8R8 carrier, 8 MB flash and 8 MB OPI PSRAM | Provides the 40-pin 3SPI+RGB FPC connector and native USB. |
| BL-HD458002 / HD458002C40 panel | 4.58 inch 320x960 IPS TFT, ST7701S controller, 40-pin FPC | Sold by several vendors; Adafruit publish a spec sheet for the same panel. |
| USB-C cable | Data-capable, host to board | The board is bus powered; no separate supply is needed. |

The board also carries an 18-pin SPI LCD connector, which this firmware does not
use. It shares pins with the 40-pin path.

## Panel

Panels ship with inconsistent markings. The reference unit is labelled
`BL-HD458002`, `HC0701-03 250519` and `A12/21`, and matches the HD458002C40
specification: a 4.58 inch 320x960 IPS TFT driven by an ST7701S over 3-wire SPI
for register setup plus an RGB TTL dot clock for pixel data.

It is an IPS TFT rather than an OLED, which is what makes the firmware's panel
care what it is: the risk is transient image retention, not permanent emitter
wear. Content shifts on a timer and the panel dims then blanks when nothing is
changing. See `main/runtime/tick.c`.

Vendor documentation is listed in [reference/README.md](reference/README.md).

## 40-pin LCD pin map

Taken from the Waveshare board schematic; see
[reference/README.md](reference/README.md) for where to download it.

| LCD pin | Signal | ESP32-S3 GPIO | Notes |
| --- | --- | --- | --- |
| 1 | LEDA | backlight supply | Backlight anode. |
| 2 | LEDK | backlight return | Backlight cathode. |
| 3 | LEDK | backlight return | Backlight cathode. |
| 4 | GND | - | Ground. |
| 5 | VCI | 3V3 | LCD logic supply. |
| 6 | RESET | `EXIO3` on the IO expander | Not on a GPIO; see below. |
| 7 | NC | - | Not connected. |
| 8 | NC | - | Not connected. |
| 9 | SDA | GPIO1 | 3-wire SPI data / `LCD_SDA`. |
| 10 | SCK | GPIO2 | 3-wire SPI clock / `LCD_SCK`. |
| 11 | CS | GPIO42 | 3-wire SPI chip select / `LCD_CS`. |
| 12 | PCLK | GPIO41 | RGB pixel clock. |
| 13 | DE | GPIO40 | RGB data enable. |
| 14 | VSYNC | GPIO39 | RGB vertical sync. |
| 15 | HSYNC | GPIO38 | RGB horizontal sync. |
| 16 | B0 | not connected | Board routes RGB565 B1..B5 only. |
| 17 | B1 | GPIO5 | RGB565 blue bit 0. |
| 18 | B2 | GPIO45 | RGB565 blue bit 1. |
| 19 | B3 | GPIO48 | RGB565 blue bit 2. |
| 20 | B4 | GPIO47 | RGB565 blue bit 3. |
| 21 | B5 | GPIO21 | RGB565 blue bit 4. |
| 22 | G0 | GPIO14 | RGB565 green bit 0. |
| 23 | G1 | GPIO13 | RGB565 green bit 1. |
| 24 | G2 | GPIO12 | RGB565 green bit 2. |
| 25 | G3 | GPIO11 | RGB565 green bit 3. |
| 26 | G4 | GPIO10 | RGB565 green bit 4. |
| 27 | G5 | GPIO9 | RGB565 green bit 5. |
| 28 | R0 | not connected | Board routes RGB565 R1..R5 only. |
| 29 | R1 | GPIO46 | RGB565 red bit 0. |
| 30 | R2 | GPIO3 | RGB565 red bit 1. |
| 31 | R3 | GPIO8 | RGB565 red bit 2. |
| 32 | R4 | GPIO18 | RGB565 red bit 3. |
| 33 | R5 | GPIO17 | RGB565 red bit 4. |
| 34 | GND | - | Ground. |
| 35 | TP_INT | GPIO16 | Touch interrupt. |
| 36 | TP_SDA | GPIO15 | Touch I2C SDA. |
| 37 | TP_SCL | GPIO7 | Touch I2C SCL. |
| 38 | TP_RESET | `EXIO1` on the IO expander | Not on a GPIO; see below. |
| 39 | TP_VCI | 3V3 | Touch supply. |
| 40 | GND | - | Ground. |

The panel's own R0 and B0 inputs are unconnected on this board, so the two least
significant bits of red and blue are not driven. All sixteen RGB565 lines the
ESP32-S3 emits are wired, so there is no loss of colour depth on the host side.

## Backlight

`BL_PWM` on GPIO6 is a dimming net rather than a simple enable: it drives a FET
on the LED cathode return through R9/R10/R11 into the AP3032 boost converter.
The firmware runs it from LEDC at 20 kHz with 10-bit duty, which keeps the
switching out of the audible band and clear of any beat with the panel's 16 MHz
dot clock.

Percentages are mapped through a gamma curve rather than straight to duty, and
clamped to a floor below which the boost converter stops regulating cleanly. The
floor is set conservatively at about 2 percent; measure the real knee before
lowering it.

## IO expander

A TCA9554PWR 8-bit I2C expander (U5, schematic block "I/O Extensions") sits on
the shared touch I2C bus, SDA GPIO15 and SCL GPIO7. The firmware does not drive
it, but it carries both reset lines and a second backlight control:

| Line | Net | Purpose |
| --- | --- | --- |
| `EXIO1` | `TP_RST` | Touch controller reset |
| `EXIO2` | `BL_EN` | Backlight boost converter enable, a true zero-power off |
| `EXIO3` | `LCD_RST` | Panel reset |
| `EXIO4`, `EXIO5` | `E4`, `E5` | Broken out to the expansion header |
| `EXIO6`-`EXIO8` | - | Spare |

The firmware blanks by taking the LEDC duty to zero rather than cutting
`BL_EN`, which is enough for panel care and needs no I2C driver. `BL_EN` is the
option if a genuinely zero-power state is ever wanted.

Because `LCD_RST` is behind the expander, the ST7701S is not reset by the
firmware and relies on power-on reset. This has proven reliable on the reference
board.

## Buttons

The board fits two keys: BOOT on GPIO0 and RESET on `CHIP_PU`. Only BOOT is
usable as an input, and the firmware treats it as button 0. It is already
debounced by an RC network.

GPIO0 is a strapping pin. Using it as a runtime input is fine, but holding it
through a reset enters download mode. That is a property of the board rather
than something the firmware can hide.

## Display timing

Values in `main/display/hd458002c40.c`, adapted from Adafruit's published Qualia
reference material for the same panel controller:

| Parameter | Value |
| --- | --- |
| RGB active area | 400x960 |
| Visible area | 320x960, after 80 px of left overscan |
| Pixel clock | 16 MHz |
| HSYNC pulse / front porch / back porch | 10 / 50 / 30 |
| VSYNC pulse / front porch / back porch | 2 / 14 / 17 |
| PCLK active high | false |

The active area is wider than the panel deliberately. Driving exactly 320x960
leaves a band of noise along one edge; the extra 80 columns push it off the
visible area, and every write is offset into the wider framebuffer.

The logical UI is landscape 960x320 and is rotated 90 degrees clockwise into the
portrait framebuffer. Only the display driver knows about the rotation and the
overscan; see `main/ui/canvas.h`.

## Flash layout

The 8 MB flash is partitioned by `partitions.csv`:

| Partition | Type | Offset | Size |
| --- | --- | ---: | ---: |
| `nvs` | data | 0x9000 | 24K |
| `otadata` | data | 0xf000 | 8K |
| `phy_init` | data | 0x11000 | 4K |
| `ota_0` | app | 0x20000 | 3M |
| `ota_1` | app | 0x320000 | 3M |
| `storage` | data (spiffs) | 0x620000 | 1920K |

Two application slots are reserved even though over-the-air update is not
implemented. Changing this layout after devices ship would force every existing
user through a full serial re-flash.
