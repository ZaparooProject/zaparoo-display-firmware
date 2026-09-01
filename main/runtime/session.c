// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "runtime/session.h"

#include <string.h>

#include "esp_timer.h"

// Longest a banner may stay up. A toast that never expires is a scene, and a
// scene is not what a scan confirmation should become.
#define TOAST_MAX_SECONDS 30

static uint32_t s_elapsed_base_s;
static int64_t s_elapsed_set_us;
static bool s_has_elapsed;

static int64_t s_clock_local_epoch_s;
static int64_t s_clock_set_us;
static bool s_has_clock;

static char s_toast[64];
static int64_t s_toast_expires_us;

void zd_session_set_elapsed(uint32_t seconds)
{
    s_elapsed_base_s = seconds;
    s_elapsed_set_us = esp_timer_get_time();
    s_has_elapsed = true;
}

void zd_session_clear_elapsed(void)
{
    s_has_elapsed = false;
}

bool zd_session_elapsed(uint32_t *seconds)
{
    if (!s_has_elapsed) {
        return false;
    }
    *seconds = s_elapsed_base_s + (uint32_t)((esp_timer_get_time() - s_elapsed_set_us) / 1000000);
    return true;
}

void zd_session_set_clock(int64_t epoch_seconds, int32_t utc_offset_seconds)
{
    // Stored already shifted into local time: the device has no timezone
    // database and no use for UTC on its own.
    s_clock_local_epoch_s = epoch_seconds + utc_offset_seconds;
    s_clock_set_us = esp_timer_get_time();
    s_has_clock = true;
}

int16_t zd_session_clock_minutes(void)
{
    if (!s_has_clock) {
        return -1;
    }

    const int64_t now = s_clock_local_epoch_s + (esp_timer_get_time() - s_clock_set_us) / 1000000;
    // Floored rather than truncated, so the arithmetic still holds for the
    // negative epochs a host with a badly wrong clock could hand us.
    int64_t into_day = now % 86400;
    if (into_day < 0) {
        into_day += 86400;
    }
    return (int16_t)(into_day / 60);
}

void zd_session_set_toast(const char *text, uint32_t seconds)
{
    if (seconds > TOAST_MAX_SECONDS) {
        seconds = TOAST_MAX_SECONDS;
    }
    strlcpy(s_toast, text, sizeof(s_toast));
    s_toast_expires_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
}

void zd_session_clear_toast(void)
{
    s_toast[0] = '\0';
    s_toast_expires_us = 0;
}

const char *zd_session_toast(void)
{
    if (s_toast[0] == '\0' || esp_timer_get_time() >= s_toast_expires_us) {
        return NULL;
    }
    return s_toast;
}
