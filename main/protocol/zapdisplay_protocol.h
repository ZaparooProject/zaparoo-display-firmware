// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

// ZAPDISPLAY_PROTOCOL is the wire contract version. Bump it only for a change
// hosts cannot tolerate; additive commands do not need a new version.
#define ZAPDISPLAY_PROTOCOL "zapdisplay/1"

void zapdisplay_protocol_run(void);
