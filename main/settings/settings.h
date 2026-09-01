// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Device-owned settings, persisted in NVS.
//
// The split between these and the protocol's state fields is by owner, not by
// kind. Anything the device must honour with no host attached -- and still
// honour after a power cycle -- lives here: brightness, the idle timeouts, the
// input bindings. Anything describing what is currently on screen stays
// host-pushed and is forgotten on reset.
//
// That is why brightness is not simply another field on a SHOW: a display
// plugged into a dead host still has to come up at the brightness its owner
// chose.
//
// Every value is a uint32 so the table, the wire format and the NVS records
// stay uniform. Ranges live in the table rather than at the call sites.

typedef enum {
    ZD_SETTING_BRIGHTNESS = 0,
    ZD_SETTING_DIM_LEVEL,
    ZD_SETTING_DIM_AFTER,
    ZD_SETTING_BLANK_AFTER,
    ZD_SETTING_HOST_ABSENT_AFTER,
    ZD_SETTING_VIEW,
    ZD_SETTING_IDLE_SCREENS,
    ZD_SETTING_ROTATION,
    ZD_SETTING_COUNT,
} zd_setting_t;

// Loads persisted values, falling back to defaults for anything unset or out of
// range. Safe to call before NVS is usable: the defaults still apply, and
// nothing is persisted until NVS comes up.
void zd_settings_init(void);

// Resolves a wire name such as "brightness". Returns false for unknown keys.
bool zd_settings_lookup(const char *name, zd_setting_t *out);

const char *zd_settings_name(zd_setting_t setting);
uint32_t zd_settings_get(zd_setting_t setting);

// True when the value is one this setting accepts. Most settings take any value
// in a contiguous range; a few, such as the display rotation, accept only
// certain steps within it.
bool zd_settings_in_range(zd_setting_t setting, uint32_t value);

// Validates, applies and persists. Returns ESP_ERR_INVALID_ARG for a value out
// of range. A write that fails to persist still takes effect for this session
// and is reported, because refusing a setting the user can see working would be
// the more confusing failure.
esp_err_t zd_settings_set(zd_setting_t setting, uint32_t value);
