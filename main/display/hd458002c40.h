// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "ui/state.h"

esp_err_t zd_hd458002c40_init(void);
esp_err_t zd_hd458002c40_render(const zapdisplay_state_t *state);

// Sets which way up the UI is drawn, in degrees. 0 is the panel's own
// orientation and 180 flips it; anything else is ignored. Deliberately does not
// render -- the caller decides when the framebuffer is pushed, because it is
// the one that knows whether the host is still waiting on a response.
void zd_hd458002c40_set_rotation(uint32_t degrees);

// Repaints only the band the current scene animates, and pushes only that
// window. Falls back to a full render when there is nothing animating.
//
// This exists because a full frame measures around 540 ms on this panel -- the
// scene is drawn into 400x960 and then copied again into the driver's own
// buffer, over a PSRAM bus the LCD DMA is already reading continuously. That is
// more than twice the march interval, and on an asset upload it nearly doubled
// the transfer time. The animated band is under 5000 pixels.
esp_err_t zd_hd458002c40_render_progress(const zapdisplay_state_t *state);
