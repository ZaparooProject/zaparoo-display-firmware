// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "ui/canvas.h"

#include <stdbool.h>
#include <stddef.h>

#define ZD_ELLIPSIS_CODEPOINT 0x2026
#define ZD_REPLACEMENT_CODEPOINT 0xFFFD

void zd_canvas_fill_active(zd_canvas_t *canvas, uint16_t color)
{
    const size_t pixels = (size_t)canvas->active_width * canvas->active_height;
    for (size_t i = 0; i < pixels; ++i) {
        canvas->pixels[i] = color;
    }
}

static uint16_t zd_blend_rgb565(uint16_t dst, uint16_t src, uint8_t alpha);

// map_pixel resolves a logical coordinate to a framebuffer index, or returns
// false when it falls outside the visible area. Every write and every read goes
// through here, which is what keeps the orientation out of the scene code.
//
// The logical UI is rotated a quarter turn clockwise onto the portrait panel;
// rotation 180 composes a half turn on top of that. Substituting the half turn
// into the quarter turn collapses to a swap of which axis is reversed, so
// neither case is more expensive than the other.
static void map_coord(const zd_canvas_t *canvas, int x, int y, int *out_x, int *out_y)
{
    if (canvas->rotation == 180) {
        *out_x = y;
        *out_y = (int)canvas->visible_height - 1 - x;
        return;
    }
    *out_x = (int)canvas->visible_width - 1 - y;
    *out_y = x;
}

static bool map_pixel(const zd_canvas_t *canvas, int x, int y, size_t *out_index)
{
    int physical_x;
    int physical_y;

    map_coord(canvas, x, y, &physical_x, &physical_y);

    if (physical_x < 0 || physical_x >= canvas->visible_width || physical_y < 0 ||
        physical_y >= canvas->visible_height) {
        return false;
    }

    *out_index = (size_t)physical_y * canvas->active_width + canvas->overscan_left + physical_x;
    return true;
}

bool zd_canvas_physical_rect(const zd_canvas_t *canvas, zd_rect_t logical, zd_rect_t *out_physical)
{
    if (logical.width <= 0 || logical.height <= 0) {
        return false;
    }

    // Clip in logical space. The mapping is a rigid transform, so clipping
    // afterwards would mean working out which logical edge each physical one
    // came from.
    const int x0 = logical.x < 0 ? 0 : logical.x;
    const int y0 = logical.y < 0 ? 0 : logical.y;
    int x1 = logical.x + logical.width;
    int y1 = logical.y + logical.height;

    if (x1 > canvas->logical_width) {
        x1 = canvas->logical_width;
    }
    if (y1 > canvas->logical_height) {
        y1 = canvas->logical_height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }

    // Which corner lands where depends on the rotation, so take the extremes of
    // all four rather than restating the transform per orientation.
    const int corner_x[2] = {x0, x1 - 1};
    const int corner_y[2] = {y0, y1 - 1};
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;

    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            int px;
            int py;

            map_coord(canvas, corner_x[i], corner_y[j], &px, &py);
            if (i == 0 && j == 0) {
                min_x = max_x = px;
                min_y = max_y = py;
                continue;
            }
            if (px < min_x)
                min_x = px;
            if (px > max_x)
                max_x = px;
            if (py < min_y)
                min_y = py;
            if (py > max_y)
                max_y = py;
        }
    }

    *out_physical = (zd_rect_t){
        .x = canvas->overscan_left + min_x,
        .y = min_y,
        .width = max_x - min_x + 1,
        .height = max_y - min_y + 1,
    };
    return true;
}

static void zd_canvas_set_pixel(zd_canvas_t *canvas, int x, int y, uint16_t color)
{
    size_t index;

    if (map_pixel(canvas, x, y, &index)) {
        canvas->pixels[index] = color;
    }
}

static uint16_t zd_canvas_get_pixel(const zd_canvas_t *canvas, int x, int y)
{
    size_t index;

    return map_pixel(canvas, x, y, &index) ? canvas->pixels[index] : 0x0000;
}

// blend_pixel is the common read-modify-write for antialiased coverage.
static void blend_pixel(zd_canvas_t *canvas, int x, int y, uint16_t color, uint8_t alpha)
{
    if (alpha == 0) {
        return;
    }
    if (alpha >= 15) {
        zd_canvas_set_pixel(canvas, x, y, color);
        return;
    }
    zd_canvas_set_pixel(canvas, x, y, zd_blend_rgb565(zd_canvas_get_pixel(canvas, x, y), color, alpha));
}

