#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Zaparoo Project Contributors.
"""Mirror live Zaparoo Core media state to a connected zapdisplay."""

from __future__ import annotations

import argparse
import base64
import binascii
import contextlib
import hashlib
import json
import sys
import time
import uuid
from dataclasses import dataclass
from datetime import UTC, datetime
from io import BytesIO
from pathlib import Path
from typing import Any

from PIL import Image
from websockets.sync.client import connect as ws_connect
from zapdisplay_client import ZapDisplayClient, ZapDisplayError, discover, fit_cover

IMAGE_TYPES = ["boxart", "image", "screenshot", "titleshot"]
# The device drifts against its own timer, so the wall clock is refreshed
# rather than set once.
CLOCK_PUSH_SECONDS = 3600.0
MEDIA_SYNC_NOTIFICATIONS = {
    "media.started",
    "media.stopped",
    "running",
    "tokens.added",
    "tokens.removed",
    "tokens.staged",
    "tokens.staged.ready",
}


# Catalogue tags Core stores per title, mapped to the device's metadata fields.
# Most systems fill only some of them, which is why the device packs whatever
# arrives rather than reserving a row per field.
META_TAGS = {
    "year": "year",
    "developer": "developer",
    "genre": "genre",
    "gamegenre": "genre",
    "players": "players",
}


@dataclass(frozen=True)
class ActiveMedia:
    key: str
    media_id: int | None
    title: str
    system: str
    path: str
    # Seconds since the launch, from Core's own start time. Sent once; the
    # device counts on from there rather than being told every minute.
    elapsed: int | None = None
    playback_state: str = ""
    year: str = ""
    developer: str = ""
    genre: str = ""
    players: str = ""


@dataclass
class CoverCacheEntry:
    asset_id: str
    data: bytes
    width: int
    height: int


class CoreClient:
    def __init__(self, url: str, *, verbose: bool = False) -> None:
        self.url = url
        self.verbose = verbose
        self.ws = None

    def __enter__(self) -> CoreClient:
        self.open()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def open(self) -> None:
        self.ws = ws_connect(self.url, open_timeout=5, max_size=8 * 1024 * 1024)

    def close(self) -> None:
        if self.ws is not None:
            self.ws.close()
            self.ws = None

    def rpc(self, method: str, params: dict[str, Any] | None = None, *, timeout: float = 15.0) -> Any:
        if self.ws is None:
            raise RuntimeError("Core WebSocket is not connected")
        request_id = str(uuid.uuid4())
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        payload = json.dumps(request, separators=(",", ":"))
        if self.verbose:
            print(f"CORE > {method}")
        self.ws.send(payload)

        deadline = time.time() + timeout
        while time.time() < deadline:
            message = self.ws.recv(timeout=max(0.1, deadline - time.time()))
            response = json.loads(message)
            if response.get("id") != request_id:
                if self.verbose and "method" in response:
                    print(f"CORE < notification {response.get('method')}")
                continue
            if "error" in response:
                raise RuntimeError(f"Core {method} failed: {response['error']}")
            if self.verbose:
                print(f"CORE < {method} ok")
            return response.get("result")
        raise TimeoutError(f"Core {method} timed out")

    def current_media(self) -> ActiveMedia | None:
        result = self.rpc("media")
        active = result.get("active", []) if isinstance(result, dict) else []
        if not active:
            return None
        item = active[0]
        media_id = item.get("mediaId")
        path = item.get("mediaPath") or item.get("relativePath") or ""
        key = str(media_id) if media_id is not None else stable_hash(f"{item.get('systemId', '')}:{path}")
        return ActiveMedia(
            key=key,
            media_id=media_id if isinstance(media_id, int) else None,
            title=item.get("mediaName") or "Unknown Media",
            system=item.get("systemName") or item.get("systemId") or "Unknown System",
            path=path,
            elapsed=elapsed_seconds(item.get("started")),
            playback_state=item.get("playbackState") or "",
        )

    def media_meta(self, media: ActiveMedia) -> dict[str, Any] | None:
        if media.media_id is None:
            return None
        result = self.rpc("media.meta", {"mediaId": media.media_id})
        if not isinstance(result, dict):
            return None
        media_meta = result.get("media")
        return media_meta if isinstance(media_meta, dict) else None

    def media_image(self, media: ActiveMedia) -> bytes | None:
        if media.media_id is None:
            return None
        result = self.rpc("media.image", {"mediaId": media.media_id, "imageTypes": IMAGE_TYPES}, timeout=30.0)
        if not isinstance(result, dict) or not result.get("data"):
            return None
        return base64.b64decode(result["data"])

    def notification(self, timeout: float) -> str | None:
        if self.ws is None:
            raise RuntimeError("Core WebSocket is not connected")
        try:
            message = self.ws.recv(timeout=timeout)
        except TimeoutError:
            return None
        payload = json.loads(message)
        method = payload.get("method")
        if not isinstance(method, str):
            return None
        if self.verbose:
            suffix = " -> sync" if method in MEDIA_SYNC_NOTIFICATIONS else ""
            print(f"CORE < notification {method}{suffix}")
        return method


