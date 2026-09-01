# The zapdisplay protocol

A line protocol over USB serial. The host sends state, metadata and artwork; the
device renders locally.

## Transport

The device uses the ESP32-S3's USB Serial/JTAG peripheral as its console, and
appears as `/dev/ttyACM*` on Linux.

This is the chip's native USB peripheral, not a TinyUSB CDC device, which has
two consequences for host software. Output written as `\n` arrives as `\r\n`, so
hosts must strip the carriage return. Input is line-ending translated: a `0x0D`
byte sent by the host is rewritten to `0x0A` before the firmware sees it, which
is why the firmware sets its receive line endings explicitly; see
`ASSET_CHUNK_RAW` below. The device also enumerates with Espressif's stock
`303a:1001` USB ID and no product string, so the `HELLO` handshake is the only
reliable way to identify it.

## Model

The host sends what to show and the device draws it. Full frames are never
streamed: a single 320x960 RGB565 frame is about 614 KB, which at the link's
measured throughput would take thirteen seconds.

Artwork is sent once and re-selected by ID, so a scene change costs a few short
lines rather than another upload.

## Host discovery

Host software should never assume the first serial port is the display. Use a handshake:

1. Enumerate `/dev/ttyACM*`, then `/dev/ttyUSB*` as fallback.
2. Open each candidate at 115200 baud.
3. Drain boot/log text briefly.
4. Send `HELLO zapdisplay/1`.
5. Require `OK zapdisplay/1`.
6. Send `INFO` and record capabilities.
7. Send `QUIET` to claim a clean command-response session.
8. Keep the port open and reconnect if reads/writes fail.

Boot logs can appear before the session is claimed. Host clients should ignore unrelated lines until the expected `OK`, `INFO`, or `ERR` response appears.

Reference tools:

```sh
python tools/zapdisplay_discover.py
python tools/zapdisplay_demo.py --port auto
python tools/zapdisplay_live_daemon.py --core <core-host> --display-port auto
```

The live daemon talks to Zaparoo Core over plaintext WebSocket JSON-RPC when API encryption is disabled, listens for media and launch-adjacent notifications, polls `media` as a fallback, fetches `media.meta` and `media.image`, scales returned image data to fit the device's artwork bounds while keeping its aspect, and drives the display protocol. It also sends catalogue metadata, the session start time and the wall clock, and maps forwarded input events to ZapScript. During long playing sessions it refreshes the scene every 2 minutes, though the device now advances its own retention offset regardless. If firmware reports `ERR asset-missing` for a cached cover ID, the daemon re-uploads the cached RGB565 cover once and retries.

## Commands

The device prints READY periodically until `QUIET` is received:

```text
READY zapdisplay/1 fw=0.1.0 display=hd458002c40_320x960
```

General commands:

```text
HELLO zapdisplay/1
INFO
PING
READY?
QUIET
VERBOSE
```

Session mode:

- Device starts verbose and advertises `READY` every 5 seconds.
- `QUIET` stops periodic `READY` output and suppresses protocol RX logs, leaving command responses on the serial stream.
- `VERBOSE` re-enables periodic `READY` and protocol RX logs for debugging.

Scene commands update pending state. `SHOW` renders it atomically:

```text
CLEAR
SCENE idle
SCENE launching
SCENE playing
SCENE error
TITLE Super Metroid
SYSTEM Super Nintendo
STATUS Running
MESSAGE Launch failed
ACCENT #00C853
SHOW
```

Catalogue metadata, shown by the `meta` view. Fields the host leaves empty are
skipped rather than drawn as a gap, because most systems fill only some of them:

```text
YEAR 1996
DEVELOPER Nintendo EAD
GENRE Platformer
PLAYERS 1
```

The playing scene has three views. They differ in how much text competes with
the artwork, not in how large the artwork is - the panel's 320px height is the
binding constraint, so even a full-canvas view could only grow a cover by about
a tenth:

```text
VIEW art
VIEW detail
VIEW meta
```

`VIEW` sets what is currently shown. The view the device starts in, and returns
to on `CLEAR`, is the `view` setting.

`ACCENT` accepts any 24-bit colour including `#000000`; the device tracks
whether an accent was set separately from its value.

### Time and transient messages

