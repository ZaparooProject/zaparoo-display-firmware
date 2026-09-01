// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Time references the host sets once and the device extrapolates from.
//
// Both exist so a long session does not need a message a minute. The host says
// "this session started N seconds ago" or "it is now T", and the device counts
// on from there against its own monotonic clock. That also means the values
// keep advancing through a host's quiet periods, and stop being a reason to
// talk to the device at all.
//
// There is no RTC. The wall clock is unknown until a host sets it and is
// forgotten on reset, which is why the idle clock screen is skipped rather
// than guessed at.

void zd_session_set_elapsed(uint32_t seconds);
void zd_session_clear_elapsed(void);
// Writes the current session length and returns true, or returns false when no
// host has set a baseline.
bool zd_session_elapsed(uint32_t *seconds);

void zd_session_set_clock(int64_t epoch_seconds, int32_t utc_offset_seconds);
// Local time as minutes since midnight, or -1 when unknown.
int16_t zd_session_clock_minutes(void);

// A banner shown over the current scene until it expires.
void zd_session_set_toast(const char *text, uint32_t seconds);
void zd_session_clear_toast(void);
// Returns the live toast text, or NULL once it has expired.
const char *zd_session_toast(void);
