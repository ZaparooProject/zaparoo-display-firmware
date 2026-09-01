// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Cover art bounds. Artwork is not a fixed slot: box art runs anywhere from
// tall portrait to wide landscape depending on the system, so the host sends
// whatever aspect it has and the playing scene lays out around it.
//
// Art fills the full height of the safe area. Width follows from the aspect,
// capped so the detail column keeps a readable width; art wider than the cap is
// scaled down and centred instead.
#define ZD_COVER_MAX_WIDTH 512
#define ZD_COVER_MAX_HEIGHT 288
#define ZD_COVER_MAX_BYTES (ZD_COVER_MAX_WIDTH * ZD_COVER_MAX_HEIGHT * 2)

typedef enum {
    ZAPDISPLAY_SCENE_IDLE = 0,
    ZAPDISPLAY_SCENE_LAUNCHING,
    ZAPDISPLAY_SCENE_PLAYING,
    ZAPDISPLAY_SCENE_ERROR,
} zapdisplay_scene_t;

// How the playing scene arranges what it has. The host supplies one set of
// fields; the view decides how much of it competes with the artwork.
typedef enum {
    // Artwork only, filling the panel.
    ZAPDISPLAY_VIEW_ART = 0,
    // Artwork beside title, system and status.
    ZAPDISPLAY_VIEW_DETAIL,
    // Artwork beside the catalogue metadata.
    ZAPDISPLAY_VIEW_META,
    ZAPDISPLAY_VIEW_COUNT,
} zapdisplay_view_t;

// The parts idle may show, as bit positions in the idle_screens setting.
//
// These used to be three compositions taking turns. They are now three elements
// of one frame: the mark, the time, and what was last played. Taking turns cost
// more than it bought -- the same status line was drawn twice at two different
// sizes, and the panel cut between a narrow centred card and a full-width
// layout every ten seconds. See ui/scenes.c.
typedef enum {
    ZAPDISPLAY_IDLE_BRAND = 0,
    ZAPDISPLAY_IDLE_CLOCK,
    ZAPDISPLAY_IDLE_LAST_PLAYED,
    ZAPDISPLAY_IDLE_COUNT,
} zapdisplay_idle_screen_t;

// zapdisplay_state_t is the full description of what the panel should show.
// The protocol layer accumulates it and the scene renderer consumes it, so it
// is deliberately free of panel or board detail.
typedef struct {
    zapdisplay_scene_t scene;
    char title[64];
    char system[48];
    char status[48];
    char message[96];
    // Catalogue metadata. Shown by the meta view; empty fields are skipped
    // rather than drawn as a gap, because most systems fill only some of them.
    char year[8];
    char developer[48];
    char genre[32];
    char players[16];
    const uint16_t *cover_rgb565;
    // What was last actually played. Kept apart from title/system/cover so idle
    // can show it without a host re-sending anything, and so a CLEAR followed by
    // some other scene cannot pick it up by accident. Captured when a playing
    // frame is shown, which is also what keeps a launch that failed from being
    // remembered as something played. Held in RAM, so it survives CLEAR but not
    // a reset, like everything else the host pushes.
    char last_title[64];
    char last_system[48];
    const uint16_t *last_cover_rgb565;
    uint16_t last_cover_width;
    uint16_t last_cover_height;
    uint16_t accent_color;
    uint16_t cover_width;
    uint16_t cover_height;
    zapdisplay_view_t view;
    // Bitmask over zapdisplay_idle_screen_t. Resolved from the idle_screens
    // setting by the protocol so scenes stay pure functions of this struct; a
    // mask of zero means the mark alone.
    uint8_t idle_elements;

    // -- Values that change on their own as time passes -------------------
    //
    // These are derived at render time rather than pushed per minute, and they
    // are deliberately excluded from the "did the frame change" comparison the
    // idle policy uses: a clock ticking is not someone using the device.

    // Session length in seconds, extrapolated from the baseline the host set.
    uint32_t elapsed_seconds;
    // Local time as minutes since midnight, or -1 when no host has set the
    // clock. There is no RTC, so this is unknown until told and forgotten on
    // reset.
    int16_t clock_minutes;
    bool has_elapsed;

    // Which cell of the launching scene's progress track is lit. A counter the
    // tick advances rather than a function of uptime, so it keeps moving with
    // no host attached, and excluded from the "did the frame change" comparison
    // for the same reason the clock is: a bar marching is not somebody using
    // the device.
    uint8_t progress_phase;

    // Transient banner over whatever scene is current. Used for token scan
    // feedback; composes with every scene, so it needs no scene of its own.
    char toast[64];
    bool has_toast;

    // Set once the host sends ACCENT. Without it a black accent would be
    // indistinguishable from no accent at all.
    bool has_accent;
} zapdisplay_state_t;

void zapdisplay_state_set_defaults(zapdisplay_state_t *state);
const char *zapdisplay_scene_name(zapdisplay_scene_t scene);
const char *zapdisplay_view_name(zapdisplay_view_t view);
