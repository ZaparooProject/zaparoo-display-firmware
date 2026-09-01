#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Host-side zapdisplay protocol client reference."""

from __future__ import annotations

import base64
import binascii
import glob
import re
import time
from collections.abc import Iterable
from dataclasses import dataclass

import serial

BAUDRATE = 115200
# Artwork bounds, mirroring main/ui/state.h. Art is not a fixed slot: it fills
# the full height of the panel's safe area and takes whatever width its own
# aspect calls for, capped so the detail column stays readable.
COVER_MAX_WIDTH = 512
COVER_MAX_HEIGHT = 288
COVER_MAX_BYTES = COVER_MAX_WIDTH * COVER_MAX_HEIGHT * 2


def fit_cover(width: int, height: int) -> tuple[int, int]:
    """Scale a source image's dimensions into the display's artwork box.

    Height is filled first, because the panel is short and wide and vertical
    space is what artwork is starved of. Anything that would then be too wide
    for the detail column is scaled down instead of being cropped.
    """
    if width <= 0 or height <= 0:
        raise ValueError("cover dimensions must be positive")
    scaled_w = max(1, round(COVER_MAX_HEIGHT * width / height))
    if scaled_w <= COVER_MAX_WIDTH:
        return scaled_w, COVER_MAX_HEIGHT
    return COVER_MAX_WIDTH, max(1, round(COVER_MAX_WIDTH * height / width))


# Base64 chunk size, bounded by the device's decode buffer.
CHUNK_DECODED_BYTES = 768
# Raw chunks are read straight into the asset buffer, so the device imposes no
# size limit and this is chosen purely to amortise the per-chunk round trip.
# Measured on the reference board, throughput flattens around here: 4 KB chunks
# manage 40 KB/s, 32 KB reach 47 KB/s, and 64 KB add under 1% more.
CHUNK_RAW_BYTES = 32768
_FIELD_MAX = 63
_INFO_PREFIX = "INFO zapdisplay/1"
_HELLO_OK = "OK zapdisplay/1"
# Lines the device may send without being asked. They are skipped rather than
# mistaken for a response.
_UNSOLICITED_PREFIXES = ("READY ", "EVENT ")

VIEWS = ("art", "detail", "meta")
SCENES = ("idle", "launching", "playing", "error")
INPUT_SOURCES = ("button", "encoder", "touch")
INPUT_ACTIONS = ("down", "up", "click", "double", "long", "delta", "tap")


def _matches(line: str, expected: str | tuple[str, ...]) -> bool:
    if isinstance(expected, str):
        return line == expected or line.startswith(expected)
    return any(line == item or line.startswith(item) for item in expected)


def parse_features(info: str) -> frozenset[str]:
    """Pull the capability tokens out of an INFO line.

    Parsed rather than substring-matched: `features=` is one field among
    several, and a display name or firmware version containing a capability
    token would otherwise advertise a capability the device does not have.
    """
    for field in info.split():
        if field.startswith("features="):
            return frozenset(token for token in field[len("features=") :].split(",") if token)
    return frozenset()


class ZapDisplayError(RuntimeError):
    """Base zapdisplay client error."""


class ZapDisplayTimeoutError(ZapDisplayError):
    """Raised when expected protocol response does not arrive."""


@dataclass
class DiscoveryResult:
    port: str
    info: str


def find_candidate_ports() -> list[str]:
    """Return deterministic serial candidates, preferring USB CDC ACM ports."""

    acm = sorted(glob.glob("/dev/ttyACM*"))
    usb = sorted(glob.glob("/dev/ttyUSB*"))
    return acm + [port for port in usb if port not in acm]


def _is_control_char(char: str) -> bool:
    return ord(char) < 32 or ord(char) == 127


def sanitize_field(value: str, *, name: str, max_length: int = _FIELD_MAX) -> str:
    """Trim a field to the byte budget the device stores it in.

    The device's fields are byte buffers, so the limit is bytes rather than
    characters: an accented title counts double. Truncation lands on a character
    boundary so the text never ends in half a code point.
    """
    if any(_is_control_char(char) for char in value):
        raise ValueError(f"{name} contains control characters")
    encoded = value.encode("utf-8")
    if len(encoded) <= max_length:
        return value
    return encoded[:max_length].decode("utf-8", "ignore")


def validate_asset_id(asset_id: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,47}", asset_id):
        raise ValueError("asset_id must be 1-47 chars: A-Z a-z 0-9 _ . -")
    return asset_id