def normalize_core_url(value: str) -> str:
    if value.startswith("ws://") or value.startswith("wss://"):
        return value
    if value.startswith("http://"):
        return "ws://" + value[len("http://") :].rstrip("/") + "/api/v0.1"
    if value.startswith("https://"):
        return "wss://" + value[len("https://") :].rstrip("/") + "/api/v0.1"
    host = value.rstrip("/")
    if ":" not in host:
        host = f"{host}:7497"
    return f"ws://{host}/api/v0.1"


def elapsed_seconds(started: Any) -> int | None:
    """Seconds since Core recorded the launch.

    Core reports an absolute start time rather than a duration, so the length of
    a session that began before this daemon started is still known.
    """
    if not isinstance(started, str):
        return None
    try:
        when = datetime.fromisoformat(started.replace("Z", "+00:00"))
    except ValueError:
        return None
    if when.tzinfo is None:
        when = when.replace(tzinfo=UTC)
    return max(0, int((datetime.now(UTC) - when).total_seconds()))


def status_for(playback_state: str, *, has_cover: bool) -> str:
    """What the device should say it is doing.

    Core distinguishes paused from playing; a display insisting "Playing" over a
    paused game is simply wrong, and it is the sort of wrong a glanceable panel
    gets blamed for.
    """
    if playback_state == "paused":
        return "Paused"
    if playback_state == "stopped":
        return "Stopped"
    return "Playing" if has_cover else "Playing - no cover"


def stable_hash(value: str) -> str:
    return hashlib.sha1(value.encode("utf-8")).hexdigest()[:12]


def rgb565_bytes(image_bytes: bytes) -> tuple[bytes, int, int]:
    """Scale artwork to the display's box, keeping its own aspect ratio.

    Box art is portrait on some systems and landscape on others. Cropping it to
    one fixed shape threw away most of a wide cover, so it is scaled to fit and
    the display lays out around whatever size comes back.
    """
    with Image.open(BytesIO(image_bytes)) as image:
        width, height = fit_cover(*image.size)
        fitted = image.convert("RGB").resize((width, height), Image.Resampling.LANCZOS)
        output = bytearray()
        get_pixels = getattr(fitted, "get_flattened_data", fitted.getdata)
        for r, g, b in get_pixels():
            color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            output.append(color & 0xFF)
            output.append((color >> 8) & 0xFF)
        return bytes(output), width, height


def connect_display(port: str, verbose: bool) -> tuple[ZapDisplayClient, str]:
    if port == "auto":
        found = discover(verbose=verbose)
        if found is None:
            raise RuntimeError("no zapdisplay device found")
        return found
    client = ZapDisplayClient(port, verbose=verbose)
    client.open(reset=False)
    return client, client.claim()


def apply_idle(display: ZapDisplayClient) -> None:
    display.clear()
    display.set_scene("idle")
    display.set_status("Ready")
    display.show()


def push_clock(display: ZapDisplayClient) -> None:
    """Give the device the wall clock. It has no RTC and forgets this on reset."""
    if not display.supports("clock"):
        return
    now = datetime.now().astimezone()
    offset = int(now.utcoffset().total_seconds()) if now.utcoffset() else 0
    with contextlib.suppress(ZapDisplayError):
        display.set_time(int(now.timestamp()), offset)


def enrich_media(core: CoreClient, media: ActiveMedia) -> ActiveMedia:
    try:
        meta = core.media_meta(media)
    except Exception as exc:  # noqa: BLE001 - metadata is optional for live display.
        if core.verbose:
            print(f"WARN metadata unavailable: {exc}")
        return media
    if not meta:
        return media
    title = meta.get("title", {}) if isinstance(meta.get("title"), dict) else {}
    title_name = title.get("name") if isinstance(title.get("name"), str) else None
    system_obj = title.get("system", {}) if isinstance(title.get("system"), dict) else {}
    system_name = system_obj.get("name") if isinstance(system_obj.get("name"), str) else None
    tags = extract_tags(meta)
    return ActiveMedia(
        key=media.key,
        media_id=media.media_id,
        title=title_name or media.title,
        system=system_name or media.system,
        path=media.path,
        elapsed=media.elapsed,
        playback_state=media.playback_state,
        year=tags.get("year", ""),
        developer=tags.get("developer", ""),
        genre=tags.get("genre", ""),
        players=tags.get("players", ""),
    )