void zd_canvas_fill_rect(zd_canvas_t *canvas, int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;
    int y1 = y + height;

    if (x1 > canvas->logical_width) {
        x1 = canvas->logical_width;
    }
    if (y1 > canvas->logical_height) {
        y1 = canvas->logical_height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    // Logical y is the contiguous framebuffer axis in both orientations: at
    // rotation 0 the index is x * active_width + overscan_left +
    // (visible_width - 1 - y), and at 180 it is (visible_height - 1 - x) *
    // active_width + overscan_left + y. Walking columns outermost keeps the
    // PSRAM writes sequential instead of striding by a whole row per pixel;
    // only which way the inner walk runs changes, so the branch is hoisted out
    // of the loop rather than repeated per pixel. This duplicates map_pixel on
    // purpose -- a rotation change has to be made in both places.
    const bool flipped = canvas->rotation == 180;
    const int step = flipped ? 1 : -1;

    for (int col = x0; col < x1; ++col) {
        const int physical_y = flipped ? (int)canvas->visible_height - 1 - col : col;
        const int physical_x = flipped ? y0 : canvas->visible_width - 1 - y0;
        uint16_t *pixel =
            canvas->pixels + (size_t)physical_y * canvas->active_width + canvas->overscan_left + physical_x;

        for (int line = y0; line < y1; ++line, pixel += step) {
            *pixel = color;
        }
    }
}

static uint16_t zd_blend_rgb565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 0) {
        return dst;
    }
    if (alpha >= 15) {
        return src;
    }

    const uint8_t src_r = (src >> 11) & 0x1F;
    const uint8_t src_g = (src >> 5) & 0x3F;
    const uint8_t src_b = src & 0x1F;
    const uint8_t dst_r = (dst >> 11) & 0x1F;
    const uint8_t dst_g = (dst >> 5) & 0x3F;
    const uint8_t dst_b = dst & 0x1F;

    const uint8_t out_r = (uint8_t)((src_r * alpha + dst_r * (15 - alpha) + 7) / 15);
    const uint8_t out_g = (uint8_t)((src_g * alpha + dst_g * (15 - alpha) + 7) / 15);
    const uint8_t out_b = (uint8_t)((src_b * alpha + dst_b * (15 - alpha) + 7) / 15);
    return (uint16_t)((out_r << 11) | (out_g << 5) | out_b);
}

// -- Rounded corners --------------------------------------------------------

static uint32_t isqrt32(uint32_t value)
{
    uint32_t result = 0;
    uint32_t bit = 1u << 30;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

// Coverage of one pixel against a corner arc, as 4-bit alpha.
//
// Works in half-pixel units so a pixel centre is exact without floating point:
// a pixel sitting exactly on the arc gets half coverage, and the transition
// spans a single pixel either side.
static uint8_t corner_alpha(int px, int py, int center_x, int center_y, int radius)
{
    const int dx = 2 * (px - center_x) + 1;
    const int dy = 2 * (py - center_y) + 1;
    const int distance = (int)isqrt32((uint32_t)(dx * dx + dy * dy));
    const int coverage = 15 * (2 * radius - distance + 1);

    if (coverage <= 0) {
        return 0;
    }
    if (coverage >= 30) {
        return 15;
    }
    return (uint8_t)(coverage / 2);
}

static int clamp_radius(int radius, int width, int height)
{
    const int limit = (width < height ? width : height) / 2;

    if (radius < 0) {
        return 0;
    }
    return radius > limit ? limit : radius;
}

void zd_canvas_fill_round_rect(zd_canvas_t *canvas, int x, int y, int width, int height, int radius, uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    radius = clamp_radius(radius, width, height);

    for (int row = 0; row < height; ++row) {
        int corner_radius = 0;
        int center_y = 0;

        if (row < radius) {
            corner_radius = radius;
            center_y = radius;
        } else if (row >= height - radius) {
            corner_radius = radius;
            center_y = height - radius;
        }

        for (int col = 0; col < corner_radius; ++col) {
            blend_pixel(canvas, x + col, y + row, color,
                        corner_alpha(col, row, corner_radius, center_y, corner_radius));
            blend_pixel(canvas, x + width - 1 - col, y + row, color,
                        corner_alpha(col, row, corner_radius, center_y, corner_radius));
        }

        const int span = width - 2 * corner_radius;
        if (span > 0) {
            zd_canvas_fill_rect(canvas, x + corner_radius, y + row, span, 1, color);
        }
    }
}

// -- Text -------------------------------------------------------------------

// utf8_next decodes one code point and advances the cursor.
//
// Malformed input yields U+FFFD and consumes exactly one byte, so the caller
// always makes progress. Multi-byte sequences are consumed whole, so text the
// font does not cover degrades to one fallback glyph per code point rather than
// one per byte.
static uint32_t utf8_next(const char **cursor)
{
    const uint8_t *bytes = (const uint8_t *)*cursor;
    const uint8_t lead = bytes[0];
    uint32_t codepoint;
    uint32_t lowest;
    int extra;

    if (lead < 0x80) {
        *cursor += 1;
        return lead;
    }
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        codepoint = lead & 0x1Fu;
        lowest = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        codepoint = lead & 0x0Fu;
        lowest = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        codepoint = lead & 0x07u;
        lowest = 0x10000;
    } else {
        *cursor += 1;
        return ZD_REPLACEMENT_CODEPOINT;
    }

    for (int i = 1; i <= extra; ++i) {
        if ((bytes[i] & 0xC0) != 0x80) {
            // Also covers a truncated sequence: the terminator fails this test.
            *cursor += 1;
            return ZD_REPLACEMENT_CODEPOINT;
        }
        codepoint = (codepoint << 6) | (bytes[i] & 0x3Fu);
    }
    *cursor += extra + 1;

    if (codepoint < lowest || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return ZD_REPLACEMENT_CODEPOINT;
    }
    return codepoint;
}

