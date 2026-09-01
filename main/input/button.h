// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>

#include "input/input.h"

// The board's BOOT key as a general-purpose button.
//
// It is the only key fitted, it is already debounced by an RC network, and it
// costs nothing to use -- no wiring, no enclosure change. GPIO0 is a strapping
// pin, so holding it through a reset still enters download mode; that is a
// documented quirk of the board, not something the firmware can or should hide.
//
// Sampled from the protocol loop's existing 20ms poll rather than an interrupt.
// A key press has no deadline a person can perceive at that rate, and staying
// out of interrupt context keeps the whole firmware single-threaded.

void zd_button_init(void);

// Polls the key. Returns true and fills event when one is recognised. Only
// click and long press are produced: distinguishing a double click would mean
// holding every single click back to see whether a second arrives, and that
// delay is more annoying than the gesture is useful.
bool zd_button_poll(zd_input_event_t *event);