class ZapDisplayClient:
    def __init__(
        self,
        port: str,
        *,
        baudrate: int = BAUDRATE,
        timeout: float = 0.25,
        write_timeout: float = 10.0,
        verbose: bool = False,
    ) -> None:
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.write_timeout = write_timeout
        self.verbose = verbose
        # Set from the INFO capability list; base64 is the conservative default
        # for firmware that predates the binary-clean console.
        self.supports_raw_assets = False
        self.features: frozenset[str] = frozenset()
        # Unsolicited EVENT lines seen while reading responses. Collected rather
        # than discarded so a caller polling between commands still sees them
        # without needing a reader thread.
        self.events: list[str] = []
        self._ser: serial.Serial | None = None

    def __enter__(self) -> ZapDisplayClient:
        self.open()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def open(self, *, reset: bool = False, settle_seconds: float = 0.2) -> None:
        if self._ser is not None and self._ser.is_open:
            return
        self._ser = serial.Serial(
            self.port,
            self.baudrate,
            timeout=self.timeout,
            write_timeout=self.write_timeout,
        )
        if reset:
            self._ser.setDTR(False)
            self._ser.setRTS(True)
            time.sleep(0.05)
            self._ser.setRTS(False)
            time.sleep(max(settle_seconds, 2.0))
        elif settle_seconds > 0:
            time.sleep(settle_seconds)
        self.drain(0.15)

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    @property
    def serial(self) -> serial.Serial:
        if self._ser is None or not self._ser.is_open:
            raise ZapDisplayError(f"serial port is not open: {self.port}")
        return self._ser

    def drain(self, seconds: float = 0.2) -> list[str]:
        lines: list[str] = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            line = self._read_line_once()
            if line is None:
                continue
            lines.append(line)
            self._log(f"< {line}")
        return lines

    def command(
        self,
        text: str,
        *,
        expect: str | Iterable[str] | None = None,
        timeout: float = 5.0,
    ) -> str:
        if "\n" in text or "\r" in text:
            raise ValueError("command must be one line")
        self._log(f"> {text}")
        self.serial.write((text + "\n").encode("utf-8"))
        self.serial.flush()
        return self._read_expected(text, expect=expect, timeout=timeout)

    def hello(self) -> None:
        self.command("HELLO zapdisplay/1", expect=_HELLO_OK)

    def info(self) -> str:
        info = self.command("INFO", expect=_INFO_PREFIX)
        # Recorded here rather than in claim() so every path that reads INFO --
        # including discover(), which runs its own handshake -- picks up the
        # device's capabilities.
        self.features = parse_features(info)
        self.supports_raw_assets = "assets-raw" in self.features
        return info

    def supports(self, feature: str) -> bool:
        return feature in self.features

    def quiet(self) -> None:
        self.command("QUIET", expect="OK")

    def verbose_mode(self) -> None:
        self.command("VERBOSE", expect="OK")

    def ping(self) -> None:
        self.command("PING", expect="PONG")

    def clear(self) -> None:
        self.command("CLEAR", expect="OK")

    def show(self) -> None:
        self.command("SHOW", expect="OK")

    def set_scene(self, scene: str) -> None:
        if scene not in {"idle", "launching", "playing", "error"}:
            raise ValueError(f"bad scene: {scene}")
        self.command(f"SCENE {scene}", expect="OK")

    def set_title(self, text: str) -> None:
        self.command(f"TITLE {sanitize_field(text, name='title')}", expect="OK")

    def set_system(self, text: str) -> None:
        self.command(f"SYSTEM {sanitize_field(text, name='system')}", expect="OK")

    def set_status(self, text: str) -> None:
        self.command(f"STATUS {sanitize_field(text, name='status')}", expect="OK")

    def set_message(self, text: str) -> None:
        self.command(f"MESSAGE {sanitize_field(text, name='message')}", expect="OK")

    def set_year(self, text: str) -> None:
        self.command(f"YEAR {sanitize_field(text, name='year', max_length=7)}", expect="OK")

    def set_developer(self, text: str) -> None:
        self.command(f"DEVELOPER {sanitize_field(text, name='developer', max_length=47)}", expect="OK")

    def set_genre(self, text: str) -> None:
        self.command(f"GENRE {sanitize_field(text, name='genre', max_length=31)}", expect="OK")

    def set_players(self, text: str) -> None:
        self.command(f"PLAYERS {sanitize_field(text, name='players', max_length=15)}", expect="OK")

    def set_view(self, view: str) -> None:
        if view not in VIEWS:
            raise ValueError(f"bad view: {view}")
        self.command(f"VIEW {view}", expect="OK")

    def set_elapsed(self, seconds: int | None) -> None:
        """Set the session length the device counts on from, or clear it."""
        if seconds is None:
            self.command("ELAPSED", expect="OK")
            return
        if seconds < 0:
            raise ValueError("elapsed must not be negative")
        self.command(f"ELAPSED {int(seconds)}", expect="OK")

    def set_time(self, epoch_seconds: int, utc_offset_seconds: int) -> None:
        """Give the device a wall clock. It has no RTC and forgets this on reset."""
        self.command(f"TIME {int(epoch_seconds)} {int(utc_offset_seconds)}", expect="OK")

    def toast(self, text: str, seconds: int = 4) -> None:
        if seconds <= 0:
            raise ValueError("toast duration must be positive")
        self.command(f"TOAST {int(seconds)} {sanitize_field(text, name='toast')}", expect="OK")

    def send_input(self, source: str, ident: int, action: str, value: int | None = None) -> None:
        """Inject an input event, exactly as a physical button would raise it."""
        if source not in INPUT_SOURCES:
            raise ValueError(f"bad input source: {source}")
        if action not in INPUT_ACTIONS:
            raise ValueError(f"bad input action: {action}")
        suffix = "" if value is None else f" {int(value)}"
        self.command(f"INPUT {source} {int(ident)} {action}{suffix}", expect="OK")

    def bind_input(self, source: str, ident: int, action: str, binding: str) -> None:
        if source not in INPUT_SOURCES:
            raise ValueError(f"bad input source: {source}")
        if action not in INPUT_ACTIONS:
            raise ValueError(f"bad input action: {action}")
        self.command(f"BIND {source} {int(ident)} {action} {binding}", expect="OK")

    def enable_events(self, enabled: bool = True) -> None:
        """Ask the device to report input events it did not handle itself."""
        self.command(f"EVENTS {'on' if enabled else 'off'}", expect="OK")

    def set_setting(self, key: str, value: int) -> None:
        self.command(f"SET {key} {int(value)}", expect="OK")

    def get_setting(self, key: str) -> int:
        return int(self.command(f"GET {key}", expect="SETTING").split()[2])

    def settings(self, timeout: float = 3.0) -> dict[str, int]:
        """Read every setting. Terminated by OK rather than by a known count."""
        self.serial.write(b"SETTINGS\n")
        self.serial.flush()
        values: dict[str, int] = {}
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._read_line_once()
            if line is None:
                continue
            self._log(f"< {line}")
            if line == "OK":
                return values
            if line.startswith("ERR"):
                raise ZapDisplayError(f"{self.port} SETTINGS failed: {line}")
            if line.startswith("SETTING "):
                _, key, value = line.split(maxsplit=2)
                values[key] = int(value)
        raise ZapDisplayTimeoutError(f"{self.port} timed out reading SETTINGS")

    def state(self) -> dict[str, str]:
        """What the device is actually doing, as opposed to what it was told."""
        line = self.command("STATE", expect="STATE")
        return dict(field.split("=", 1) for field in line.split()[1:] if "=" in field)

    def poll_events(self) -> list[str]:
        """Drain input events the device reported. Requires enable_events()."""
        while True:
            line = self._read_line_once()
            if line is None:
                break
            self._log(f"< {line}")
            if line.startswith("EVENT "):
                self.events.append(line[len("EVENT ") :])
        drained, self.events = self.events, []
        return drained

    def set_accent(self, hex_rgb: str) -> None:
        """Override the accent role only.

        The device ships the Zaparoo palette and derives its own surfaces, so
        this no longer repaints scene chrome. Near-black accents are ignored by
        the device because they would not read against the page.
        """
        if not re.fullmatch(r"#[0-9A-Fa-f]{6}", hex_rgb):
            raise ValueError("accent must be #RRGGBB")
        self.command(f"ACCENT {hex_rgb}", expect="OK")

    def _upload_raw_chunks(self, data: bytes) -> None:
        for index, offset in enumerate(range(0, len(data), CHUNK_RAW_BYTES)):
            payload = data[offset : offset + CHUNK_RAW_BYTES]
            self.command(f"ASSET_CHUNK_RAW {index} {len(payload)}", expect="OK ASSET_CHUNK_RAW")
            self._log(f"> <{len(payload)} raw bytes>")
            self.serial.write(payload)
            self.serial.flush()
            self._read_expected(f"ASSET_CHUNK_RAW {index}", expect=f"OK ASSET_CHUNK {index}", timeout=15.0)

    def _upload_base64_chunks(self, data: bytes) -> None:
        for index, offset in enumerate(range(0, len(data), CHUNK_DECODED_BYTES)):
            encoded = base64.b64encode(data[offset : offset + CHUNK_DECODED_BYTES])
            self.command(f"ASSET_CHUNK_DATA {index} {len(encoded)}", expect="OK ASSET_CHUNK_DATA")
            self._log(f"> <{len(encoded)} base64 bytes>")
            self.serial.write(encoded)
            self.serial.flush()
            self._read_expected(f"ASSET_CHUNK_DATA {index}", expect=f"OK ASSET_CHUNK {index}", timeout=8.0)

    def upload_rgb565_cover(self, asset_id: str, data: bytes, width: int, height: int) -> None:
        asset_id = validate_asset_id(asset_id)
        if width <= 0 or height <= 0 or width > COVER_MAX_WIDTH or height > COVER_MAX_HEIGHT:
            raise ValueError(f"cover must fit within {COVER_MAX_WIDTH}x{COVER_MAX_HEIGHT}")
        if len(data) != width * height * 2:
            raise ValueError(f"cover must be exactly {width * height * 2} bytes for {width}x{height}")

        crc = binascii.crc32(data) & 0xFFFFFFFF
        self.command(
            f"ASSET_BEGIN {asset_id} image/rgb565 {len(data)} {crc:08x}",
            expect="OK ASSET_BEGIN",
        )

        if self.supports_raw_assets:
            self._upload_raw_chunks(data)
        else:
            self._upload_base64_chunks(data)

        self.command(
            f"ASSET_END {asset_id}",
            expect=f"OK ASSET {asset_id} bytes={len(data)} crc={crc:08x}",
            timeout=8.0,
        )

    def use_cover(self, asset_id: str, width: int, height: int) -> None:
        asset_id = validate_asset_id(asset_id)
        if width <= 0 or height <= 0 or width > COVER_MAX_WIDTH or height > COVER_MAX_HEIGHT:
            raise ValueError(f"cover must fit within {COVER_MAX_WIDTH}x{COVER_MAX_HEIGHT}")
        self.command(f"ASSET_USE cover {asset_id} {width} {height} rgb565", expect="OK")

    def claim(self) -> str:
        self.hello()
        info = self.info()
        self.quiet()
        return info

    def _read_line_once(self) -> str | None:
        raw = self.serial.readline()
        if not raw:
            return None
        return raw.decode("utf-8", errors="replace").strip()

    def _read_expected(
        self,
        command: str,
        *,
        expect: str | Iterable[str] | None,
        timeout: float,
    ) -> str:
        expected = tuple(expect) if expect is not None and not isinstance(expect, str) else expect
        deadline = time.time() + timeout
        seen: list[str] = []
        while time.time() < deadline:
            line = self._read_line_once()
            if line is None:
                continue
            self._log(f"< {line}")

            # An explicit expectation wins outright, so `READY?` can still be
            # answered by a READY line even though READY is also a heartbeat.
            if expected is not None and _matches(line, expected):
                return line

            # Otherwise an unsolicited line is never the answer to a command.
            # Letting one satisfy a pending request desynchronises every
            # exchange after it by one.
            if line.startswith(_UNSOLICITED_PREFIXES):
                if line.startswith("EVENT "):
                    self.events.append(line[len("EVENT ") :])
                continue

            seen.append(line)
            if line.startswith("ERR"):
                raise ZapDisplayError(f"{self.port} {command!r} failed: {line}")
            if expected is None and line.startswith(("OK", "INFO", "PONG")):
                return line
        tail = "; ".join(seen[-5:]) if seen else "no response"
        raise ZapDisplayTimeoutError(f"{self.port} timed out waiting for {expect!r} after {command!r}; saw {tail}")

    def _log(self, text: str) -> None:
        if self.verbose:
            print(text)


def discover(timeout: float = 2.0, *, verbose: bool = False) -> tuple[ZapDisplayClient, str] | None:
    for port in find_candidate_ports():
        client = ZapDisplayClient(port, verbose=verbose)
        try:
            client.open(settle_seconds=0.1)
            client.drain(0.2)
            client.hello()
            info = client.info()
            client.quiet()
            return client, info
        except (OSError, serial.SerialException, ZapDisplayError):
            client.close()
            continue
    return None
