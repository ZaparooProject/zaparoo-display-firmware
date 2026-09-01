// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "settings/settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "ui/state.h"
#include "ui/theme.h"

static const char *TAG = "settings";

// NVS keys are capped at 15 characters, which every name below fits. They are
// also the wire names, so renaming one is a protocol change as well as a
// migration.
#define SETTINGS_NAMESPACE "zapdisplay"

typedef struct {
    const char *name;
    uint32_t min;
    uint32_t max;
    // Allowed increment above min, or 0 for any value in the range. A setting
    // whose valid values are not a contiguous run says so here rather than
    // leaving the call site to re-check it.
    uint32_t step;
    uint32_t fallback;
} setting_spec_t;

// clang-format off
static const setting_spec_t SPECS[ZD_SETTING_COUNT] = {
    [ZD_SETTING_BRIGHTNESS]  = {"brightness",  0, 100,   0, ZD_BRIGHTNESS_DEFAULT},
    [ZD_SETTING_DIM_LEVEL]   = {"dim_level",   0, 100,   0, ZD_DIM_LEVEL_DEFAULT},
    // 0 disables the timeout entirely. The upper bound is 24 hours; anything
    // longer is indistinguishable from "never" and 0 already says that.
    [ZD_SETTING_DIM_AFTER]   = {"dim_after",   0, 86400, 0, ZD_IDLE_DIM_AFTER_S_DEFAULT},
    [ZD_SETTING_BLANK_AFTER] = {"blank_after", 0, 86400, 0, ZD_IDLE_BLANK_AFTER_S_DEFAULT},
    [ZD_SETTING_HOST_ABSENT_AFTER] = {"host_absent", 0, 86400, 0, ZD_HOST_ABSENT_AFTER_S_DEFAULT},
    // Which playing view the device starts in and returns to on CLEAR. Indexes
    // zapdisplay_view_t.
    [ZD_SETTING_VIEW]        = {"view",         0, ZAPDISPLAY_VIEW_COUNT - 1, 0, ZAPDISPLAY_VIEW_DETAIL},
    // Bitmask over zapdisplay_idle_screen_t: bit 0 the mark, bit 1 the clock,
    // bit 2 last played. 0 leaves idle as the mark alone.
    [ZD_SETTING_IDLE_SCREENS] = {"idle_screens", 0, (1u << ZAPDISPLAY_IDLE_COUNT) - 1, 0, ZD_IDLE_SCREENS_DEFAULT},
    // Degrees, and only the two the UI has a layout for. A step of 180 is what
    // rejects 90 and 270; if a portrait layout ever lands this becomes
    // {0, 270, 90}. See ui/canvas.h.
    [ZD_SETTING_ROTATION]    = {"rotation",     0, 180, 180, ZD_ROTATION_DEFAULT},
};
// clang-format on

static uint32_t s_values[ZD_SETTING_COUNT];

void zd_settings_init(void)
{
    for (int i = 0; i < ZD_SETTING_COUNT; i++) {
        s_values[i] = SPECS[i].fallback;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        // A namespace that has never been written is the normal first-boot
        // case, not a fault.
        ESP_LOGI(TAG, "No stored settings (%s); using defaults", esp_err_to_name(ret));
        return;
    }

    for (int i = 0; i < ZD_SETTING_COUNT; i++) {
        uint32_t stored = 0;
        if (nvs_get_u32(handle, SPECS[i].name, &stored) != ESP_OK) {
            continue;
        }
        if (!zd_settings_in_range((zd_setting_t)i, stored)) {
            // A stored value outside the current range means the range moved
            // under it in a firmware update. Prefer the default over honouring
            // something the running build considers invalid.
            ESP_LOGW(TAG, "Stored %s=%lu out of range; using default", SPECS[i].name, (unsigned long)stored);
            continue;
        }
        s_values[i] = stored;
    }

    nvs_close(handle);
}

bool zd_settings_lookup(const char *name, zd_setting_t *out)
{
    for (int i = 0; i < ZD_SETTING_COUNT; i++) {
        if (strcmp(name, SPECS[i].name) == 0) {
            *out = (zd_setting_t)i;
            return true;
        }
    }
    return false;
}

const char *zd_settings_name(zd_setting_t setting)
{
    if (setting < 0 || setting >= ZD_SETTING_COUNT) {
        return "unknown";
    }
    return SPECS[setting].name;
}

uint32_t zd_settings_get(zd_setting_t setting)
{
    if (setting < 0 || setting >= ZD_SETTING_COUNT) {
        return 0;
    }
    return s_values[setting];
}

bool zd_settings_in_range(zd_setting_t setting, uint32_t value)
{
    if (setting < 0 || setting >= ZD_SETTING_COUNT) {
        return false;
    }

    const setting_spec_t *spec = &SPECS[setting];

    if (value < spec->min || value > spec->max) {
        return false;
    }
    return spec->step == 0 || (value - spec->min) % spec->step == 0;
}

esp_err_t zd_settings_set(zd_setting_t setting, uint32_t value)
{
    if (!zd_settings_in_range(setting, value)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_values[setting] == value) {
        return ESP_OK;
    }

    s_values[setting] = value;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS to persist %s: %s", SPECS[setting].name, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u32(handle, SPECS[setting].name, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot persist %s: %s", SPECS[setting].name, esp_err_to_name(ret));
    }

    nvs_close(handle);
    return ret;
}
