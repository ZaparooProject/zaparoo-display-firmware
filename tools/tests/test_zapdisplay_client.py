# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Protocol conformance tests for the reference zapdisplay host client.

These exercise the framing the firmware actually parses: base64 chunking,
CRC32 over decoded bytes, sequential chunk indexes and field limits. They run
against an in-process device simulator, so no hardware is needed.
"""

from __future__ import annotations

import base64
import binascii
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from zapdisplay_client import (
    COVER_MAX_HEIGHT,
    COVER_MAX_WIDTH,
    ZapDisplayClient,
    ZapDisplayError,
    fit_cover,
    parse_features,
    sanitize_field,
    validate_asset_id,
)

COVER_W, COVER_H = 192, 288
COVER_BYTES = COVER_W * COVER_H * 2

PROTOCOL = "zapdisplay/1"


class FakeSerial:
    """Minimal stand-in for the firmware's line protocol and asset state machine."""

    def __init__(self, *, fail_asset_use: str | None = None) -> None:
        self.is_open = True
        self.out = bytearray()
        self.partial = bytearray()
        self.commands: list[str] = []
        self.awaiting = 0
        self.chunk = bytearray()
        self.next_chunk = 0
        self.asset_id = ""
        self.asset_size = 0
        self.asset_crc = 0
        self.asset_data = bytearray()
        self.asset_done = False
        self.committed_id = ""
        self.fail_asset_use = fail_asset_use
        self.features = "scenes,assets-b64,quiet"

    # -- pyserial surface used by the client --

    def write(self, data: bytes) -> int:
        for byte in data:
            if self.awaiting:
                self.chunk.append(byte)
                self.awaiting -= 1
                if self.awaiting == 0:
                    self._finish_chunk()
                continue
            if byte == 0x0A:
                line = self.partial.decode("utf-8", "replace").rstrip("\r")
                self.partial.clear()
                self._handle(line)
                continue
            self.partial.append(byte)
        return len(data)

    def flush(self) -> None:
        return None

    def close(self) -> None:
        self.is_open = False

    def readline(self) -> bytes:
        index = self.out.find(b"\n")
        if index < 0:
            return b""
        line = bytes(self.out[: index + 1])
        del self.out[: index + 1]
        return line

    # -- device behaviour --

    def _reply(self, text: str) -> None:
        # The firmware writes LF; the USB console turns it into CRLF.
        self.out += (text + "\r\n").encode()

    def _finish_chunk(self) -> None:
        try:
            decoded = base64.b64decode(bytes(self.chunk), validate=True)
        except binascii.Error:
            self._reply("ERR bad-base64")
            return
        finally:
            index = self.next_chunk
            self.chunk = bytearray()
        self.asset_data += decoded
        self.next_chunk += 1
        self._reply(f"OK ASSET_CHUNK {index}")

    def _handle(self, line: str) -> None:
        self.commands.append(line)
        if line == f"HELLO {PROTOCOL}":
            self._reply(f"OK {PROTOCOL}")
        elif line == "INFO":
            self._reply(
                f"INFO {PROTOCOL} fw=0.1.0 display=hd458002c40 visible=320x960 transport=cdc features={self.features}"
            )
        elif line == "PING":
            self._reply("PONG")
        elif line == "READY?":
            self._reply(f"READY {PROTOCOL} fw=0.1.0 display=hd458002c40_320x960")
        elif line == "STATE":
            self._reply("STATE scene=playing view=detail panel=full brightness=100 idle=0 silent=0 retention=0")
        elif line == "SETTINGS":
            self._reply("SETTING brightness 100")
            self._reply("SETTING dim_after 300")
            self._reply("OK")
        elif line.startswith("GET "):
            self._reply(f"SETTING {line.split()[1]} 42")
        elif line in {"QUIET", "VERBOSE", "CLEAR", "SHOW", "EVENTS on", "EVENTS off"} or line.startswith(
            (
                "SCENE ",
                "TITLE ",
                "SYSTEM ",
                "STATUS ",
                "MESSAGE ",
                "ACCENT ",
                "YEAR ",
                "DEVELOPER ",
                "GENRE ",
                "PLAYERS ",
                "VIEW ",
                "ELAPSED",
                "TIME ",
                "TOAST ",
                "INPUT ",
                "BIND ",
                "SET ",
            )
        ):
            self._reply("OK")
        elif line.startswith("ASSET_BEGIN "):
            _, asset_id, _mime, size, crc = line.split()
            self.asset_id = asset_id
            self.asset_size = int(size)
            self.asset_crc = int(crc, 16)
            self.asset_data = bytearray()
            self.asset_done = False
            self.next_chunk = 0
            self._reply("OK ASSET_BEGIN")
        elif line.startswith("ASSET_CHUNK_DATA "):
            _, index, length = line.split()
            if int(index) != self.next_chunk:
                self._reply("ERR bad-chunk-index")
                return
            if int(length) % 4 != 0:
                self._reply("ERR bad-asset-chunk")
                return
            self.awaiting = int(length)
            self._reply("OK ASSET_CHUNK_DATA")
        elif line.startswith("ASSET_END "):
            self._handle_asset_end(line.split(maxsplit=1)[1])
        elif line.startswith("ASSET_USE "):
            self._handle_asset_use(line.split())
        else:
            self._reply("ERR unknown-command")

    def _handle_asset_end(self, asset_id: str) -> None:
        if asset_id != self.asset_id:
            self._reply("ERR bad-asset-id")
        elif len(self.asset_data) != self.asset_size:
            self._reply("ERR bad-asset-size")
        elif binascii.crc32(bytes(self.asset_data)) & 0xFFFFFFFF != self.asset_crc:
            self._reply("ERR crc")
        else:
            self.asset_done = True
            self._reply(f"OK ASSET {asset_id} bytes={self.asset_size} crc={self.asset_crc:08x}")

    def _handle_asset_use(self, parts: list[str]) -> None:
        if self.fail_asset_use:
            reason, self.fail_asset_use = self.fail_asset_use, None
            self._reply(f"ERR {reason}")
            return
        asset_id = parts[2]
        if self.committed_id == asset_id:
            self._reply("OK")
        elif self.asset_done and self.asset_id == asset_id:
            self.committed_id = asset_id
            self._reply("OK")
        else:
            self._reply("ERR asset-missing")


