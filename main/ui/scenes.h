// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include "ui/canvas.h"
#include "ui/state.h"

// zd_scenes_draw composes the scene described by state onto canvas. It does no
// panel I/O; the caller is responsible for pushing the canvas to the display.
void zd_scenes_draw(zd_canvas_t *canvas, const zapdisplay_state_t *state);

// zd_scenes_retention_advance steps the image-retention offset every scene draws
// against. The tick calls it on a timer so content keeps moving even when no
// host is sending SHOW. The caller is responsible for the redraw that makes the
// new offset visible.
void zd_scenes_retention_advance(void);

// Current offset index, reported by the protocol's STATE command so the shift
// can be observed without watching the panel.
unsigned zd_scenes_retention_phase(void);
