#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Generate tight-bounding-box 4-bit alpha bitmap fonts for the firmware renderer.

Glyphs are stored at their ink bounding box rather than in a fixed cell. A fixed
cell pads every glyph out to the widest in the face, which costs roughly four
times the flash for the same coverage.
"""

from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

DEFAULT_FONT_PATH = Path("tools/assets/NotoSans.ttf")
DEFAULT_OUT_PATH = Path("main/ui/generated_fonts.c")

# Weight axis position on the variable face. The design system steps a selected row
# up one weight for an optical correction; the display has no selection, so it
# sets everything at Normal.
FONT_WEIGHT = 400.0

# Contiguous code point runs to bake. Latin-1 Supplement and Latin Extended-A
# cover Western and Central European titles; the ellipsis is what elision draws
# and sits outside every other run.
RANGES: tuple[tuple[int, int], ...] = (
    (0x0020, 0x007E),  # Basic Latin
    (0x00A0, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
    (0x2026, 0x2026),  # horizontal ellipsis
)

FALLBACK_CODEPOINT = ord("?")

# Named for the shared type ladder, sized for a 960x320 bar panel at 221 PPI
# rather than derived from a percentage of a 16:9 screen.
#
# Only the rungs scenes actually set text in are baked. Each one costs flash for
# every glyph in COVERAGE whether it is drawn or not, so an unused rung is pure
# weight; the "small" role has no use on a panel this size, where the
# caption rung is already the smallest thing worth reading across a room. Adding
# a rung back is a line here plus a regenerate.
FONTS: tuple[tuple[str, int], ...] = (
    ("zd_font_hero", 44),
    ("zd_font_title", 34),
    ("zd_font_section", 26),
    ("zd_font_body", 22),
    ("zd_font_caption", 18),
)


@dataclass
class Glyph:
    codepoint: int
    offset: int
    width: int
    height: int
    advance: int
    bearing_x: int
    bearing_y: int
    data: bytes


def codepoints() -> list[int]:
    points: list[int] = []
    for first, last in RANGES:
        points.extend(range(first, last + 1))
    return points


def load_font(path: Path, pixel_size: int) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(path), pixel_size)
    # A static face has no variation axes and keeps its own weight.
    with contextlib.suppress(OSError):
        font.set_variation_by_axes([FONT_WEIGHT])
    return font


def quantise(value: int) -> int:
    """Map 8-bit coverage to 4 bits so full coverage lands on 15 exactly."""
    return (value * 15 + 127) // 255


def pack_rows(image: Image.Image) -> bytes:
    """Pack an 8-bit coverage image into 4-bit alpha, two pixels per byte."""
    width, height = image.size
    pixels = image.load()
    packed = bytearray()
    for y in range(height):
        for x in range(0, width, 2):
            high = quantise(pixels[x, y])
            low = quantise(pixels[x + 1, y]) if x + 1 < width else 0
            packed.append((high << 4) | low)
    return bytes(packed)


def render_glyph(
    font: ImageFont.FreeTypeFont, char: str, ascent: int, descent: int, pad: int
) -> tuple[Image.Image, int, int] | None:
    """Render one glyph and return its ink image plus left and top bearings.

    Pillow's textbbox reports the layout box rather than the ink box, so the
    bearings are measured off the rendered mask instead.
    """
    canvas_w = int(font.getlength(char)) + 4 * pad
    canvas_h = ascent + descent + 2 * pad
    image = Image.new("L", (max(canvas_w, 1), max(canvas_h, 1)), 0)
    draw = ImageDraw.Draw(image)
    origin = (2 * pad, pad + ascent)
    draw.text(origin, char, font=font, fill=255, anchor="ls")

    ink = image.getbbox()
    if ink is None:
        return None
    left, top = ink[0], ink[1]
    return image.crop(ink), left - origin[0], origin[1] - top


def build_glyphs(font: ImageFont.FreeTypeFont, ascent: int, descent: int, pixel_size: int) -> list[Glyph]:
    glyphs: list[Glyph] = []
    offset = 0
    for codepoint in codepoints():
        char = chr(codepoint)
        advance = round(font.getlength(char))
        rendered = render_glyph(font, char, ascent, descent, pixel_size)
        if rendered is None:
            glyphs.append(Glyph(codepoint, 0, 0, 0, advance, 0, 0, b""))
            continue

        image, bearing_x, bearing_y = rendered
        data = pack_rows(image)
        width, height = image.size
        check_field(codepoint, "advance", advance, 0, 255)
        check_field(codepoint, "width", width, 0, 255)
        check_field(codepoint, "height", height, 0, 255)
        check_field(codepoint, "bearing_x", bearing_x, -128, 127)
        check_field(codepoint, "bearing_y", bearing_y, -128, 127)
        glyphs.append(Glyph(codepoint, offset, width, height, advance, bearing_x, bearing_y, data))
        offset += len(data)
    return glyphs


def check_field(codepoint: int, name: str, value: int, low: int, high: int) -> None:
    if not low <= value <= high:
        raise ValueError(f"U+{codepoint:04X} {name}={value} does not fit in [{low}, {high}]")


def measure(font: ImageFont.FreeTypeFont, char: str, ascent: int, descent: int, pixel_size: int) -> int:
    rendered = render_glyph(font, char, ascent, descent, pixel_size)
    return 0 if rendered is None else rendered[0].size[1]


def format_bytes(values: bytes) -> list[str]:
    lines = []
    for i in range(0, len(values), 16):
        chunk = values[i : i + 16]
        lines.append("    " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")
    return lines


def emit_font(name: str, pixel_size: int, font_path: Path) -> tuple[list[str], int]:
    font = load_font(font_path, pixel_size)
    ascent, descent = font.getmetrics()
    glyphs = build_glyphs(font, ascent, descent, pixel_size)
    blob = b"".join(glyph.data for glyph in glyphs)

    points = codepoints()
    fallback = points.index(FALLBACK_CODEPOINT)
    cap_height = measure(font, "H", ascent, descent, pixel_size)
    x_height = measure(font, "x", ascent, descent, pixel_size)

    lines = [f"static const uint8_t {name}_bitmap[] = {{"]
    lines.extend(format_bytes(blob))
    lines.append("};")
    lines.append("")
    lines.append(f"static const zd_glyph_t {name}_glyphs[] = {{")
    for glyph in glyphs:
        lines.append(
            f"    {{{glyph.offset}, {glyph.width}, {glyph.height}, {glyph.advance}, "
            f"{glyph.bearing_x}, {glyph.bearing_y}}}, // U+{glyph.codepoint:04X}"
        )
    lines.append("};")
    lines.append("")
    lines.append(f"static const zd_font_range_t {name}_ranges[] = {{")
    base = 0
    for first, last in RANGES:
        lines.append(f"    {{0x{first:04X}, 0x{last:04X}, {base}}},")
        base += last - first + 1
    lines.append("};")
    lines.append("")
    lines.append(f"const zd_font_t {name} = {{")
    lines.append(f"    .bitmap = {name}_bitmap,")
    lines.append(f"    .glyphs = {name}_glyphs,")
    lines.append(f"    .ranges = {name}_ranges,")
    lines.append(f"    .glyph_count = {len(glyphs)},")
    lines.append(f"    .fallback_glyph = {fallback},")
    lines.append(f"    .range_count = {len(RANGES)},")
    lines.append(f"    .pixel_size = {pixel_size},")
    lines.append(f"    .ascent = {ascent},")
    lines.append(f"    .descent = {descent},")
    lines.append(f"    .line_height = {ascent + descent},")
    lines.append(f"    .cap_height = {cap_height},")
    lines.append(f"    .x_height = {x_height},")
    lines.append("};")
    lines.append("")

    glyph_table_bytes = len(glyphs) * 12
    return lines, len(blob) + glyph_table_bytes


def main(font_path: Path, out_path: Path) -> None:
    parts = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// Copyright (c) 2026 The Zaparoo Project Contributors.",
        "",
        "// Generated by tools/generate_font.py. Do not edit by hand.",
        "// Source font: Noto Sans, SIL Open Font License 1.1. See NOTICE.md.",
        '#include "ui/font.h"',
        "",
    ]

    total = 0
    for name, pixel_size in FONTS:
        lines, size = emit_font(name, pixel_size, font_path)
        parts.extend(lines)
        total += size
        print(f"{name:20s} {pixel_size:>3}px {size:>9,} bytes")
    print(f"{'total':20s} {'':>5} {total:>9,} bytes")

    out_path.write_text("\n".join(parts), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--font",
        type=Path,
        default=DEFAULT_FONT_PATH,
        help=f"TrueType file to rasterise (default: {DEFAULT_FONT_PATH})",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT_PATH,
        help=f"generated C file to write (default: {DEFAULT_OUT_PATH})",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    main(args.font, args.out)
