#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Discover a zapdisplay USB serial accessory."""

import argparse
import sys

from zapdisplay_client import discover, find_candidate_ports


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        if args.verbose:
            ports = find_candidate_ports()
            print("CANDIDATES " + (" ".join(ports) if ports else "none"))
        found = discover(verbose=args.verbose)
        if found is None:
            print("NOT_FOUND")
            return 1
        client, info = found
        try:
            print(f"FOUND {client.port} {info}")
        finally:
            client.close()
        return 0
    except Exception as exc:  # noqa: BLE001 - CLI should convert unexpected failures to exit code.
        print(f"ERROR {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
