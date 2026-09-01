// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "runtime/tick.h"

#include "board/board.h"
#include "esp_timer.h"
#include "runtime/session.h"
#include "settings/settings.h"
#include "ui/scenes.h"
#include "ui/state.h"
#include "ui/theme.h"

static int64_t s_last_activity_us;
static int64_t s_last_contact_us;
static int64_t s_next_retention_us;
static zd_panel_level_t s_level;
static bool s_host_absent_reported;
// Set when the retention phase moves while the panel is dark. Rendering to a
// blank panel would push a full framebuffer nobody can see, so the redraw is
// deferred until it is worth doing.
static bool s_redraw_deferred;
// A blank the user asked for, which the idle policy must leave alone. Without
// this the very next tick would see an idle counter that had just been reset,
// decide the panel belongs at full, and undo the request.
static bool s_manual_blank;
static int64_t s_next_march_us;
static uint8_t s_progress_phase;
// Last values actually rendered, so a rollover is detected rather than
// redrawing on every tick.
static uint32_t s_shown_elapsed_minutes;
static int16_t s_shown_clock_minutes;
static bool s_shown_toast;

static uint8_t level_percent(zd_panel_level_t level)
{
    const uint8_t brightness = (uint8_t)zd_settings_get(ZD_SETTING_BRIGHTNESS);

    switch (level) {
    case ZD_PANEL_BLANK:
        return 0;
    case ZD_PANEL_DIM: {
        const uint8_t dim = (uint8_t)zd_settings_get(ZD_SETTING_DIM_LEVEL);
        // Dimming must never brighten. A dim level above the configured
        // brightness is a user mistake, not an instruction.
        return dim < brightness ? dim : brightness;
    }
    case ZD_PANEL_FULL:
    default:
        return brightness;
    }
}

static void apply_level(zd_panel_level_t level, uint32_t fade_ms)
{
    s_level = level;
    zd_board_backlight_fade_percent(level_percent(level), fade_ms);
}

void zd_tick_init(void)
{
    s_last_activity_us = esp_timer_get_time();
    s_last_contact_us = s_last_activity_us;
    s_next_retention_us = s_last_activity_us + ZD_RETENTION_PERIOD_US;
    s_host_absent_reported = false;
    s_redraw_deferred = false;
    s_next_march_us = s_last_activity_us + ZD_TRACK_MARCH_US;
    s_progress_phase = 0;
    s_shown_elapsed_minutes = 0;
    s_shown_clock_minutes = -1;
    s_shown_toast = false;
    s_manual_blank = false;
    apply_level(ZD_PANEL_FULL, 0);
}

void zd_tick_notify_activity(void)
{
    s_last_activity_us = esp_timer_get_time();

    // A deliberate blank survives the clock ticking, but not something actually
    // happening. A game launching is worth lighting the panel for.
    if (s_manual_blank) {
        s_manual_blank = false;
        apply_level(ZD_PANEL_FULL, ZD_FADE_WAKE_MS);
    }
}

void zd_tick_notify_host_contact(void)
{
    s_last_contact_us = esp_timer_get_time();
    s_host_absent_reported = false;
}

zd_panel_level_t zd_tick_panel_level(void)
{
    return s_level;
}

const char *zd_tick_panel_level_name(zd_panel_level_t level)
{
    switch (level) {
    case ZD_PANEL_FULL:
        return "full";
    case ZD_PANEL_DIM:
        return "dim";
    case ZD_PANEL_BLANK:
        return "blank";
    default:
        return "unknown";
    }
}

uint8_t zd_tick_progress_phase(void)
{
    return s_progress_phase;
}

bool zd_tick_progress_step(void)
{
    const int64_t now = esp_timer_get_time();

    if (now < s_next_march_us) {
        return false;
    }

    s_progress_phase = (uint8_t)((s_progress_phase + 1) % ZD_TRACK_CELLS);
    // From now rather than from the deadline: a march that fell behind during a
    // transfer should resume its cadence, not sprint to catch up.
    s_next_march_us = now + ZD_TRACK_MARCH_US;
    return s_level != ZD_PANEL_BLANK;
}

