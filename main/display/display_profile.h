// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZD_DISPLAY_PROFILE_NAME "hd458002c40"

typedef struct {
    uint16_t active_width;
    uint16_t active_height;
    uint16_t visible_width;
    uint16_t visible_height;
    uint16_t overscan_left;
    uint32_t pixel_clock_hz;
    uint16_t hsync_pulse_width;
    uint16_t hsync_front_porch;
    uint16_t hsync_back_porch;
    uint16_t vsync_pulse_width;
    uint16_t vsync_front_porch;
    uint16_t vsync_back_porch;
    bool hsync_idle_low;
    bool vsync_idle_low;
    bool de_idle_high;
    bool pclk_active_high;
    bool pclk_idle_high;
} zd_display_timing_t;

typedef struct {
    const char *name;
    const char *controller;
    zd_display_timing_t timing;
} zd_display_profile_t;

const zd_display_profile_t *zd_display_get_profile(void);
void zd_display_log_profile(void);
