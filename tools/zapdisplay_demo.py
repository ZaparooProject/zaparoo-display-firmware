#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Drive a Zaparoo display accessory over USB serial."""

import argparse
import struct

from zapdisplay_client import COVER_MAX_HEIGHT, ZapDisplayClient, discover, fit_cover


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def gradient_cover(width: int, height: int) -> bytes:
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            color = rgb565((x * 255) // max(1, width - 1), (y * 255) // max(1, height - 1), 180)
            pixels += struct.pack("<H", color)
    return bytes(pixels)


def connect(port: str, verbose: bool) -> tuple[ZapDisplayClient, str]:
    if port == "auto":
        found = discover(verbose=verbose)
        if found is None:
            raise RuntimeError("no zapdisplay device found")
        return found

    client = ZapDisplayClient(port, verbose=verbose)
    client.open(reset=True)
    info = client.claim()
    return client, info


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="auto", help="serial port, or auto")
    parser.add_argument("--title", default="Super Metroid")
    parser.add_argument("--system", default="Super Nintendo")
    parser.add_argument("--status", default="Host demo")
    parser.add_argument(
        "--aspect",
        type=float,
        default=2 / 3,
        help="artwork aspect ratio to simulate, width over height (default: 2/3 portrait box art)",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    width, height = fit_cover(round(COVER_MAX_HEIGHT * args.aspect), COVER_MAX_HEIGHT)
    cover = gradient_cover(width, height)
    client, info = connect(args.port, args.verbose)
    try:
        if args.verbose:
            print(info)
        client.upload_rgb565_cover("demo-cover", cover, width, height)
        client.use_cover("demo-cover", width, height)
        client.set_scene("playing")
        client.set_title(args.title)
        client.set_system(args.system)
        client.set_status(args.status)
        client.show()
    finally:
        client.close()

    print(f"DEMO_OK cover={width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
