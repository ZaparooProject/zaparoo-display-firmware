// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/font.h"

// zd_canvas_t draws a logical landscape UI onto a portrait panel framebuffer.
//
// The panel is physically portrait and the framebuffer is wider than the
// visible area, so every write is rotated a quarter turn and offset by
// overscan_left. Callers work purely in logical coordinates and never need to
// know either detail.
//
// rotation turns the logical image, not the panel: 0 is the quarter turn
// clockwise this panel has always used, and 180 composes a half turn on top of
// it for a board mounted the other way up. Both land inside the same visible
// window, so the overscan stays on the same physical edge either way.
//
// 90 and 270 are deliberately absent. They would leave the logical space
// portrait rather than landscape, and every scene is written against 960x320;
// that is a second UI, not a rotation.
typedef struct {
    uint16_t *pixels;
    uint16_t active_width;
    uint16_t active_height;
    uint16_t visible_width;
    uint16_t visible_height;
    uint16_t logical_width;
    uint16_t logical_height;
    uint16_t overscan_left;
    // Degrees, 0 or 180. Anything else is treated as 0.
    uint16_t rotation;
} zd_canvas_t;

// A rectangle. Logical coordinates unless something says otherwise.
typedef struct {
    int x;
    int y;
    int width;
    int height;
} zd_rect_t;

// Maps a logical rectangle onto the framebuffer, clipped to the visible area
// and with the overscan already added. Returns false when none of it is
// visible.
//
// This exists so the display layer can push one band instead of a whole frame.
// A full frame on this panel costs around half a second: 400x960 drawn and then
// copied again into the driver's own buffer, over a PSRAM bus the LCD DMA is
// already reading flat out. Anything that repaints on a timer needs a window.
bool zd_canvas_physical_rect(const zd_canvas_t *canvas, zd_rect_t logical, zd_rect_t *out_physical);

// -- Pixels and rectangles --------------------------------------------------

void zd_canvas_fill_active(zd_canvas_t *canvas, uint16_t color);
void zd_canvas_fill_rect(zd_canvas_t *canvas, int x, int y, int width, int height, uint16_t color);

// Per-pixel read, write and blend are deliberately not exposed. Scenes compose
// from the shapes, text and blits below; anything that needs a single pixel is
// working at the wrong layer.

// Rounded rectangle with antialiased corners. Radii are clamped to half the
// shorter side.
void zd_canvas_fill_round_rect(zd_canvas_t *canvas, int x, int y, int width, int height, int radius, uint16_t color);

// There is deliberately no raised-card or bordered-card primitive. Those belong
// to surfaces you press, and the device takes input from a physical key and from
// the host, never from a region of the screen. Nothing drawn here needs to look
// pressable; containers are a fill and a radius. See ui/scenes.c.

// -- Text -------------------------------------------------------------------
//
// Text is baseline anchored: y is the baseline, not the top of a cell. Mixed
// sizes on one row only line up if they share a baseline. Input is UTF-8;
// unbaked code points draw the font's fallback glyph.

int zd_canvas_text_width(const zd_font_t *font, const char *utf8);
int zd_canvas_draw_text(zd_canvas_t *canvas, int x, int baseline_y, const zd_font_t *font, const char *utf8,
                        uint16_t color);

// Draws at most max_width pixels, appending a real ellipsis when the run is cut
// short. Returns the width drawn.
int zd_canvas_draw_text_elided(zd_canvas_t *canvas, int x, int baseline_y, int max_width, const zd_font_t *font,
                               const char *utf8, uint16_t color);
int zd_canvas_draw_text_centered(zd_canvas_t *canvas, int center_x, int baseline_y, int max_width,
                                 const zd_font_t *font, const char *utf8, uint16_t color);

// A section heading: a label sitting on a hairline rule. Returns the y below
// the rule, so callers can stack content without restating the arithmetic.
int zd_canvas_draw_section_header(zd_canvas_t *canvas, int x, int label_baseline_y, int width, const zd_font_t *font,
                                  const char *label, uint16_t label_color, int rule_gap, int rule_height,
                                  uint16_t rule_color);

// -- Images -----------------------------------------------------------------

// Cover art, with its corners rounded to match the rest of the chrome.
void zd_canvas_blit_rgb565_round(zd_canvas_t *canvas, int x, int y, const uint16_t *pixels, int width, int height,
                                 int radius);

// Full-colour image with a separate 4-bit alpha plane, for the logo's bolt.
void zd_canvas_blit_rgb565_alpha(zd_canvas_t *canvas, int x, int y, const uint16_t *pixels, const uint8_t *alpha,
                                 int width, int height);

// A 4-bit alpha mask painted in one colour, for the logo's wordmark. Storing
// flat artwork as a mask lets it take a palette colour instead of shipping a
// second variant.
void zd_canvas_blit_alpha_mask(zd_canvas_t *canvas, int x, int y, const uint8_t *alpha, int width, int height,
                               uint16_t color);

// -- Widgets ----------------------------------------------------------------

// Segmented progress. Discrete cells rather than a continuous fill, whose right
// edge would land on a fractional pixel.
//
// on_count cells starting at first_on take on_color and the rest take
// off_color, which covers both forms the track has: a single lit cell walking
// it says "working, length unknown", and a run from zero says how far along
// something is. The overall width never changes with either.
void zd_canvas_draw_progress_track(zd_canvas_t *canvas, int x, int y, int cells, int first_on, int on_count,
                                   int cell_width, int cell_height, int gap, int radius, uint16_t on_color,
                                   uint16_t off_color);

// -- Baseline helpers -------------------------------------------------------

// Baseline for text whose capitals should start at cap_top_y.
static inline int zd_baseline_for_cap_top(const zd_font_t *font, int cap_top_y)
{
    return cap_top_y + font->cap_height;
}

// Baseline that optically centres capitals in a band, which is what a header or
// bar row wants -- centring the full ascent-to-descent box instead leaves the
// text sitting visibly high.
static inline int zd_baseline_centered(const zd_font_t *font, int y, int height)
{
    return y + (height + font->cap_height) / 2;
}
