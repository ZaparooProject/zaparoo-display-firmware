// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include "esp_err.h"
#include "ui/state.h"

esp_err_t zd_hd458002c40_init(void);
esp_err_t zd_hd458002c40_render(const zapdisplay_state_t *state);