def extract_tags(meta: dict[str, Any]) -> dict[str, str]:
    """Flatten Core's tag list into the fields the device renders.

    Core stores catalogue data as typed tags rather than named columns, so the
    same shape covers year, developer, genre and player count. First value wins;
    a title with three genres gets its primary one rather than a run-on.
    """
    found: dict[str, str] = {}
    for source in (
        meta.get("tags"),
        (meta.get("title") or {}).get("tags") if isinstance(meta.get("title"), dict) else None,
    ):
        if not isinstance(source, list):
            continue
        for tag in source:
            if not isinstance(tag, dict):
                continue
            field = META_TAGS.get(str(tag.get("type", "")).lower())
            value = tag.get("label") or tag.get("tag")
            if field and isinstance(value, str) and value and field not in found:
                found[field] = value
    return found


def upload_cover(
    display: ZapDisplayClient,
    media: ActiveMedia,
    cover_cache: dict[str, CoverCacheEntry],
    data: bytes,
    width: int,
    height: int,
) -> CoverCacheEntry:
    crc = binascii.crc32(data) & 0xFFFFFFFF
    asset_id = f"core-{media.key}-{crc:08x}"[:47]
    display.upload_rgb565_cover(asset_id, data, width, height)
    entry = CoverCacheEntry(asset_id=asset_id, data=data, width=width, height=height)
    cover_cache[media.key] = entry
    return entry


def apply_cached_cover(
    core: CoreClient,
    display: ZapDisplayClient,
    media: ActiveMedia,
    cover_cache: dict[str, CoverCacheEntry],
    entry: CoverCacheEntry,
) -> bool:
    try:
        display.use_cover(entry.asset_id, entry.width, entry.height)
        return True
    except ZapDisplayError as exc:
        if "asset-missing" not in str(exc):
            raise
        if core.verbose:
            print(f"WARN cover asset missing on display, re-uploading {entry.asset_id}")
        try:
            entry = upload_cover(display, media, cover_cache, entry.data, entry.width, entry.height)
            display.use_cover(entry.asset_id, entry.width, entry.height)
            return True
        except Exception as retry_exc:  # noqa: BLE001 - live scene still useful without cover.
            if core.verbose:
                print(f"WARN cover re-upload failed: {retry_exc}")
            return False


def apply_playing(
    core: CoreClient,
    display: ZapDisplayClient,
    media: ActiveMedia,
    cover_cache: dict[str, CoverCacheEntry],
    *,
    refresh_only: bool = False,
) -> None:
    cover_uploaded = False
    entry = cover_cache.get(media.key)
    if entry is None and not refresh_only:
        try:
            image_bytes = core.media_image(media)
            if image_bytes:
                cover, cover_w, cover_h = rgb565_bytes(image_bytes)
                entry = upload_cover(display, media, cover_cache, cover, cover_w, cover_h)
        except Exception as exc:  # noqa: BLE001 - live scene still useful without cover.
            if core.verbose:
                print(f"WARN cover unavailable: {exc}")

    if entry is not None:
        cover_uploaded = apply_cached_cover(core, display, media, cover_cache, entry)

    display.set_scene("playing")
    display.set_title(media.title)
    display.set_system(media.system)
    display.set_status(status_for(media.playback_state, has_cover=cover_uploaded))

    if display.supports("meta"):
        display.set_year(media.year)
        display.set_developer(media.developer)
        display.set_genre(media.genre)
        display.set_players(media.players)
    if display.supports("clock") and media.elapsed is not None:
        display.set_elapsed(media.elapsed)
    display.show()


def sync_display(
    core: CoreClient,
    display: ZapDisplayClient,
    last_key: str | None,
    cover_cache: dict[str, CoverCacheEntry],
    *,
    force: bool = False,
    refresh_only: bool = False,
) -> str:
    media = core.current_media()
    key = media.key if media else "idle"
    if key == last_key and not force:
        return last_key

    if media is None:
        if core.verbose:
            print("STATE idle")
        apply_idle(display)
    else:
        media = enrich_media(core, media)
        if core.verbose:
            action = "refresh" if refresh_only else "playing"
            print(f"STATE {action} {media.system} / {media.title}")
        if not refresh_only:
            display.clear()
        apply_playing(core, display, media, cover_cache, refresh_only=refresh_only)
    return key


