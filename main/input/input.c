// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "input/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "runtime/tick.h"

static const char *TAG = "input";

#define BINDINGS_NAMESPACE "zapdisplay"
#define BINDINGS_KEY "bindings"

// Only button events are bindable, and only the first few of them. Four keys is
// already more than this board exposes; the table exists to be edited, not to
// be exhaustive.
#define BINDABLE_BUTTONS 4
#define BINDINGS_SIZE (BINDABLE_BUTTONS * ZD_INPUT_ACTION_COUNT)

static const char *const SOURCE_NAMES[ZD_INPUT_SOURCE_COUNT] = {"button", "encoder", "touch"};
static const char *const ACTION_NAMES[ZD_INPUT_ACTION_COUNT] = {"down", "up",    "click", "double",
                                                                "long", "delta", "tap"};
static const char *const BINDING_NAMES[ZD_BIND_COUNT] = {
    "forward", "none", "wake", "view.next", "view.prev", "brightness.cycle", "screen.toggle",
};

static uint8_t s_bindings[BINDINGS_SIZE];

static size_t binding_index(uint8_t id, zd_input_action_t action)
{
    return (size_t)id * ZD_INPUT_ACTION_COUNT + (size_t)action;
}

static bool bindable(const zd_input_event_t *event)
{
    return event->source == ZD_INPUT_SOURCE_BUTTON && event->id < BINDABLE_BUTTONS &&
           event->action < ZD_INPUT_ACTION_COUNT;
}

// The shipped defaults give the one key this board has something useful to do
// without a host: cycle the view, and blank the panel on a long press. Anything
// else forwards, so a second button added later reaches the host by default
// rather than doing nothing.
static void apply_default_bindings(void)
{
    memset(s_bindings, ZD_BIND_FORWARD, sizeof(s_bindings));
    s_bindings[binding_index(0, ZD_INPUT_CLICK)] = ZD_BIND_VIEW_NEXT;
    s_bindings[binding_index(0, ZD_INPUT_LONG)] = ZD_BIND_SCREEN_TOGGLE;
    // Press and release are the raw halves of a click. Reporting them as well
    // would double every button event on the wire for no gain.
    for (uint8_t id = 0; id < BINDABLE_BUTTONS; id++) {
        s_bindings[binding_index(id, ZD_INPUT_DOWN)] = ZD_BIND_NONE;
        s_bindings[binding_index(id, ZD_INPUT_UP)] = ZD_BIND_NONE;
    }
}

static void persist_bindings(void)
{
    nvs_handle_t handle;

    if (nvs_open(BINDINGS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open NVS to persist bindings");
        return;
    }
    if (nvs_set_blob(handle, BINDINGS_KEY, s_bindings, sizeof(s_bindings)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

void zd_input_init(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(s_bindings);

    apply_default_bindings();

    if (nvs_open(BINDINGS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    // A stored table of a different length was written by a build with a
    // different vocabulary. Defaults are a better answer than reinterpreting
    // someone else's bytes.
    if (nvs_get_blob(handle, BINDINGS_KEY, s_bindings, &size) != ESP_OK || size != sizeof(s_bindings)) {
        apply_default_bindings();
    }
    for (size_t i = 0; i < sizeof(s_bindings); i++) {
        if (s_bindings[i] >= ZD_BIND_COUNT) {
            s_bindings[i] = ZD_BIND_FORWARD;
        }
    }
    nvs_close(handle);
}

const char *zd_input_source_name(zd_input_source_t source)
{
    return source < ZD_INPUT_SOURCE_COUNT ? SOURCE_NAMES[source] : "unknown";
}

const char *zd_input_action_name(zd_input_action_t action)
{
    return action < ZD_INPUT_ACTION_COUNT ? ACTION_NAMES[action] : "unknown";
}

const char *zd_input_binding_name(zd_input_binding_t binding)
{
    return binding < ZD_BIND_COUNT ? BINDING_NAMES[binding] : "unknown";
}

bool zd_input_binding_lookup(const char *name, zd_input_binding_t *binding)
{
    for (int i = 0; i < ZD_BIND_COUNT; i++) {
        if (strcmp(name, BINDING_NAMES[i]) == 0) {
            *binding = (zd_input_binding_t)i;
            return true;
        }
    }
    return false;
}

zd_input_binding_t zd_input_binding_for(const zd_input_event_t *event)
{
    if (!bindable(event)) {
        return ZD_BIND_FORWARD;
    }
    return (zd_input_binding_t)s_bindings[binding_index(event->id, event->action)];
}

bool zd_input_bind(const zd_input_event_t *event, zd_input_binding_t binding)
{
    if (!bindable(event) || binding >= ZD_BIND_COUNT) {
        return false;
    }
    s_bindings[binding_index(event->id, event->action)] = (uint8_t)binding;
    persist_bindings();
    return true;
}

bool zd_input_parse(const char *args, zd_input_event_t *event)
{
    char source[16];
    char action[16];
    unsigned id;
    long value = 0;

    const int fields = sscanf(args, "%15s %u %15s %ld", source, &id, action, &value);
    if (fields < 3 || id > UINT8_MAX) {
        return false;
    }

    memset(event, 0, sizeof(*event));
    event->id = (uint8_t)id;
    event->value = (int32_t)value;
    event->has_value = fields >= 4;

    for (int i = 0; i < ZD_INPUT_SOURCE_COUNT; i++) {
        if (strcmp(source, SOURCE_NAMES[i]) == 0) {
            event->source = (zd_input_source_t)i;
            goto source_ok;
        }
    }
    return false;

source_ok:
    for (int i = 0; i < ZD_INPUT_ACTION_COUNT; i++) {
        if (strcmp(action, ACTION_NAMES[i]) == 0) {
            event->action = (zd_input_action_t)i;
            return true;
        }
    }
    return false;
}

void zd_input_format(const zd_input_event_t *event, char *buf, size_t size)
{
    if (event->has_value) {
        snprintf(buf, size, "%s %u %s %ld", zd_input_source_name(event->source), event->id,
                 zd_input_action_name(event->action), (long)event->value);
    } else {
        snprintf(buf, size, "%s %u %s", zd_input_source_name(event->source), event->id,
                 zd_input_action_name(event->action));
    }
}

zd_input_binding_t zd_input_dispatch(const zd_input_event_t *event)
{
    // Releasing a key that woke the panel must not act either, so only the
    // press half is allowed to consume the wake.
    if (event->action != ZD_INPUT_UP && zd_tick_wake()) {
        return ZD_BIND_NONE;
    }
    zd_tick_notify_activity();
    return zd_input_binding_for(event);
}
