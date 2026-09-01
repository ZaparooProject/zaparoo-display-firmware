// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Design tokens for the Zaparoo display accessory.
//
// Colours are the `zaparoo-dark` scheme shared with the Zaparoo app, quantised
// to RGB565. The app derives these roles at runtime from three authored
// colours; the firmware ships the resolved result instead, because it renders
// one scheme and has no reason to carry the colour-space solvers.
//
// Scene code must use these names rather than literals so the palette stays in
// one place. Only the roles this device actually draws are defined; selection,
// on-accent and marker roles describe interaction states nothing here has.

// -- Colour roles (RGB565) --------------------------------------------------

#define ZD_C_BG_DEEP 0x0021       // #050608 flat page background
#define ZD_C_BG_PANEL 0x10A3      // #11171d modal/menu panel
#define ZD_C_SURFACE_CARD 0x18E4  // #181f26 card and control face
#define ZD_C_BORDER_SUBTLE 0x2146 // #262b30 low-contrast edges and keylines
#define ZD_C_BORDER_MID 0x4AAB    // #4f565c resting card edge, section rules
#define ZD_C_TEXT_PRIMARY 0xF7BE  // #f7f7f5 primary content
#define ZD_C_TEXT_LABEL 0x9CD3    // #9b9b9b secondary content
#define ZD_C_TEXT_VARIANT 0x8494  // #8091a1 tinted secondary content
#define ZD_C_ACCENT 0x145F        // #168bff highlight; host-overridable
#define ZD_C_ERROR 0xFA72         // #ff4f91 failure text; deliberately not red

// The design system splits surfaces into two registers: a physical one for
// things you press, and a typographic one for things you only read.
// Nothing on this panel is pressable -- the device does take input, but from a
// key on its edge and from the host, never from a target drawn on screen -- so
// the physical register has nothing to describe here and is deliberately
// absent. `tileEdge` and `controlEdge` are therefore not defined, and
// containers use modal panel chrome instead: ZD_C_BG_PANEL, ZD_RADIUS_MD, and
// no border.

// -- Shape and stroke -------------------------------------------------------
//
// The design system keys its radius and stroke tiers on logical height, as a
// proxy for a ~96 PPI display. This panel is 320 logical pixels tall but
// 221 PPI, so that table would select a 2px radius: around 0.23mm, and
// invisible. These are picked for physical size instead.

#define ZD_RADIUS_MD 6
#define ZD_RADIUS_SM 3

// Section rules. The design system uses a 1px hairline; at 221 PPI that is
// sub-visible, and it shimmers as the retention offset moves it.
#define ZD_STROKE_RULE 2

// -- Screen and padding -----------------------------------------------------
//
// The logical UI is landscape 960x320; the canvas rotates it onto the portrait
// panel. See ui/canvas.h.

#define ZD_SCREEN_W 960
#define ZD_SCREEN_H 320

// Which way up that landscape space is drawn. 0 is the panel's own orientation;
// 180 flips it for a board mounted the other way up. The logical space is
// identical either way, which is why no scene knows about it and why 90 and 270
// are not offered. See ui/canvas.h.
#define ZD_ROTATION_DEFAULT 0

// Outer safe area. Cover art is pushed out to this edge, because on a 320px
// panel every pixel spent on chrome is a pixel of artwork lost.
#define ZD_SAFE_X 16
#define ZD_SAFE_Y 16

// The detail column never narrows past this, which is what caps how wide a
// piece of artwork is allowed to grow.
#define ZD_DETAIL_MIN_W 360

// Text columns and panels sit in from the safe area so a run of text does not
// start hard against the bezel.
#define ZD_PAD_SCREEN_X 24
#define ZD_PAD_PANEL 28
#define ZD_PAD_GUTTER 28
#define ZD_PAD_LABEL_RULE 6

#define ZD_CONTENT_X ZD_PAD_SCREEN_X
#define ZD_CONTENT_RIGHT (ZD_SCREEN_W - ZD_PAD_SCREEN_X)
#define ZD_CONTENT_W (ZD_CONTENT_RIGHT - ZD_CONTENT_X)

// Panel card used by the launching and error scenes: modal chrome, which is a
// fill and a radius with no border and no raised edge.
#define ZD_PANEL_X ZD_CONTENT_X
#define ZD_PANEL_W ZD_CONTENT_W
#define ZD_PANEL_H 233
#define ZD_PANEL_Y ((ZD_SCREEN_H - ZD_PANEL_H) / 2)
#define ZD_PANEL_INNER_X (ZD_PANEL_X + ZD_PAD_PANEL)
#define ZD_PANEL_INNER_W (ZD_PANEL_W - 2 * ZD_PAD_PANEL)

