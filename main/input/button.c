// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "input/button.h"

#include "driver/gpio.h"
#include "esp_timer.h"

// The BOOT key pulls GPIO0 to ground.
#define BUTTON_GPIO GPIO_NUM_0
#define BUTTON_ID 0

// Three consecutive agreeing samples at the loop's 20ms cadence. The board's RC
// network already suppresses most bounce; this covers the rest without adding
// perceptible lag.
#define DEBOUNCE_SAMPLES 3
#define LONG_PRESS_US (600 * 1000)

static bool s_pressed;
static uint8_t s_agree;
static bool s_last_sample;
static int64_t s_pressed_at_us;
static bool s_long_fired;

void zd_button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&config);
    s_pressed = false;
    s_agree = 0;
    s_last_sample = false;
    s_long_fired = false;
}

static void emit(zd_input_event_t *event, zd_input_action_t action)
{
    event->source = ZD_INPUT_SOURCE_BUTTON;
    event->id = BUTTON_ID;
    event->action = action;
    event->value = 0;
    event->has_value = false;
}

bool zd_button_poll(zd_input_event_t *event)
{
    const bool sample = gpio_get_level(BUTTON_GPIO) == 0;
    const int64_t now = esp_timer_get_time();

    // A long press fires while the key is still down, so it feels like a hold
    // rather than something that only happens once you let go.
    if (s_pressed && !s_long_fired && now - s_pressed_at_us >= LONG_PRESS_US) {
        s_long_fired = true;
        emit(event, ZD_INPUT_LONG);
        return true;
    }

    if (sample != s_last_sample) {
        s_last_sample = sample;
        s_agree = 1;
        return false;
    }
    if (s_agree < DEBOUNCE_SAMPLES) {
        s_agree++;
    }
    if (s_agree < DEBOUNCE_SAMPLES || sample == s_pressed) {
        return false;
    }

    s_pressed = sample;
    if (s_pressed) {
        s_pressed_at_us = now;
        s_long_fired = false;
        return false;
    }

    // Released. A press that already reported a long hold has had its event.
    if (s_long_fired) {
        return false;
    }
    emit(event, ZD_INPUT_CLICK);
    return true;
}