uint32_t zd_tick_idle_seconds(void)
{
    return (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000000);
}

uint32_t zd_tick_silent_seconds(void)
{
    return (uint32_t)((esp_timer_get_time() - s_last_contact_us) / 1000000);
}

bool zd_tick_wake(void)
{
    const bool was_dark = s_level != ZD_PANEL_FULL;

    s_manual_blank = false;
    s_last_activity_us = esp_timer_get_time();
    if (was_dark) {
        apply_level(ZD_PANEL_FULL, ZD_FADE_WAKE_MS);
    }
    return was_dark;
}

void zd_tick_toggle_blank(void)
{
    if (s_level == ZD_PANEL_BLANK) {
        zd_tick_wake();
        return;
    }
    // Restart the idle countdown from here, but without going through
    // notify_activity, which exists to undo exactly this.
    s_last_activity_us = esp_timer_get_time();
    s_manual_blank = true;
    apply_level(ZD_PANEL_BLANK, ZD_FADE_DIM_MS);
}

void zd_tick_apply_brightness(void)
{
    apply_level(s_level, 0);
}

// desired_level maps how long the device has been idle onto a panel level. A
// timeout of 0 disables that step.
static zd_panel_level_t desired_level(uint32_t idle_s)
{
    const uint32_t blank_after = zd_settings_get(ZD_SETTING_BLANK_AFTER);
    const uint32_t dim_after = zd_settings_get(ZD_SETTING_DIM_AFTER);

    if (blank_after != 0 && idle_s >= blank_after) {
        return ZD_PANEL_BLANK;
    }
    if (dim_after != 0 && idle_s >= dim_after) {
        return ZD_PANEL_DIM;
    }
    return ZD_PANEL_FULL;
}

zd_tick_result_t zd_tick(void)
{
    zd_tick_result_t result = {0};
    const int64_t now = esp_timer_get_time();

    if (now >= s_next_retention_us) {
        zd_scenes_retention_advance();
        s_next_retention_us = now + ZD_RETENTION_PERIOD_US;
        s_redraw_deferred = true;
    }

    const uint32_t idle_s = (uint32_t)((now - s_last_activity_us) / 1000000);
    const zd_panel_level_t desired = desired_level(idle_s);

    if (!s_manual_blank && desired != s_level) {
        // Waking is quick so the panel does not feel sluggish; dimming and
        // blanking ramp, so they read as deliberate rather than as a fault.
        apply_level(desired, desired == ZD_PANEL_FULL ? ZD_FADE_WAKE_MS : ZD_FADE_DIM_MS);
    }

    if (s_redraw_deferred && s_level != ZD_PANEL_BLANK) {
        result.redraw = true;
        s_redraw_deferred = false;
    }

    // After the level update, so a march that arrives on the same tick as a
    // blank is not reported as something worth drawing.
    result.progress_stepped = zd_tick_progress_step();

    uint32_t elapsed_s;
    if (zd_session_elapsed(&elapsed_s) && elapsed_s / 60 != s_shown_elapsed_minutes) {
        s_shown_elapsed_minutes = elapsed_s / 60;
        result.time_changed = true;
    }

    const int16_t clock_minutes = zd_session_clock_minutes();
    if (clock_minutes != s_shown_clock_minutes) {
        s_shown_clock_minutes = clock_minutes;
        result.time_changed = true;
    }

    const bool toast_live = zd_session_toast() != NULL;
    if (toast_live != s_shown_toast) {
        s_shown_toast = toast_live;
        result.toast_changed = true;
    }

    const uint32_t host_absent_after = zd_settings_get(ZD_SETTING_HOST_ABSENT_AFTER);
    const uint32_t silent_s = (uint32_t)((now - s_last_contact_us) / 1000000);
    if (!s_host_absent_reported && host_absent_after != 0 && silent_s >= host_absent_after) {
        s_host_absent_reported = true;
        result.host_absent = true;
    }

    return result;
}