@pytest.fixture
def client() -> ZapDisplayClient:
    c = ZapDisplayClient("/dev/fake")
    c._ser = FakeSerial()
    return c


def device(c: ZapDisplayClient) -> FakeSerial:
    return c._ser


def test_claim_performs_full_handshake(client: ZapDisplayClient) -> None:
    info = client.claim()

    assert info.startswith(f"INFO {PROTOCOL}")
    assert device(client).commands[:3] == [f"HELLO {PROTOCOL}", "INFO", "QUIET"]


def test_cover_upload_framing_matches_firmware(client: ZapDisplayClient) -> None:
    pixels = bytes(i % 256 for i in range(COVER_BYTES))
    assert len(pixels) == COVER_BYTES

    client.upload_rgb565_cover("test-cover", pixels, COVER_W, COVER_H)

    dev = device(client)
    assert bytes(dev.asset_data) == pixels, "device must reassemble the exact pixel buffer"
    assert dev.asset_done
    # The simulator does not advertise raw chunks, so this exercises the base64
    # path: 110592 bytes at 768 decoded per chunk divides exactly into 144.
    assert sum(1 for c in dev.commands if c.startswith("ASSET_CHUNK_DATA ")) == 144
    assert "ASSET_CHUNK_DATA 143 1024" in dev.commands


def test_cover_upload_rejects_wrong_size(client: ZapDisplayClient) -> None:
    with pytest.raises(ValueError, match="exactly"):
        client.upload_rgb565_cover("test-cover", b"\x00" * 10, COVER_W, COVER_H)


def test_use_cover_reports_missing_asset(client: ZapDisplayClient) -> None:
    client._ser = FakeSerial(fail_asset_use="asset-missing")

    with pytest.raises(ZapDisplayError, match="asset-missing"):
        client.use_cover("never-uploaded", COVER_W, COVER_H)


