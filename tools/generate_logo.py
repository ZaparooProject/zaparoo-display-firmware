#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Bake the Zaparoo logo into C arrays for the firmware renderer.

The mark is two different kinds of artwork and is stored as two layers. The
bolt carries a gradient and needs real colour; the wordmark is flat white, so it
is stored as an alpha mask and tinted with a palette colour at draw time, which
is why only the on-dark artwork needs vendoring.

The split is by chroma rather than by column: the wordmark's leading Z overlaps
the bolt horizontally, so no vertical cut separates them.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

DEFAULT_ASSET_DIR = Path("tools/assets")
DEFAULT_OUT_PATH = Path("main/ui/generated_logo.c")

# Saturation above which a pixel belongs to the bolt. The wordmark is pure
# white and the bolt is saturated throughout, so anything near this threshold is
# an antialiased boundary pixel rather than a real classification decision.
CHROMA_THRESHOLD = 24

# Rungs to bake, matching the pre-rendered source artwork available at
# these exact sizes. Resampling the 600px master instead would discard its
# hinting at the size the display actually paints.
RUNGS = (144, 256)


@dataclass
class Layer:
    x: int
    y: int
    width: int
    height: int
    rgb: list[int]
    alpha: bytes


def quantise(value: int) -> int:
    """Map 8-bit coverage to 4 bits so full coverage lands on 15 exactly."""
    return (value * 15 + 127) // 255


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)


def pack_alpha(values: list[list[int]]) -> bytes:
    packed = bytearray()
    for row in values:
        for i in range(0, len(row), 2):
            high = quantise(row[i])
            low = quantise(row[i + 1]) if i + 1 < len(row) else 0
            packed.append((high << 4) | low)
    return bytes(packed)


def split_layers(image: Image.Image) -> tuple[Layer, Layer]:
    """Separate the coloured bolt from the flat wordmark."""
    width, height = image.size
    pixels = image.load()
    bolt = Image.new("L", (width, height), 0)
    word = Image.new("L", (width, height), 0)
    bolt_mask = bolt.load()
    word_mask = word.load()

    for y in range(height):
        for x in range(width):
            red, green, blue, alpha = pixels[x, y]
            if alpha == 0:
                continue
            if max(red, green, blue) - min(red, green, blue) > CHROMA_THRESHOLD:
                bolt_mask[x, y] = alpha
            else:
                word_mask[x, y] = alpha

    return build_layer(image, bolt, colour=True), build_layer(image, word, colour=False)


def build_layer(source: Image.Image, mask: Image.Image, *, colour: bool) -> Layer:
    box = mask.getbbox()
    if box is None:
        raise ValueError("logo layer is empty; check CHROMA_THRESHOLD")

    left, top, right, bottom = box
    source_pixels = source.load()
    mask_pixels = mask.load()
    rows = [[mask_pixels[x, y] for x in range(left, right)] for y in range(top, bottom)]
    rgb: list[int] = []
    if colour:
        for y in range(top, bottom):
            for x in range(left, right):
                red, green, blue, _ = source_pixels[x, y]
                rgb.append(rgb565(red, green, blue))

    return Layer(left, top, right - left, bottom - top, rgb, pack_alpha(rows))


def format_words(values: list[int], per_line: int, width: int) -> list[str]:
    lines = []
    for i in range(0, len(values), per_line):
        chunk = values[i : i + per_line]
        lines.append("    " + ", ".join(f"0x{value:0{width}X}" for value in chunk) + ",")
    return lines


def emit_rung(rung: int, asset_dir: Path) -> tuple[list[str], int]:
    path = asset_dir / f"logo-on-dark-{rung}.png"
    image = Image.open(path).convert("RGBA")
    bolt, word = split_layers(image)
    name = f"zd_logo_{rung}"

    lines = [f"static const uint16_t {name}_bolt_rgb[] = {{"]
    lines.extend(format_words(bolt.rgb, 12, 4))
    lines.append("};")
    lines.append("")
    lines.append(f"static const uint8_t {name}_bolt_alpha[] = {{")
    lines.extend(format_words(list(bolt.alpha), 16, 2))
    lines.append("};")
    lines.append("")
    lines.append(f"static const uint8_t {name}_word_alpha[] = {{")
    lines.extend(format_words(list(word.alpha), 16, 2))
    lines.append("};")
    lines.append("")
    lines.append(f"const zd_logo_t {name} = {{")
    lines.append(f"    .width = {image.size[0]},")
    lines.append(f"    .height = {image.size[1]},")
    lines.append(f"    .bolt_rgb = {name}_bolt_rgb,")
    lines.append(f"    .bolt_alpha = {name}_bolt_alpha,")
    lines.append(f"    .bolt_x = {bolt.x},")
    lines.append(f"    .bolt_y = {bolt.y},")
    lines.append(f"    .bolt_w = {bolt.width},")
    lines.append(f"    .bolt_h = {bolt.height},")
    lines.append(f"    .word_alpha = {name}_word_alpha,")
    lines.append(f"    .word_x = {word.x},")
    lines.append(f"    .word_y = {word.y},")
    lines.append(f"    .word_w = {word.width},")
    lines.append(f"    .word_h = {word.height},")
    lines.append("};")
    lines.append("")

    size = len(bolt.rgb) * 2 + len(bolt.alpha) + len(word.alpha)
    print(
        f"{name:14s} bolt {bolt.width}x{bolt.height} at ({bolt.x},{bolt.y})  "
        f"word {word.width}x{word.height} at ({word.x},{word.y})  {size:>7,} bytes"
    )
    return lines, size


def main(asset_dir: Path, out_path: Path) -> None:
    parts = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// Copyright (c) 2026 The Zaparoo Project Contributors.",
        "",
        "// Generated by tools/generate_logo.py. Do not edit by hand.",
        "// The Zaparoo logo is a trademark of Wizzo Pty Ltd. See NOTICE.md.",
        '#include "ui/logo.h"',
        "",
    ]

    total = 0
    for rung in RUNGS:
        lines, size = emit_rung(rung, asset_dir)
        parts.extend(lines)
        total += size
    print(f"{'total':14s} {total:>62,} bytes")

    out_path.write_text("\n".join(parts), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--assets",
        type=Path,
        default=DEFAULT_ASSET_DIR,
        help=f"directory holding logo-on-dark-<rung>.png (default: {DEFAULT_ASSET_DIR})",
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
    main(args.assets, args.out)
