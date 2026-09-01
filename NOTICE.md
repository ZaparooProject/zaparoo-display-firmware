# Third-party notices

Zaparoo Display Firmware is licensed under the Apache License 2.0; see
[LICENSE](LICENSE). It also includes or derives from the following work.

## Noto Sans

`main/ui/generated_fonts.c` contains glyph bitmaps rasterised from the Noto Sans
typeface by `tools/generate_font.py`, from the copy vendored at
`tools/assets/NotoSans.ttf`. Noto Sans is Copyright 2022 The Noto Project
Authors and is licensed under the SIL Open Font License 1.1, reproduced in full
at [licenses/NotoSans-OFL.txt](licenses/NotoSans-OFL.txt).

The generated file is a bitmap rendering rather than a redistribution of the
font binary, and it does not use the reserved font name.

Noto Sans is also the interface typeface of the Zaparoo frontend, so the display
accessory and the frontend set text in the same face.

## Zaparoo brand mark

`main/ui/generated_logo.c` contains a bitmap rendering of the Zaparoo logo,
baked by `tools/generate_logo.py` from the artwork vendored at
`tools/assets/logo-on-dark-144.png` and `tools/assets/logo-on-dark-256.png`.

The Zaparoo name and logo are trademarks of Wizzo Pty Ltd, included here with
permission for use in official Zaparoo display firmware builds. The Apache
License 2.0 covers the source code in this repository; as section 6 of that
licence makes explicit, it does not grant rights to use the licensor's
trademarks. Redistributing modified firmware under a different name should
replace this artwork.

## ST7701S panel initialisation sequence

The register sequence in `main/display/hd458002c40.c` is adapted from Adafruit's
published Qualia / HD458002C40 reference material for the same panel controller.
Adafruit's driver code is MIT licensed.

## ESP-IDF

The firmware builds against Espressif's ESP-IDF, which is licensed under the
Apache License 2.0. ESP-IDF is not vendored in this repository; see
[README.md](README.md) for installation.