def test_committed_cover_can_be_reselected(client: ZapDisplayClient) -> None:
    pixels = b"\x00" * COVER_BYTES
    client.upload_rgb565_cover("test-cover", pixels, COVER_W, COVER_H)
    client.use_cover("test-cover", COVER_W, COVER_H)

    before = sum(1 for c in device(client).commands if c.startswith("ASSET_BEGIN "))
    client.use_cover("test-cover", COVER_W, COVER_H)
    after = sum(1 for c in device(client).commands if c.startswith("ASSET_BEGIN "))

    assert before == after == 1, "the device keeps the committed cover; re-selecting needs no upload"


def test_scene_and_accent_validation(client: ZapDisplayClient) -> None:
    client.set_scene("playing")
    with pytest.raises(ValueError, match="bad scene"):
        client.set_scene("attract")

    client.set_accent("#00C853")
    with pytest.raises(ValueError, match="#RRGGBB"):
        client.set_accent("green")


def test_commands_must_be_single_line(client: ZapDisplayClient) -> None:
    with pytest.raises(ValueError, match="one line"):
        client.command("TITLE broken\nSHOW")


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        ("Super Metroid", "Super Metroid"),
        ("a" * 100, "a" * 63),
        # The device stores fields as bytes, so an accented title counts double
        # and the cut lands on a character boundary rather than mid-sequence.
        ("\u00e9" * 40, "\u00e9" * 31),
        ("\u30b9" * 40, "\u30b9" * 21),
    ],
)
def test_sanitize_field(value: str, expected: str) -> None:
    assert sanitize_field(value, name="title") == expected


@pytest.mark.parametrize("value", ["a" * 100, "\u00e9" * 40, "\u30b9" * 40, "Pok\u00e9mon " * 12])
def test_sanitize_field_fits_the_device_buffer(value: str) -> None:
    trimmed = sanitize_field(value, name="title")

    encoded = trimmed.encode("utf-8")
    assert len(encoded) <= 63, "must fit the device field without the device having to truncate"
    assert encoded.decode("utf-8") == trimmed, "must not end in a partial code point"


def test_sanitize_field_rejects_control_characters() -> None:
    with pytest.raises(ValueError, match="control characters"):
        sanitize_field("bad\x07title", name="title")


@pytest.mark.parametrize("asset_id", ["a", "core-abc_123.def", "z" * 47])
def test_validate_asset_id_accepts_device_legal_ids(asset_id: str) -> None:
    assert validate_asset_id(asset_id) == asset_id


@pytest.mark.parametrize("asset_id", ["", "z" * 48, "has space", "has/slash", "has:colon"])
def test_validate_asset_id_rejects_illegal_ids(asset_id: str) -> None:
    with pytest.raises(ValueError, match="asset_id"):
        validate_asset_id(asset_id)


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        # Portrait box art fills the height and stays narrow.
        ((234, 320), (211, 288)),
        # N64-style landscape box art fills the height and runs wide.
        ((320, 234), (394, 288)),
        ((640, 480), (384, 288)),
        # Square art.
        ((512, 512), (288, 288)),
        # Wider than the cap: scaled down rather than cropped.
        ((1600, 400), (512, 128)),
        # A huge source still lands inside the box.
        ((4000, 3000), (384, 288)),
    ],
)
def test_fit_cover_preserves_aspect_within_bounds(source: tuple[int, int], expected: tuple[int, int]) -> None:
    result = fit_cover(*source)

    assert result == expected
    width, height = result
    assert width <= COVER_MAX_WIDTH and height <= COVER_MAX_HEIGHT
    source_aspect = source[0] / source[1]
    assert abs(width / height - source_aspect) / source_aspect < 0.01, "aspect must survive the fit"


def test_fit_cover_rejects_empty_source() -> None:
    with pytest.raises(ValueError, match="positive"):
        fit_cover(0, 100)


def test_use_cover_rejects_out_of_bounds(client: ZapDisplayClient) -> None:
    with pytest.raises(ValueError, match="fit within"):
        client.use_cover("test-cover", COVER_MAX_WIDTH + 1, COVER_MAX_HEIGHT)


# -- Capability parsing ------------------------------------------------------


def test_parse_features_reads_the_features_field() -> None:
    info = f"INFO {PROTOCOL} fw=0.1.0 display=hd458002c40 visible=320x960 transport=cdc features=scenes,quiet,input"
    assert parse_features(info) == {"scenes", "quiet", "input"}