// -- Progress track ---------------------------------------------------------
//
// Discrete cells rather than a continuous fill: a continuous fill's right edge
// lands on a fractional pixel.
//
// One cell is lit and marches left to right, wrapping. This is indeterminate on
// purpose: the device is told a launch has started, not how long it will take,
// and a bar that fills to the end and then sits there while the game is still
// loading says something untrue. The app's track does the same thing while the
// database optimises.
//
// A quarter second a step, so one pass across the track takes three seconds:
// slow enough to read as deliberate, quick enough to look alive.
//
// The interval is affordable only because a step repaints this band alone. A
// full frame measures around 540 ms on this panel, twice the interval; pushing
// one per step took a 227 KB cover upload from 4.8 s to 8.6 s. See
// zd_hd458002c40_render_progress.

#define ZD_TRACK_CELLS 12
#define ZD_TRACK_CELL_W 22
#define ZD_TRACK_CELL_H 14
#define ZD_TRACK_GAP 4
#define ZD_TRACK_MARCH_US (250 * 1000)

// -- Image retention --------------------------------------------------------
//
// Any scene can sit unchanged for hours, so content shifts around a small cycle
// of offsets rather than resting on the same pixels. The panel is an IPS TFT,
// where retention is transient rather than the permanent emitter wear an OLED
// suffers, so this is cheap insurance rather than the main defence -- dimming
// and blanking on idle do the real work. See ui/tick.h.
//
// The phase is a counter advanced by the tick, not a function of uptime, so it
// keeps moving with no host attached and does not restart at centre on reboot.

#define ZD_RETENTION_STEP 2
#define ZD_RETENTION_PERIOD_US (120 * 1000 * 1000)

// -- Idle behaviour ---------------------------------------------------------
//
// A display left at full brightness on a frame that never changes is the one
// case that can actually mark an LCD, and it is also how a dead host leaves the
// panel. Both are the same timeout.

#define ZD_IDLE_DIM_AFTER_S_DEFAULT 300
#define ZD_IDLE_BLANK_AFTER_S_DEFAULT 1800
#define ZD_BRIGHTNESS_DEFAULT 100
#define ZD_DIM_LEVEL_DEFAULT 25

// How long the host has to be silent before the device stops trusting what is
// on screen and falls back to idle. Deliberately separate from the blank
// timeout: turning blanking off should not also lose the stale-frame recovery.
#define ZD_HOST_ABSENT_AFTER_S_DEFAULT 600

// Which parts of the idle frame may appear. All three by default: the clock
// stays dark until a host sets the time and last-played until something has
// been launched, so enabling them costs nothing when unused.
#define ZD_IDLE_SCREENS_DEFAULT 0x7

// Idle column rhythm. Each gap is measured from the previous row's cap bottom
// to the next row's cap top, and the group is then centred in the safe area, so
// a device that knows less draws fewer rows without the column hanging from the
// top of the panel. See ui/scenes.c.
#define ZD_IDLE_GAP_STATUS 22
#define ZD_IDLE_GAP_SECTION 34
#define ZD_IDLE_GAP_TITLE 22
#define ZD_IDLE_GAP_ROW 14

// Narrowest the idle column is allowed to get. A device showing only a clock
// and a status word would otherwise draw a rule barely wider than the word.
#define ZD_IDLE_COLUMN_MIN_W 280

// Brightness ramps. Long enough to read as deliberate, short enough that waking
// the panel does not feel sluggish.
#define ZD_FADE_DIM_MS 800
#define ZD_FADE_WAKE_MS 150

// -- Helpers ----------------------------------------------------------------

// Rec.709 luma of an RGB565 colour, 0-255. Used only to decide whether a
// colour reads against the page, not to measure contrast precisely.
static inline uint32_t zd_theme_luma565(uint16_t color)
{
    const uint32_t r = ((color >> 11) & 0x1F) * 255u / 31u;
    const uint32_t g = ((color >> 5) & 0x3F) * 255u / 63u;
    const uint32_t b = (color & 0x1F) * 255u / 31u;
    return (77u * r + 150u * g + 29u * b) >> 8;
}

// The accent to draw with. A host ACCENT overrides the accent role only, never
// a background, border, edge or text colour. Near-black accents are rejected:
// they would silently blank whatever they filled.
static inline uint16_t zd_theme_accent(uint16_t accent, bool has_accent)
{
    if (!has_accent || zd_theme_luma565(accent) < 48) {
        return ZD_C_ACCENT;
    }
    return accent;
}