static const zd_glyph_t *font_glyph(const zd_font_t *font, uint32_t codepoint)
{
    for (uint8_t i = 0; i < font->range_count; ++i) {
        const zd_font_range_t *range = &font->ranges[i];
        if (codepoint >= range->first && codepoint <= range->last) {
            return &font->glyphs[range->glyph_base + (codepoint - range->first)];
        }
    }
    return &font->glyphs[font->fallback_glyph];
}

static void draw_glyph(zd_canvas_t *canvas, const zd_font_t *font, const zd_glyph_t *glyph, int pen_x, int baseline_y,
                       uint16_t color)
{
    if (glyph->width == 0 || glyph->height == 0) {
        return;
    }

    const int left = pen_x + glyph->bearing_x;
    const int top = baseline_y - glyph->bearing_y;
    const size_t stride = ((size_t)glyph->width + 1) / 2;
    const uint8_t *rows = font->bitmap + glyph->offset;

    for (int row = 0; row < glyph->height; ++row) {
        const uint8_t *packed = rows + (size_t)row * stride;
        for (int col = 0; col < glyph->width; ++col) {
            const uint8_t byte = packed[col / 2];
            const uint8_t alpha = (col & 1) == 0 ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
            blend_pixel(canvas, left + col, top + row, color, alpha);
        }
    }
}

int zd_canvas_text_width(const zd_font_t *font, const char *utf8)
{
    int width = 0;

    while (*utf8 != '\0') {
        width += font_glyph(font, utf8_next(&utf8))->advance;
    }
    return width;
}

int zd_canvas_draw_text(zd_canvas_t *canvas, int x, int baseline_y, const zd_font_t *font, const char *utf8,
                        uint16_t color)
{
    const int start = x;

    while (*utf8 != '\0') {
        const zd_glyph_t *glyph = font_glyph(font, utf8_next(&utf8));
        draw_glyph(canvas, font, glyph, x, baseline_y, color);
        x += glyph->advance;
    }
    return x - start;
}

// elided_width reports what draw_text_elided would occupy, so right and centred
// placement can be resolved before anything is painted.
static int elided_width(const zd_font_t *font, const char *utf8, int max_width)
{
    const int full = zd_canvas_text_width(font, utf8);
    if (full <= max_width) {
        return full;
    }

    const int ellipsis = font_glyph(font, ZD_ELLIPSIS_CODEPOINT)->advance;
    const int budget = max_width - ellipsis;
    int used = 0;

    if (budget < 0) {
        return 0;
    }
    while (*utf8 != '\0') {
        const int advance = font_glyph(font, utf8_next(&utf8))->advance;
        if (used + advance > budget) {
            break;
        }
        used += advance;
    }
    return used + ellipsis;
}