# Default meanings for input events the device forwarded rather than handling
# itself. The firmware deliberately has no opinion about these: it reports that
# a key was pressed, and the mapping to something Zaparoo can do lives here,
# where ZapScript already covers every verb worth binding.
DEFAULT_EVENT_BINDINGS = {
    "button 1 click": "**launch.random",
    "button 1 long": "**stop",
}


def load_event_bindings(path: str | None) -> dict[str, str]:
    """Read `<source> <id> <action> = <zapscript>` lines, one per binding."""
    if not path:
        return dict(DEFAULT_EVENT_BINDINGS)
    bindings: dict[str, str] = {}
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        shape, script = line.split("=", 1)
        bindings[" ".join(shape.split())] = script.strip()
    return bindings


def dispatch_events(core: CoreClient, display: ZapDisplayClient, bindings: dict[str, str]) -> None:
    for event in display.poll_events():
        script = bindings.get(event)
        if script is None:
            if core.verbose:
                print(f"EVENT {event} (unbound)")
            continue
        if core.verbose:
            print(f"EVENT {event} -> {script}")
        try:
            core.rpc("run", {"text": script})
        except Exception as exc:  # noqa: BLE001 - a bad binding must not kill the daemon.
            print(f"WARN event {event!r} failed: {exc}")


def announce_scan(core: CoreClient, display: ZapDisplayClient) -> None:
    """Confirm a tap on the panel sitting next to the reader.

    A toast rather than a scene, so the confirmation appears over whatever was
    already showing and disappears again without having to put it back.
    """
    if not display.supports("toast"):
        return
    media = core.current_media()
    with contextlib.suppress(ZapDisplayError):
        display.toast(f"Scanned  {media.title}" if media else "Scanned", seconds=4)


def run(args: argparse.Namespace) -> int:
    core_url = normalize_core_url(args.core)
    display, display_info = connect_display(args.display_port, args.verbose)
    if args.verbose:
        print(f"DISPLAY {display.port} {display_info}")
        print(f"CORE {core_url}")

    last_key: str | None = None
    cover_cache: dict[str, CoverCacheEntry] = {}
    bindings = load_event_bindings(args.event_bindings)
    try:
        with CoreClient(core_url, verbose=args.verbose) as core:
            push_clock(display)
            if display.supports("input"):
                display.enable_events(True)
            last_key = sync_display(core, display, last_key, cover_cache, force=True)
            if args.once:
                return 0

            next_poll = time.monotonic() + args.poll_seconds
            next_refresh = time.monotonic() + args.refresh_seconds
            next_clock = time.monotonic() + CLOCK_PUSH_SECONDS
            while True:
                next_deadline = min(next_poll, next_refresh, next_clock)
                timeout = max(0.1, next_deadline - time.monotonic())
                method = core.notification(timeout)
                now = time.monotonic()

                # Events are read on every wakeup, not on the refresh tick, so a
                # key press is acted on in about as long as it takes to notice.
                dispatch_events(core, display, bindings)

                if method == "tokens.added":
                    announce_scan(core, display)
                if method in MEDIA_SYNC_NOTIFICATIONS:
                    last_key = sync_display(core, display, last_key, cover_cache, force=True)
                    next_poll = now + args.poll_seconds
                    next_refresh = now + args.refresh_seconds
                elif now >= next_clock:
                    # The device has no RTC and drifts against its own timer, so
                    # the clock is re-sent rather than set once.
                    push_clock(display)
                    next_clock = now + CLOCK_PUSH_SECONDS
                elif now >= next_refresh:
                    last_key = sync_display(core, display, last_key, cover_cache, force=True, refresh_only=True)
                    next_refresh = now + args.refresh_seconds
                elif now >= next_poll:
                    last_key = sync_display(core, display, last_key, cover_cache)
                    next_poll = now + args.poll_seconds
    finally:
        display.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", required=True, help="Core host, http(s) URL, or ws(s) URL")
    parser.add_argument("--display-port", default="auto", help="zapdisplay serial port, or auto")
    parser.add_argument("--poll-seconds", type=float, default=2.0, help="fallback poll interval")
    parser.add_argument(
        "--refresh-seconds", type=float, default=120.0, help="playing scene anti-retention refresh interval"
    )
    parser.add_argument(
        "--event-bindings",
        default=None,
        help="file mapping forwarded input events to ZapScript, one '<source> <id> <action> = <script>' per line",
    )
    parser.add_argument("--once", action="store_true", help="update once and exit")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        return run(args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # noqa: BLE001 - script should report concise CLI failure.
        print(f"ERROR {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