The device extrapolates from a reference rather than being told a value every
minute. Both survive host quiet periods, and both stop being a reason to talk to
the device at all during a long session:

```text
ELAPSED 4980
ELAPSED
TIME 1700000000 36000
TOAST 4 Scanned  Banjo-Kazooie
```

`ELAPSED <seconds>` sets the session length the device counts on from; `ELAPSED`
with no argument clears it. `TIME <epoch_seconds> <utc_offset_seconds>` sets the
wall clock. There is no RTC, so the clock is unknown until a host sets it and is
forgotten on reset - the idle clock screen is skipped rather than guessed at, and
hosts should re-send periodically to bound drift.

`TOAST <seconds> <text>` shows a banner over whatever scene is current, for up
to 30 seconds. It is an overlay rather than a scene so a token scan does not
throw away what was on screen and needs no state machine to put it back. A toast
counts as activity and wakes a dimmed panel.

### Settings

Settings are device-owned and persist in NVS. The split from scene state is by
owner, not by kind: anything the device must honour with no host attached, and
still honour after a power cycle, lives here. A display plugged into a dead host
still has to come up at the brightness its owner chose.

```text
SET brightness 60
GET brightness
SETTINGS
```

`SET` answers `OK`, `ERR bad-setting` or `ERR bad-value`. `GET` answers
`SETTING <key> <value>`. `SETTINGS` lists every key as `SETTING` lines followed
by `OK`.

| Key | Range | Default | Meaning |
| --- | --- | --- | --- |
| `brightness` | 0-100 | 100 | Backlight level. Perceptual, not raw duty. |
| `dim_level` | 0-100 | 25 | Level to dim to. Never brightens past `brightness`. |
| `dim_after` | 0-86400 | 300 | Seconds of no visible change before dimming. 0 disables. |
| `blank_after` | 0-86400 | 1800 | Seconds before blanking. 0 disables. |
| `host_absent` | 0-86400 | 600 | Seconds of host silence before falling back to idle. 0 disables. |
| `view` | 0-2 | 1 | Starting playing view: art, detail, meta. |
| `idle_screens` | 0-7 | 7 | Bitmask of the idle frame's parts: mark, clock, last played. 0 leaves the mark alone. |
| `rotation` | 0 or 180 | 0 | Turns the UI for a display mounted the other way up. |

`idle_screens` selects which parts of the idle frame may appear, not which of
several screens is due. Idle used to rotate between three compositions and no
longer does; `idle_rotate` is gone with it. Both are protocol-visible changes,
but neither needs a version bump: `SETTINGS` enumerates keys rather than hosts
assuming them, and no host in the pairing reads or writes either one.

`rotation` takes only 0 and 180; any other value answers `ERR bad-value`. The
device has one landscape layout, so 90 and 270 would need a UI it does not have
rather than a transform. It applies immediately and survives a power cycle,
because a display plugged into a dead host still has to come up the right way
up.

`dim_after` counts from the last *visible change*, not the last command. A host
refreshing an unchanging frame every two seconds still lets the panel dim, which
is the behaviour worth having: a machine left on the same game for an hour
should go dark. `host_absent` counts from the last command of any kind, so a
healthy host holding a long session is never mistaken for a dead one.

### Input

One vocabulary, three producers: the host can inject an event, a physical key
raises the same event locally, and a touch panel could later do the same. The
firmware resolves each event to a display-local action or forwards it; it
deliberately knows nothing about what an event *means*.

```text
INPUT button 0 click
INPUT encoder 1 delta -2
BIND button 0 long screen.toggle
EVENTS on
EVENTS off
```

Events are `<source> <id> <action> [value]` where source is `button`, `encoder`
or `touch`, and action is `down`, `up`, `click`, `double`, `long`, `delta` or
`tap`. Bindable actions are `forward`, `none`, `wake`, `view.next`, `view.prev`,
`brightness.cycle` and `screen.toggle`. Only button events are bindable, and
bindings persist in NVS. Defaults are `button 0 click -> view.next` and
`button 0 long -> screen.toggle`; everything else forwards.

An event that wakes a dimmed or blanked panel is consumed rather than also
acting - the press that wakes a screen should not do something as well.

Events the device does not handle itself are reported to the host, but only
after `EVENTS on`:

```text
EVENT button 1 click
```

