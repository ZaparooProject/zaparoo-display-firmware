// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdint.h>

// Bitmap fonts baked by tools/generate_font.py.
//
// Glyphs are stored at their tight ink bounding box rather than in a fixed
// cell. A fixed cell pads every glyph out to the widest one in the face, which
// costs roughly four times the flash for the same coverage -- the difference
// between ASCII and full Latin coverage on this device.
//
// Coverage is Basic Latin, Latin-1 Supplement, Latin Extended-A, and the
// horizontal ellipsis used to elide overlong text. Anything outside that draws
// the fallback glyph.

// One glyph's metrics. Bitmap data is 4-bit alpha, two pixels per byte, packed
// row-major with a stride of (width + 1) / 2 bytes.
//
// Deliberately not packed: unaligned 32-bit reads cost more on Xtensa than the
// padding saves across the six rungs.
typedef struct {
    uint32_t offset; // byte offset into zd_font_t.bitmap
    uint8_t width;
    uint8_t height;
    uint8_t advance;  // pen movement, which is not the ink width
    int8_t bearing_x; // ink left  = pen_x + bearing_x
    int8_t bearing_y; // ink top   = baseline_y - bearing_y
} zd_glyph_t;

// A contiguous run of code points. Coverage is a handful of runs, so lookup is
// a short linear scan rather than a binary search over every code point.
typedef struct {
    uint16_t first; // inclusive
    uint16_t last;  // inclusive
    uint16_t glyph_base;
} zd_font_range_t;

typedef struct {
    const uint8_t *bitmap;
    const zd_glyph_t *glyphs;
    const zd_font_range_t *ranges;
    uint16_t glyph_count;
    uint16_t fallback_glyph;
    uint8_t range_count;
    uint8_t pixel_size;
    uint8_t ascent;
    uint8_t descent;
    uint8_t line_height;
    // Layout positions text by cap height, so scene code never hardcodes a
    // per-rung offset that a font change would silently invalidate.
    uint8_t cap_height;
    uint8_t x_height;
} zd_font_t;

// Named for the type ladder shared with the Zaparoo app. The sizes differ
// because this panel is a 960x320 bar at 221 PPI rather than a 16:9 screen, and
// only the rungs scenes actually use are baked. See tools/generate_font.py.
extern const zd_font_t zd_font_hero;    // selected title
extern const zd_font_t zd_font_title;   // detail title
extern const zd_font_t zd_font_section; // section and status
extern const zd_font_t zd_font_body;    // body and control
extern const zd_font_t zd_font_caption; // secondary