def test_parse_features_is_not_fooled_by_other_fields() -> None:
    """A capability token appearing in a display name must not advertise it.

    The device name and firmware version are free-form, so a substring test over
    the whole line can claim a capability the firmware does not implement -- and
    the client would then speak a dialect the device cannot parse.
    """
    info = f"INFO {PROTOCOL} fw=assets-raw display=input-panel visible=320x960 features=scenes,quiet"
    assert parse_features(info) == {"scenes", "quiet"}
    assert "assets-raw" not in parse_features(info)


def test_raw_assets_gate_on_the_advertised_capability(client: ZapDisplayClient) -> None:
    client.info()
    assert client.supports_raw_assets is False

    client.serial.features = "scenes,assets-b64,assets-raw,quiet"
    client.info()
    assert client.supports_raw_assets is True
    assert client.supports("assets-raw")


# -- Unsolicited lines -------------------------------------------------------


def test_heartbeat_does_not_satisfy_a_pending_response(client: ZapDisplayClient) -> None:
    """A READY beacon landing mid-exchange must not be read as the response.

    The device emits READY on its own timer, so it can arrive between a command
    and its reply. Accepting it would return the wrong line and leave the real
    response queued, desynchronising every exchange after it.
    """
    client.serial.out += b"READY zapdisplay/1 fw=0.1.0 display=hd458002c40_320x960\r\n"
    assert client.command("PING", expect="PONG").startswith("PONG")


def test_ready_is_still_matched_when_it_is_what_was_asked_for(client: ZapDisplayClient) -> None:
    client.serial.out += b"READY zapdisplay/1 fw=0.1.0 display=hd458002c40_320x960\r\n"
    assert client.command("READY?", expect="READY").startswith("READY")


def test_events_are_collected_not_mistaken_for_a_response(client: ZapDisplayClient) -> None:
    client.serial.out += b"EVENT button 0 click\r\n"
    client.command("SHOW", expect="OK")
    assert client.poll_events() == ["button 0 click"]
    assert client.poll_events() == []


# -- New commands ------------------------------------------------------------


def test_metadata_and_view_commands_are_framed(client: ZapDisplayClient) -> None:
    client.set_year("1996")
    client.set_developer("Nintendo EAD")
    client.set_genre("Platformer")
    client.set_players("1")
    client.set_view("meta")
    client.set_elapsed(4980)
    client.set_elapsed(None)
    client.set_time(1_700_000_000, 36000)
    client.toast("Scanned", seconds=4)
    assert client.serial.commands == [
        "YEAR 1996",
        "DEVELOPER Nintendo EAD",
        "GENRE Platformer",
        "PLAYERS 1",
        "VIEW meta",
        "ELAPSED 4980",
        "ELAPSED",
        "TIME 1700000000 36000",
        "TOAST 4 Scanned",
    ]


def test_metadata_fields_are_truncated_to_the_device_buffers(client: ZapDisplayClient) -> None:
    client.set_genre("G" * 200)
    assert len(client.serial.commands[-1]) == len("GENRE ") + 31


@pytest.mark.parametrize(
    ("method", "args"),
    [
        ("set_view", ("sideways",)),
        ("send_input", ("mouse", 0, "click")),
        ("send_input", ("button", 0, "wiggle")),
    ],
)
def test_invalid_arguments_are_rejected_before_the_wire(
    client: ZapDisplayClient, method: str, args: tuple[object, ...]
) -> None:
    with pytest.raises(ValueError):
        getattr(client, method)(*args)
    assert client.serial.commands == []


def test_input_and_bind_framing(client: ZapDisplayClient) -> None:
    client.send_input("button", 0, "click")
    client.send_input("encoder", 1, "delta", -2)
    client.bind_input("button", 0, "long", "screen.toggle")
    client.enable_events(True)
    client.enable_events(False)
    assert client.serial.commands == [
        "INPUT button 0 click",
        "INPUT encoder 1 delta -2",
        "BIND button 0 long screen.toggle",
        "EVENTS on",
        "EVENTS off",
    ]


def test_settings_listing_terminates_on_ok(client: ZapDisplayClient) -> None:
    assert client.settings() == {"brightness": 100, "dim_after": 300}


def test_state_is_parsed_into_fields(client: ZapDisplayClient) -> None:
    assert client.state()["view"] == "detail"
    assert client.state()["panel"] == "full"