`EVENT` lines are unsolicited. They are off by default because every existing
host parses responses by prefix, so an unexpected line is at best discarded and
at worst mistaken for a response. They are emitted only between commands, never
inside the framed windows where the device is reading raw payload bytes, and
they never begin with `ERR`. Hosts that opt in must skip both `EVENT ` and
`READY ` lines when matching a response.

### Diagnostics

`STATE` reports what the device is actually doing, as opposed to what it was
told. The panel's own behaviour - dimming, blanking, stepping the retention
offset, rotating idle screens - is otherwise invisible to a host and untestable
without watching the screen:

```text
STATE scene=playing view=detail panel=full brightness=100 idle=12 silent=0 retention=2 idle_elements=7 elapsed=4980 clock=875 toast=0
```

`elapsed` is -1 when unset, `clock` is -1 when no host has set the time.

`ACCENT` overrides the accent role only. The device ships the Zaparoo palette
and derives its own backgrounds, borders and text colours from it,
so it does not repaint scene chrome. It tints the elements the design draws in
the accent, such as the launching scene's progress track. An accent too dark to
read against the page is ignored in favour of the palette's own.

The launching scene's track is indeterminate: one cell is lit and marches across
it, wrapping. The device is told a launch has started, never how far along it
is, so there is no progress to report and no command to report it with. A host
with something to say about a launch should say it in `STATUS` or `MESSAGE`.

Diagnostic benchmark command reads exactly N payload bytes and reports device-side receive rate:

```text
BENCH_BEGIN 65536
<payload bytes>
BENCH bytes=65536 ms=1055 rate=62126Bps checksum=00565c00
```

Asset upload has two chunk forms. The firmware sets the console's receive line
endings to LF so bytes pass through unmodified, which makes the raw form safe;
base64 remains for hosts talking to firmware that predates it. A device
advertises `assets-raw` in its `INFO` feature list when raw chunks are
available, and the reference client picks the raw path automatically.

Raw chunks are read straight into the asset buffer, so there is no size limit
beyond the asset itself:

```text
ASSET_BEGIN test-cover image/rgb565 226944 b1314226
ASSET_CHUNK_RAW 0 32768
<32768 payload bytes>
ASSET_CHUNK_RAW 1 32768
<32768 payload bytes>
ASSET_END test-cover
```

Base64 chunking is the fallback:

Preferred chunk form is length-framed, so payload bytes are read directly instead of going through line buffering:

```text
ASSET_BEGIN test-cover image/jpeg 4096 b1314226
ASSET_CHUNK_DATA 0 1024
<1024 base64 bytes>
ASSET_CHUNK_DATA 1 1024
<1024 base64 bytes>
ASSET_END test-cover
```

Legacy inline chunks also work for short payloads:

```text
ASSET_CHUNK 0 CxwtPk9gcYKTpLXG1+j5ChssPU5fcIGSo7TF1uf4CRorPE1e
```

Asset responses:

```text
OK ASSET_BEGIN
OK ASSET_CHUNK_DATA
OK ASSET_CHUNK_RAW
OK ASSET_CHUNK 0
OK ASSET test-cover bytes=4096 crc=b1314226
ERR crc
ERR overflow
ERR bad-base64
ERR bad-state
ERR timeout
```

Current limits:

- decoded asset max: 294,912 bytes, which is a 512x288 RGB565 cover
- storage: PSRAM only
- one upload in flight, plus one committed cover
- chunk indexes must be sequential
- CRC32 is over decoded bytes
- base64 chunk buffer holds 1024 encoded bytes, which is 768 decoded
- raw chunks have no size limit; the reference client sends 32 KB, past which
  throughput gains fall under 1%
- legacy inline `ASSET_CHUNK` lines must stay short for ESP console stdin; previous tests used 30-36 decoded bytes per chunk

After `ASSET_BEGIN` or `ASSET_CHUNK_DATA` the device reads the announced payload
bytes directly. It gives up after 5 seconds without progress and answers
`ERR timeout`, so a host that dies mid-transfer does not leave the device stuck.

Command lines are limited to 519 bytes. A longer line is discarded up to the
next newline and answered once with `ERR line-too-long`, rather than having its
tail parsed as further commands.

Uploaded RGB565 covers can be selected for the playing scene:

