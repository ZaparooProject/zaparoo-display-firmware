# Zaparoo Display Firmware

Firmware for a Zaparoo display accessory: a small panel that sits next to your
reader and shows what is playing.

Tap a card, and the display shows the game's cover art, title, system and how
long you have been playing. When nothing is running it falls back to a clock and
the last thing you played. It is driven over USB by
[Zaparoo Core](https://zaparoo.org), which already knows all of this from its
own media database, so there is nothing to configure per game.

The device looks after its own panel. It dims and blanks when the screen stops
changing, shifts content on a timer, and returns to idle if the host goes quiet,
none of which needs the host to cooperate. Brightness, timeouts and button
bindings are stored on the device, so it behaves the way you set it up even when
plugged into a machine that is not running.

## Hardware

The reference build is two off-the-shelf parts and a ribbon cable, with no
custom PCB. See [docs/hardware.md](docs/hardware.md) for pin mapping and panel
details.

| Part | Description |
| --- | --- |
| Waveshare ESP32-S3-LCD-Driver-Board | ESP32-S3-WROOM-1-N8R8 carrier, 8 MB flash and 8 MB PSRAM |
| BL-HD458002 / HD458002C40 panel | 4.58 inch 320x960 IPS TFT, ST7701S controller, 40-pin FPC |
| USB-C cable | Data-capable. The board is bus powered. |

## Flashing

Release builds are attached to GitHub releases as `bootloader.bin`,
`partition-table.bin` and `zaparoo_display_firmware.bin`. Write them with
[esptool](https://github.com/espressif/esptool):

```sh
pip install esptool
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-size 8MB --flash-freq 80m \
    0x0 bootloader.bin \
    0x8000 partition-table.bin \
    0x20000 zaparoo_display_firmware.bin
```

The board normally resets into download mode on its own. If it does not, hold
BOOT, tap RESET, then release BOOT before running the command.

On Linux the serial device is owned by the `dialout` group; add yourself with
`sudo usermod -aG dialout "$USER"` and log back in. On macOS the board appears
as `/dev/tty.usbmodem*`. On Windows it appears as a `COM` port and needs no
driver, because the ESP32-S3 presents a standard USB CDC interface.

## Controls

The board's BOOT button cycles the playing screen between three layouts: artwork
alone, artwork with title and status, and artwork with catalogue details. Hold it
to blank the panel, and press again to wake it. A press that wakes a dimmed
screen only wakes it, so nothing changes under your finger.

What the button does is configurable, and a host can rebind it or take the events
for itself. See [docs/protocol.md](docs/protocol.md).

## Using it with Zaparoo Core

Core ships a `zapdisplay` reader driver that speaks this protocol, so a display
plugged into the machine running Core works with no extra software. Enable it:

```toml
[readers.drivers.zapdisplay]
enabled = true
```

Core then detects the display and drives it from its own media database.

## Driving it yourself

The display speaks a plain line protocol over USB serial, documented in
[docs/protocol.md](docs/protocol.md), and this repository ships a Python client
for it. That is the route if Core runs on a different machine from the display,
or if you want to drive the panel from something else entirely.

```sh
pip install -r tools/requirements.txt

python tools/zapdisplay_discover.py
python tools/zapdisplay_demo.py --port auto
python tools/zapdisplay_live_daemon.py --core <core-host> --display-port auto
```

The daemon mirrors a running Core instance over its WebSocket API, and maps the
board's button to ZapScript.

## Building from source

Firmware builds need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)
v6.0.1 activated in the shell. The target is pinned in `CMakeLists.txt`, so a
fresh checkout needs no setup step:

```sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The checks that run in CI can all be run locally, and none of them need ESP-IDF
or a board:

```sh
# Scene layout tests: builds the renderer for the host and checks it in pixels.
make -C tests/render test

# Host tools.
pip install -r tools/requirements-dev.txt
ruff check tools && ruff format --check tools
python -m pytest tools/tests

# Firmware formatting. See the lint workflow for the exact invocation.
clang-format -i main/**/*.c main/**/*.h
```

The bundled font and logo data is generated. Regenerate it after changing sizes
or the source artwork in `tools/assets/`:

```sh
python tools/generate_font.py
python tools/generate_logo.py
```

## How it fits together

```text
main/board/     Board pin map and PWM backlight control
main/display/   Panel driver: ST7701S init, RGB timing, framebuffer
main/ui/        Scene layouts, design tokens, drawing canvas, fonts, logo
main/runtime/   Periodic panel care, and the time references it extrapolates
main/settings/  Device-owned settings, persisted in NVS
main/input/     Input vocabulary, binding table, and the on-board button
main/protocol/  zapdisplay line protocol over USB serial
tools/          Host client, diagnostics, and asset generators
tests/render/   Scene layout tests, built for the host
```

`main/ui/` knows nothing about the panel and `main/protocol/` knows nothing
about either, so scenes are composed against a logical canvas and only the
display driver deals with rotation and overscan. Scenes are pure functions of
the display state, which is what lets the layout tests render and inspect them
on a host with no hardware attached.

`main/runtime/` owns everything periodic. It runs from the protocol loop's
existing poll rather than a task of its own, so the firmware is single-threaded
and nothing needs locking.

## Not implemented

- Over-the-air update. The partition table reserves two application slots for
  it, so adding it later will not require re-flashing existing devices.
- An enclosure and mounting solution.

## Licence

Apache License 2.0; see [LICENSE](LICENSE). Third-party components, including
the bundled Noto Sans glyph data, are listed in [NOTICE.md](NOTICE.md).

The Zaparoo name and logo are trademarks of Wizzo Pty Ltd and are not covered by
that grant; see [NOTICE.md](NOTICE.md) before redistributing modified firmware
under a different name.
