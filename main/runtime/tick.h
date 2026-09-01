// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Periodic device behaviour: image-retention stepping and the idle dim/blank
// policy.
//
// This runs from the protocol loop's existing poll rather than from a task or
// an esp_timer callback. The firmware is deliberately single-threaded -- the
// protocol loop owns the pending state, the asset buffer and the framebuffer --
// and a second context touching any of those would need locking that buys
// nothing here. See protocol/zapdisplay_protocol.c.
//
// The panel is an IPS TFT, so the risk this manages is not OLED-style emitter
// burn-in. It is a frame left at full brightness for hours -- most often
// because the host died and nothing noticed -- plus the slow, uniform ageing of
// the backlight LEDs, which dimming directly slows.

typedef enum {
    ZD_PANEL_FULL = 0,
    ZD_PANEL_DIM,
    ZD_PANEL_BLANK,
} zd_panel_level_t;

// What the tick noticed. The flags are separate rather than one redraw bit
// because only the caller knows which scene is up, and repainting the whole
// framebuffer for a clock the current scene does not show is pure waste.
typedef struct {
    // Unconditional: the retention offset moved, or the panel just woke.
    bool redraw;
    // A displayed minute rolled over -- session elapsed or the wall clock.
    bool time_changed;
    // The idle screen rotated.
    bool idle_rotated;
    // A toast appeared or expired.
    bool toast_changed;
    // Fires once per silent stretch. The host has been silent long enough to be
    // presumed gone, and the caller should drop back to a safe resting state
    // rather than leave a stale session frame up.
    bool host_absent;
} zd_tick_result_t;

void zd_tick_init(void);
zd_tick_result_t zd_tick(void);

// Something a person would notice: the rendered frame changed, or an input
// event arrived. This resets the dim/blank countdown. A host refresh that
// redraws the identical frame is deliberately not activity -- a machine left on
// the same game for an hour should dim.
void zd_tick_notify_activity(void);

// Any command at all from the host, including keepalives that change nothing.
// This resets the host-absent countdown only, so a healthy host holding a long
// session is never mistaken for a dead one.
void zd_tick_notify_host_contact(void);

zd_panel_level_t zd_tick_panel_level(void);
const char *zd_tick_panel_level_name(zd_panel_level_t level);

// Seconds since the last thing a person would have noticed, and since the last
// word from the host. Reported by the protocol's STATE command; the two differ
// whenever a host is refreshing an unchanging frame.
uint32_t zd_tick_idle_seconds(void);
uint32_t zd_tick_silent_seconds(void);

// Returns true if the panel was dimmed or blanked and has now been woken, which
// is also the signal that an input event should be consumed rather than acted
// on -- the press that wakes a screen should not also do something.
bool zd_tick_wake(void);

// Blanks the panel, or restores it if already blank. Held until something else
// wakes it, which is what a deliberate "turn the screen off" has to mean.
void zd_tick_toggle_blank(void);

// Re-applies the current level at whatever brightness the settings now hold.
// Call after changing the brightness or dim level.
void zd_tick_apply_brightness(void);

// Which idle composition is due. Skips screens disabled in settings, so a
// device with only one enabled simply never rotates.
uint8_t zd_tick_idle_screen(void);
