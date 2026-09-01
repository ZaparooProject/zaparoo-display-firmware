# Agent Instructions

## Repository purpose

- ESP-IDF C firmware for a Zaparoo display accessory: an ESP32-S3 driving a small TFT panel that shows what Zaparoo Core is playing.
- Public interfaces are the `zapdisplay/1` serial line protocol, the NVS setting names, and the reference host client. Zaparoo Core ships a `zapdisplay` reader driver that speaks the same protocol, so both ends move together.
- Read `docs/protocol.md` before touching the wire format and `docs/hardware.md` before touching the panel or board.

## Commands

ESP-IDF v6.0.1 must be activated first, however your installation provides it.
The target is pinned in `CMakeLists.txt`, so a fresh checkout needs no
`set-target` step.

- Build: `idf.py build`
- Flash and watch: `idf.py -p <port> flash monitor`
- Scene layout tests, no hardware needed: `make -C tests/render test`
- Host tool tests: `python -m pytest tools/tests -q`
- Host tool lint: `ruff check tools && ruff format --check tools`
- Firmware formatting: `find main tests -name '*.c' -o -name '*.h' | grep -v generated_ | xargs clang-format --dry-run --Werror`
- Regenerate baked assets: `python tools/generate_font.py`, `python tools/generate_logo.py`

## Testing

- Scenes are pure functions of `zapdisplay_state_t`. Keep them that way: it is what lets `tests/render/` build the real canvas and scene code for the host and check layout in pixels. A scene that reads a clock, a setting or a driver breaks this.
- Add tests for behaviour changes. Prefer a regression test that fails before the fix.
- Assert properties that survive a redesign, not golden images. Edge clearance, containment, and fallback behaviour are worth pinning; exact pixels are not.
- Host protocol tests run against an in-process fake device in `tools/tests/`. Extend it rather than building a parallel one.
- Nothing in CI may need a board attached.

## Code and compatibility rules

- C only. Do not add Arduino, PlatformIO, Rust, a UI toolkit, or a build wrapper without discussion.
- The protocol is a compatibility contract with devices that cannot be updated in step. New commands are additive and need no version bump; changing or removing one means bumping `ZAPDISPLAY_PROTOCOL` and updating `docs/protocol.md` and Core's driver together.
- Device→host lines other than a command response must be opt-in, must not begin with `ERR`, and must only be emitted between commands. Hosts match responses by prefix, so an unsolicited line otherwise desynchronises the session.
- The firmware is deliberately single-threaded. The protocol loop owns the pending state, the asset buffer and the framebuffer. Put periodic work in the tick rather than adding a task or timer.
- Keep the layers apart: `main/ui/` knows nothing about the panel, and `main/protocol/` knows nothing about either. Only the display driver knows about rotation and overscan.
- Colours, spacing and radii come from `main/ui/theme.h`. No literals at the call site.
- Settings the device must honour with no host attached belong in NVS; anything describing what is currently on screen stays host-pushed and is forgotten on reset.
- Treat everything from the host as untrusted: fixed-size fields, bounded reads, and a timeout on any framed payload.
- Do not hand-edit `main/ui/generated_*.c`; change the generator and regenerate.

## High-risk areas

- Protocol framing: line reassembly, the raw payload read windows, and the asset upload state machine. A host that dies mid-transfer must not wedge the device.
- Asset ownership: the committed cover slot is deliberately separate from the upload buffer, so an in-flight upload can never free pixels a later render dereferences.
- Panel bring-up: the ST7701S init sequence, RGB timing, and the framebuffer geometry. Changes here can only be confirmed on hardware.
- Backlight duty: below a floor the boost converter stops regulating and the panel flickers. Do not lower it without measuring.
- NVS layout: setting names are the wire names. Renaming one is a protocol change and a migration.
- `tools/assets/` and `NOTICE.md` carry trademarked artwork and the terms it is included under.

## Specialized validation

- Scene or layout changes: `make -C tests/render test`.
- Protocol changes: host tests, `docs/protocol.md`, and Core's `zapdisplay` driver, which will otherwise break against the new firmware.
- Panel, timing, backlight or input changes: verify on hardware. There is no substitute, and the render tests do not cover any of it.
- Workflow changes: `actionlint`. Actions are pinned by commit SHA; keep new ones pinned.
- Flash budget: `idf.py size` after adding baked assets.

## Git and pull requests

- PR titles use Conventional Commit types, enforced by `.github/workflows/pr-title.yml`.
- Do not commit, push, or rewrite history unless explicitly asked.

## Completion

Report exactly which checks ran and which were skipped. Never describe something as verified on hardware unless it was actually run on a board.
