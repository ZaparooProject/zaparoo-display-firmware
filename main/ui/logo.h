// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdint.h>

#include "ui/canvas.h"

// The Zaparoo logo, baked by tools/generate_logo.py.
//
// Stored as two layers because the mark is two kinds of artwork. The bolt
// carries a gradient and needs real colour. The wordmark is flat, so it is an
// alpha mask tinted at draw time -- it therefore follows the palette, and the
// firmware ships one variant where the source artwork needs a light and a dark
// PNG.
//
// The Zaparoo name and logo are trademarks of Wizzo Pty Ltd. See NOTICE.md.
typedef struct {
    uint16_t width;  // nominal rung size, for centring
    uint16_t height; //
    const uint16_t *bolt_rgb;
    const uint8_t *bolt_alpha;
    uint16_t bolt_x;
    uint16_t bolt_y;
    uint16_t bolt_w;
    uint16_t bolt_h;
    const uint8_t *word_alpha;
    uint16_t word_x;
    uint16_t word_y;
    uint16_t word_w;
    uint16_t word_h;
} zd_logo_t;

// Rungs match the sizes the artwork is pre-rendered at, so nothing is resampled.
extern const zd_logo_t zd_logo_144; // header
extern const zd_logo_t zd_logo_256; // idle screen

static inline void zd_canvas_draw_logo(zd_canvas_t *canvas, int x, int y, const zd_logo_t *logo, uint16_t word_color)
{
    zd_canvas_blit_rgb565_alpha(canvas, x + logo->bolt_x, y + logo->bolt_y, logo->bolt_rgb, logo->bolt_alpha,
                                logo->bolt_w, logo->bolt_h);
    zd_canvas_blit_alpha_mask(canvas, x + logo->word_x, y + logo->word_y, logo->word_alpha, logo->word_w, logo->word_h,
                              word_color);
}