```text
ASSET_USE cover banjo-kazooie 394 288 rgb565
SCENE playing
TITLE Banjo-Kazooie
SYSTEM Nintendo 64
STATUS Playing
SHOW
```

Cover artwork is not a fixed slot. Box art is portrait on some systems and
landscape on others, so a host sends whatever aspect ratio it has and the
playing scene lays out around it: the artwork fills the full height of the safe
area and the detail column takes the width that is left. Artwork too wide for
that is scaled down and centred rather than cropped.

Bounds are 512x288, which is 294,912 bytes at RGB565 and also the asset size
limit. Measured on the reference board, a raw upload runs at about 47 KB/s, so
the largest possible cover takes around 6 seconds and a portrait one around 2.5.
Hosts should still send a cover once and re-select it with `ASSET_USE` rather
than re-uploading on every scene change.

`ASSET_USE` commits the completed upload into a cover slot that the renderer
owns. The committed cover survives `CLEAR` and survives a later upload, so a
host can re-select a cover it has already sent by issuing `ASSET_USE` with the
same asset ID and no upload at all. `ERR asset-missing` means the device does
not have that asset, normally because it rebooted, so the correct response is to
upload it again.

Responses:

```text
OK zapdisplay/1
INFO zapdisplay/1 fw=0.1.0 display=hd458002c40 visible=320x960 transport=cdc features=scenes,assets-b64,assets-raw,quiet,settings,views,meta,toast,clock,input
SETTING brightness 100
STATE scene=idle view=detail panel=full brightness=100 idle=0 silent=0 retention=0 idle_elements=7 elapsed=-1 clock=-1 toast=0
EVENT button 1 click
PONG
READY zapdisplay/1 fw=0.1.0 display=hd458002c40_320x960
OK
ERR <reason>
```

Unknown commands return:

```text
ERR unknown-command
```

Full error vocabulary: `unknown-command`, `line-too-long`, `bad-scene`,
`bad-color`, `render-failed`, `bad-bench-size`, `bad-asset-begin`,
`bad-asset-chunk`, `bad-chunk-index`, `bad-base64`, `bad-state`, `timeout`,
`overflow`, `no-memory`, `bad-asset-id`, `bad-asset-size`, `crc`,
`bad-asset-use`, `asset-missing`, `bad-asset-mime`, `bad-setting`, `bad-value`,
`bad-view`, `bad-elapsed`, `bad-time`, `bad-toast`, `bad-input`, `bad-bind`.

Hosts should parse the `features=` field rather than substring-matching the
whole `INFO` line: the display name and firmware version are free-form, so a
capability token appearing in either would otherwise advertise a capability the
device does not have.

## Panel care

The panel is an IPS TFT, not an OLED, so the risk is transient image retention
rather than permanent emitter wear. Every scene applies a small foreground
offset that steps every two minutes, on a counter the device advances itself. No
host cooperation is required.

The settings above matter more: the device dims, then blanks, then falls back to
the idle scene when a host goes silent. That last one covers the case that can
actually mark an LCD, which is a session frame left at full brightness because
the host died and nothing noticed.

## USB transfer measurements

Raw console throughput, measured with `BENCH_BEGIN` on the reference board over
the ESP32-S3 USB Serial/JTAG console. This is what the link can carry, before
any per-chunk protocol overhead:

| Payload | Device time | Device rate |
| --- | ---: | ---: |
| 16 KB | 264 ms | 62,133 B/s |
| 32 KB | 528 ms | 62,060 B/s |
| 64 KB | 1,055 ms | 62,126 B/s |
| 128 KB | 2,111 ms | 62,102 B/s |
| 256 KB | 4,217 ms | 62,164 B/s |

End-to-end cover upload rates, including chunk headers and per-chunk
acknowledgements:

| Form | Chunk size | Rate |
| --- | ---: | ---: |
| base64 | 384 B | 12.4 KB/s |
| base64 | 768 B | 18.6 KB/s |
| raw | 4 KB | 39.8 KB/s |
| raw | 16 KB | 45.4 KB/s |
| raw | 32 KB | 46.8 KB/s |
| raw | 64 KB | 47.2 KB/s |

Raw transfer is throughput-bound rather than round-trip-bound past about 32 KB,
which is why the reference client stops there.
