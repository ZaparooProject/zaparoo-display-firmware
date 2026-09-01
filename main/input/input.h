// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One input vocabulary, three producers.
//
// The host can inject an event with INPUT, a physical button raises the same
// event locally, and a touch panel could later do the same. They all arrive
// here, so a layout change works identically whether it came down the wire or
// off a key -- and the wire path means every binding is testable with no
// hardware attached.
//
// The firmware deliberately knows nothing about what an event *means*. It
// resolves each one to either a display-local action or "forward", and a
// forwarded event goes to the host to be mapped onto whatever it likes. On
// Zaparoo that is ZapScript, which already has a verb for everything a button
// on a shelf could reasonably do; teaching the firmware about launching games
// would duplicate that badly.

typedef enum {
    ZD_INPUT_SOURCE_BUTTON = 0,
    ZD_INPUT_SOURCE_ENCODER,
    ZD_INPUT_SOURCE_TOUCH,
    ZD_INPUT_SOURCE_COUNT,
} zd_input_source_t;

typedef enum {
    ZD_INPUT_DOWN = 0,
    ZD_INPUT_UP,
    ZD_INPUT_CLICK,
    ZD_INPUT_DOUBLE,
    ZD_INPUT_LONG,
    ZD_INPUT_DELTA,
    ZD_INPUT_TAP,
    ZD_INPUT_ACTION_COUNT,
} zd_input_action_t;

typedef struct {
    zd_input_source_t source;
    uint8_t id;
    zd_input_action_t action;
    int32_t value;
    bool has_value;
} zd_input_event_t;

// What the device should do with an event. FORWARD is the default for anything
// unbound, so a new kind of input reaches the host rather than being silently
// dropped.
typedef enum {
    ZD_BIND_FORWARD = 0,
    ZD_BIND_NONE,
    ZD_BIND_WAKE,
    ZD_BIND_VIEW_NEXT,
    ZD_BIND_VIEW_PREV,
    ZD_BIND_BRIGHTNESS_CYCLE,
    ZD_BIND_SCREEN_TOGGLE,
    ZD_BIND_COUNT,
} zd_input_binding_t;

void zd_input_init(void);

// Resolves an event to the action the caller should carry out. Returns
// ZD_BIND_NONE when the event was swallowed waking the panel: the press that
// wakes a screen should not also do something, which is what every phone does
// and what anyone reaching for a dark display expects.
zd_input_binding_t zd_input_dispatch(const zd_input_event_t *event);

// Parses "<source> <id> <action> [value]".
bool zd_input_parse(const char *args, zd_input_event_t *event);
// Formats the same shape back, for the EVENT line.
void zd_input_format(const zd_input_event_t *event, char *buf, size_t size);

const char *zd_input_source_name(zd_input_source_t source);
const char *zd_input_action_name(zd_input_action_t action);
const char *zd_input_binding_name(zd_input_binding_t binding);
bool zd_input_binding_lookup(const char *name, zd_input_binding_t *binding);

// Binds an event shape to an action and persists it. Only button events are
// bindable: there is no hardware for the other sources yet, and a binding table
// covering inputs that cannot occur is just a bigger blob to migrate later.
bool zd_input_bind(const zd_input_event_t *event, zd_input_binding_t binding);
zd_input_binding_t zd_input_binding_for(const zd_input_event_t *event);