int zd_canvas_draw_text_elided(zd_canvas_t *canvas, int x, int baseline_y, int max_width, const zd_font_t *font,
                               const char *utf8, uint16_t color)
{
    if (zd_canvas_text_width(font, utf8) <= max_width) {
        return zd_canvas_draw_text(canvas, x, baseline_y, font, utf8, color);
    }

    const zd_glyph_t *ellipsis = font_glyph(font, ZD_ELLIPSIS_CODEPOINT);
    const int budget = max_width - ellipsis->advance;
    const int start = x;

    if (budget < 0) {
        return 0;
    }
    while (*utf8 != '\0') {
        const zd_glyph_t *glyph = font_glyph(font, utf8_next(&utf8));
        if (x - start + glyph->advance > budget) {
            break;
        }
        draw_glyph(canvas, font, glyph, x, baseline_y, color);
        x += glyph->advance;
    }
    draw_glyph(canvas, font, ellipsis, x, baseline_y, color);
    return x + ellipsis->advance - start;
}

int zd_canvas_draw_text_centered(zd_canvas_t *canvas, int center_x, int baseline_y, int max_width,
                                 const zd_font_t *font, const char *utf8, uint16_t color)
{
    const int width = elided_width(font, utf8, max_width);

    return zd_canvas_draw_text_elided(canvas, center_x - width / 2, baseline_y, max_width, font, utf8, color);
}

int zd_canvas_draw_section_header(zd_canvas_t *canvas, int x, int label_baseline_y, int width, const zd_font_t *font,
                                  const char *label, uint16_t label_color, int rule_gap, int rule_height,
                                  uint16_t rule_color)
{
    const int rule_y = label_baseline_y + font->descent + rule_gap;

    zd_canvas_draw_text_elided(canvas, x, label_baseline_y, width, font, label, label_color);
    zd_canvas_fill_rect(canvas, x, rule_y, width, rule_height, rule_color);
    return rule_y + rule_height;
}

// -- Images -----------------------------------------------------------------

void zd_canvas_blit_rgb565_round(zd_canvas_t *canvas, int x, int y, const uint16_t *pixels, int width, int height,
                                 int radius)
{
    radius = clamp_radius(radius, width, height);

    for (int row = 0; row < height; ++row) {
        int left_radius = 0;
        int left_center_y = 0;
        int right_center_y = 0;

        if (row < radius) {
            left_radius = radius;
            left_center_y = radius;
            right_center_y = radius;
        } else if (row >= height - radius) {
            left_radius = radius;
            left_center_y = height - radius;
            right_center_y = height - radius;
        }

        for (int col = 0; col < width; ++col) {
            const uint16_t color = pixels[(size_t)row * width + col];
            uint8_t alpha = 15;

            if (col < left_radius) {
                alpha = corner_alpha(col, row, left_radius, left_center_y, left_radius);
            } else if (left_radius != 0 && col >= width - left_radius) {
                alpha = corner_alpha(col, row, width - left_radius, right_center_y, left_radius);
            }
            blend_pixel(canvas, x + col, y + row, color, alpha);
        }
    }
}

void zd_canvas_blit_rgb565_alpha(zd_canvas_t *canvas, int x, int y, const uint16_t *pixels, const uint8_t *alpha,
                                 int width, int height)
{
    const size_t stride = ((size_t)width + 1) / 2;

    for (int row = 0; row < height; ++row) {
        const uint8_t *packed = alpha + (size_t)row * stride;
        for (int col = 0; col < width; ++col) {
            const uint8_t byte = packed[col / 2];
            const uint8_t coverage = (col & 1) == 0 ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
            blend_pixel(canvas, x + col, y + row, pixels[(size_t)row * width + col], coverage);
        }
    }
}

void zd_canvas_blit_alpha_mask(zd_canvas_t *canvas, int x, int y, const uint8_t *alpha, int width, int height,
                               uint16_t color)
{
    const size_t stride = ((size_t)width + 1) / 2;

    for (int row = 0; row < height; ++row) {
        const uint8_t *packed = alpha + (size_t)row * stride;
        for (int col = 0; col < width; ++col) {
            const uint8_t byte = packed[col / 2];
            const uint8_t coverage = (col & 1) == 0 ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
            blend_pixel(canvas, x + col, y + row, color, coverage);
        }
    }
}

// -- Widgets ----------------------------------------------------------------

void zd_canvas_draw_progress_track(zd_canvas_t *canvas, int x, int y, int cells, int first_on, int on_count,
                                   int cell_width, int cell_height, int gap, int radius, uint16_t on_color,
                                   uint16_t off_color)
{
    for (int cell = 0; cell < cells; ++cell) {
        const int cell_x = x + cell * (cell_width + gap);
        const bool on = cell >= first_on && cell < first_on + on_count;
        zd_canvas_fill_round_rect(canvas, cell_x, y, cell_width, cell_height, radius, on ? on_color : off_color);
    }
}
