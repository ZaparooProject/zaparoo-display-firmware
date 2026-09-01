// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "ui/scenes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ui/logo.h"
#include "ui/theme.h"

// Scene composition.
//
// Nothing drawn here is pressable: input arrives from a key on the board or from
// the host, never from a target on screen. Containers are therefore a fill and a
// radius, with no border and no raised edge. Tokens live in ui/theme.h.
//
// The logo appears on the idle screen only. Once a session is running the panel
// is 320 pixels tall and every one of them spent on chrome is artwork lost.
//
// The playing scene's three views differ in how much text competes with the
// artwork, not in how large the artwork is. The panel's 320px height is the
// binding constraint, so even a full-canvas view could only grow a cover by
// about a tenth; every view draws the one bitmap the host sent, and the art view
// gives it the screen to itself.

// Width the "No cover" plate takes when the host has sent no artwork. A 2:3
// portrait, which is the commonest box-art shape across systems.
#define ZD_COVER_PLACEHOLDER_W 192

// Idle: a centred mark over a rule over one status line.
#define IDLE_LOGO_Y 94
#define IDLE_RULE_Y 174
#define IDLE_STATUS_CAP_TOP 200

// Playing: artwork out to the safe edge, detail column beside whatever width
// the artwork turns out to need.
#define COVER_X ZD_SAFE_X
#define COVER_Y ZD_SAFE_Y

// The widest artwork the device accepts still has to leave a readable detail
// column. Checked here so raising ZD_COVER_MAX_WIDTH fails the build rather
// than quietly shipping a squashed layout.
_Static_assert(ZD_CONTENT_RIGHT - (COVER_X + ZD_COVER_MAX_WIDTH + ZD_PAD_GUTTER) >= ZD_DETAIL_MIN_W,
               "maximum cover width leaves the detail column narrower than ZD_DETAIL_MIN_W");
#define DETAIL_SYSTEM_CAP_TOP 66
#define DETAIL_TITLE_CAP_TOP 117
#define DETAIL_STATUS_CAP_TOP 183
#define DETAIL_MESSAGE_CAP_TOP 229

// Meta: the same header and title, then catalogue rows in a label/value grid.
// The title drops a rung from hero because this view has more to say.
#define META_TITLE_CAP_TOP 112
#define META_ROW_FIRST_CAP_TOP 172
#define META_ROW_PITCH 38
#define META_LABEL_W 116

// Toast: a transient pill along the bottom, over whatever scene is current. It
// is sized to its text rather than spanning the content width -- a full-width
// bar cuts straight across the artwork and reads as a rendering fault, where a
// pill floating over it reads as an overlay.
#define TOAST_H 46
#define TOAST_Y (ZD_SCREEN_H - ZD_SAFE_Y - TOAST_H)
#define TOAST_PAD_X 24
#define TOAST_MAX_W (ZD_CONTENT_W - 2 * ZD_PAD_GUTTER)

// Idle clock: the time is the content, so it takes the hero rung.
#define IDLE_CLOCK_CAP_TOP 108
#define IDLE_CLOCK_STATUS_CAP_TOP 196

// Launching and error share one panel so a launch that fails does not
// rearrange the screen underneath the reader.
#define PANEL_HEADING_CAP_TOP (ZD_PANEL_Y + ZD_PAD_PANEL)
#define PANEL_TITLE_CAP_TOP (PANEL_HEADING_CAP_TOP + 51)
#define PANEL_STATUS_CAP_TOP (PANEL_HEADING_CAP_TOP + 117)
#define PANEL_TRACK_Y (PANEL_HEADING_CAP_TOP + 163)
#define ERROR_MESSAGE_CAP_TOP (PANEL_HEADING_CAP_TOP + 51)
#define ERROR_DETAIL_CAP_TOP (PANEL_HEADING_CAP_TOP + 108)
#define ERROR_ADVICE_CAP_TOP (PANEL_HEADING_CAP_TOP + 154)

void zapdisplay_state_set_defaults(zapdisplay_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->scene = ZAPDISPLAY_SCENE_IDLE;
    strlcpy(state->status, "Waiting for a host", sizeof(state->status));
    state->accent_color = ZD_C_ACCENT;
    state->view = ZAPDISPLAY_VIEW_DETAIL;
    // There is no RTC. Until a host sets the clock the time is genuinely
    // unknown, which is different from midnight.
    state->clock_minutes = -1;
}

const char *zapdisplay_view_name(zapdisplay_view_t view)
{
    switch (view) {
    case ZAPDISPLAY_VIEW_ART:
        return "art";
    case ZAPDISPLAY_VIEW_DETAIL:
        return "detail";
    case ZAPDISPLAY_VIEW_META:
        return "meta";
    default:
        return "unknown";
    }
}

const char *zapdisplay_scene_name(zapdisplay_scene_t scene)
{
    switch (scene) {
    case ZAPDISPLAY_SCENE_IDLE:
        return "idle";
    case ZAPDISPLAY_SCENE_LAUNCHING:
        return "launching";
    case ZAPDISPLAY_SCENE_PLAYING:
        return "playing";
    case ZAPDISPLAY_SCENE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static const char *field(const char *value, const char *fallback)
{
    return value[0] != '\0' ? value : fallback;
}

// format_duration writes a session length a person can read at a glance. Hours
// and whole minutes only: seconds would redraw the whole framebuffer sixty
// times more often to tell nobody anything.
static void format_duration(char *buf, size_t size, uint32_t seconds)
{
    const uint32_t minutes = seconds / 60;

    if (minutes < 60) {
        snprintf(buf, size, "%lum", (unsigned long)minutes);
    } else {
        snprintf(buf, size, "%luh %02lum", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
    }
}

// 24-hour, because a 12-hour clock needs an am/pm marker this layout has no
// room for and the ambiguity is worse than the unfamiliarity.
static void format_clock(char *buf, size_t size, int minutes_since_midnight)
{
    snprintf(buf, size, "%02d:%02d", minutes_since_midnight / 60, minutes_since_midnight % 60);
}

// Draws text ending at right_x. There is no right-aligned primitive because
// this is the only place that needs one.
static void draw_text_right(zd_canvas_t *canvas, int right_x, int baseline_y, const zd_font_t *font, const char *utf8,
                            uint16_t color)
{
    zd_canvas_draw_text(canvas, right_x - zd_canvas_text_width(font, utf8), baseline_y, font, utf8, color);
}

typedef struct {
    int x;
    int y;
} retention_offset_t;

// Every scene can sit unchanged for hours, so all of them nudge around a small
// cycle of offsets rather than resting on the same pixels. Idle needs this most
// of all: it is the state the panel sits in longest and it carries the largest
// static graphic on the device.
//
// The phase is a counter the tick advances, not a function of uptime, so the
// content keeps moving with no host attached. See ui/tick.c.
static unsigned s_retention_phase;

static retention_offset_t retention_offset(void)
{
    static const retention_offset_t offsets[] = {
        {0, 0},
        {ZD_RETENTION_STEP, 0},
        {ZD_RETENTION_STEP, ZD_RETENTION_STEP},
        {0, ZD_RETENTION_STEP},
        {-ZD_RETENTION_STEP, ZD_RETENTION_STEP},
        {-ZD_RETENTION_STEP, 0},
        {-ZD_RETENTION_STEP, -ZD_RETENTION_STEP},
        {0, -ZD_RETENTION_STEP},
        {ZD_RETENTION_STEP, -ZD_RETENTION_STEP},
    };
    return offsets[s_retention_phase % (sizeof(offsets) / sizeof(offsets[0]))];
}

void zd_scenes_retention_advance(void)
{
    s_retention_phase++;
}

unsigned zd_scenes_retention_phase(void)
{
    return s_retention_phase;
}

// -- Artwork ----------------------------------------------------------------

typedef struct {
    const uint16_t *pixels;
    int width;
    int height;
    bool present;
} artwork_t;

static artwork_t artwork_of(const zapdisplay_state_t *state)
{
    const bool present = state->cover_rgb565 != NULL && state->cover_width > 0 && state->cover_height > 0 &&
                         state->cover_width <= ZD_COVER_MAX_WIDTH && state->cover_height <= ZD_COVER_MAX_HEIGHT;

    return (artwork_t){
        .pixels = state->cover_rgb565,
        .width = present ? state->cover_width : ZD_COVER_PLACEHOLDER_W,
        .height = present ? state->cover_height : ZD_COVER_MAX_HEIGHT,
        .present = present,
    };
}

static void draw_artwork(zd_canvas_t *canvas, const artwork_t *art, int x, int y)
{
    if (art->present) {
        zd_canvas_blit_rgb565_round(canvas, x, y, art->pixels, art->width, art->height, ZD_RADIUS_MD);
        return;
    }
    zd_canvas_fill_round_rect(canvas, x, y, art->width, art->height, ZD_RADIUS_MD, ZD_C_BG_PANEL);
    zd_canvas_draw_text_centered(canvas, x + art->width / 2, zd_baseline_centered(&zd_font_caption, y, art->height),
                                 art->width, &zd_font_caption, "No cover", ZD_C_TEXT_LABEL);
}

// -- Idle -------------------------------------------------------------------

// The brand screen is the mark on the
// page with a single line under it. A second line of status text below the
// first read as clutter without adding anything the first did not.
static void draw_idle_brand(zd_canvas_t *canvas, const zapdisplay_state_t *state, retention_offset_t offset)
{
    const int logo_x = (ZD_SCREEN_W - zd_logo_256.width) / 2 + offset.x;

    zd_canvas_draw_logo(canvas, logo_x, IDLE_LOGO_Y + offset.y, &zd_logo_256, ZD_C_TEXT_PRIMARY);
    zd_canvas_fill_rect(canvas, logo_x, IDLE_RULE_Y + offset.y, zd_logo_256.width, ZD_STROKE_RULE, ZD_C_BORDER_MID);
    zd_canvas_draw_text_centered(
        canvas, ZD_SCREEN_W / 2 + offset.x, zd_baseline_for_cap_top(&zd_font_section, IDLE_STATUS_CAP_TOP + offset.y),
        ZD_CONTENT_W, &zd_font_section, field(state->status, "Waiting for a host"), ZD_C_TEXT_VARIANT);
}

static void draw_idle_clock(zd_canvas_t *canvas, const zapdisplay_state_t *state, retention_offset_t offset)
{
    char clock[8];

    format_clock(clock, sizeof(clock), state->clock_minutes);
    zd_canvas_draw_text_centered(canvas, ZD_SCREEN_W / 2 + offset.x,
                                 zd_baseline_for_cap_top(&zd_font_hero, IDLE_CLOCK_CAP_TOP + offset.y), ZD_CONTENT_W,
                                 &zd_font_hero, clock, ZD_C_TEXT_PRIMARY);
    zd_canvas_draw_text_centered(canvas, ZD_SCREEN_W / 2 + offset.x,
                                 zd_baseline_for_cap_top(&zd_font_caption, IDLE_CLOCK_STATUS_CAP_TOP + offset.y),
                                 ZD_CONTENT_W, &zd_font_caption, field(state->status, "Ready"), ZD_C_TEXT_LABEL);
}

// Reuses the artwork the device already holds rather than asking for more. The
// cover slot survives CLEAR, so whatever was last played is still there.
static void draw_idle_last_played(zd_canvas_t *canvas, const zapdisplay_state_t *state, retention_offset_t offset)
{
    const artwork_t art = artwork_of(state);
    const int art_y = COVER_Y + (ZD_COVER_MAX_HEIGHT - art.height) / 2 + offset.y;
    const int text_x = COVER_X + art.width + ZD_PAD_GUTTER + offset.x;
    const int text_w = ZD_CONTENT_RIGHT - (COVER_X + art.width + ZD_PAD_GUTTER);

    draw_artwork(canvas, &art, COVER_X + offset.x, art_y);
    zd_canvas_draw_section_header(
        canvas, text_x, zd_baseline_for_cap_top(&zd_font_caption, DETAIL_SYSTEM_CAP_TOP + offset.y), text_w,
        &zd_font_caption, "Last played", ZD_C_TEXT_LABEL, ZD_PAD_LABEL_RULE, ZD_STROKE_RULE, ZD_C_BORDER_MID);
    zd_canvas_draw_text_elided(canvas, text_x, zd_baseline_for_cap_top(&zd_font_title, DETAIL_TITLE_CAP_TOP + offset.y),
                               text_w, &zd_font_title, state->title, ZD_C_TEXT_PRIMARY);
    zd_canvas_draw_text_elided(canvas, text_x,
                               zd_baseline_for_cap_top(&zd_font_section, DETAIL_STATUS_CAP_TOP + offset.y), text_w,
                               &zd_font_section, state->system, ZD_C_TEXT_VARIANT);
}

// Idle rotates rather than resting on one composition: it is the state the
// panel sits in longest, so a fixed frame here is the worst thing on the
// device for retention. A screen with nothing to say falls back to the brand
// rather than showing an empty frame -- the clock is unknown until a host sets
// it, and there is no last-played before the first launch.
static void draw_idle(zd_canvas_t *canvas, const zapdisplay_state_t *state)
{
    const retention_offset_t offset = retention_offset();

    zd_canvas_fill_active(canvas, ZD_C_BG_DEEP);

    switch (state->idle_screen) {
    case ZAPDISPLAY_IDLE_CLOCK:
        if (state->clock_minutes >= 0) {
            draw_idle_clock(canvas, state, offset);
            return;
        }
        break;
    case ZAPDISPLAY_IDLE_LAST_PLAYED:
        if (state->cover_rgb565 != NULL && state->title[0] != '\0') {
            draw_idle_last_played(canvas, state, offset);
            return;
        }
        break;
    case ZAPDISPLAY_IDLE_BRAND:
    default:
        break;
    }

    draw_idle_brand(canvas, state, offset);
}

// -- Playing ----------------------------------------------------------------

// Artwork alone, centred on the page, with nothing competing with it.
static void draw_playing_art(zd_canvas_t *canvas, const zapdisplay_state_t *state, retention_offset_t offset)
{
    const artwork_t art = artwork_of(state);

    draw_artwork(canvas, &art, (ZD_SCREEN_W - art.width) / 2 + offset.x, (ZD_SCREEN_H - art.height) / 2 + offset.y);
}

// draw_meta_rows lays out whatever catalogue fields the host actually sent,
// packed upward. Most systems fill only some of them, and leaving a gap where
// a missing field would have been reads as a rendering fault.
static void draw_meta_rows(zd_canvas_t *canvas, const zapdisplay_state_t *state, int x, int width, int cap_top)
{
    static const char *const LABELS[] = {"Year", "Developer", "Genre", "Players"};
    const char *const values[] = {state->year, state->developer, state->genre, state->players};
    const int value_x = x + META_LABEL_W;
    const int value_w = width - META_LABEL_W;
    int row = 0;

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (values[i][0] == '\0') {
            continue;
        }
        const int y = cap_top + row * META_ROW_PITCH;
        zd_canvas_draw_text(canvas, x, zd_baseline_for_cap_top(&zd_font_caption, y), &zd_font_caption, LABELS[i],
                            ZD_C_TEXT_LABEL);
        zd_canvas_draw_text_elided(canvas, value_x, zd_baseline_for_cap_top(&zd_font_body, y), value_w, &zd_font_body,
                                   values[i], ZD_C_TEXT_PRIMARY);
        row++;
    }

    if (row == 0) {
        zd_canvas_draw_text(canvas, x, zd_baseline_for_cap_top(&zd_font_body, cap_top), &zd_font_body,
                            "No details for this title", ZD_C_TEXT_LABEL);
    }
}

static void draw_playing_columns(zd_canvas_t *canvas, const zapdisplay_state_t *state, retention_offset_t offset,
                                 const artwork_t *art, int text_x, int text_w)
{
    const int art_y = COVER_Y + (ZD_COVER_MAX_HEIGHT - art->height) / 2 + offset.y;

    draw_artwork(canvas, art, COVER_X + offset.x, art_y);
    zd_canvas_draw_section_header(canvas, text_x,
                                  zd_baseline_for_cap_top(&zd_font_caption, DETAIL_SYSTEM_CAP_TOP + offset.y), text_w,
                                  &zd_font_caption, field(state->system, "Unknown system"), ZD_C_TEXT_LABEL,
                                  ZD_PAD_LABEL_RULE, ZD_STROKE_RULE, ZD_C_BORDER_MID);

    if (state->view == ZAPDISPLAY_VIEW_META) {
        zd_canvas_draw_text_elided(canvas, text_x,
                                   zd_baseline_for_cap_top(&zd_font_title, META_TITLE_CAP_TOP + offset.y), text_w,
                                   &zd_font_title, field(state->title, "Untitled"), ZD_C_TEXT_PRIMARY);
        draw_meta_rows(canvas, state, text_x, text_w, META_ROW_FIRST_CAP_TOP + offset.y);
        return;
    }

    // Step the title down one rung before eliding: the field holds 63 bytes and
    // the smaller rung fits most of them, so a long name reads in full rather
    // than being cut off at hero size.
    const char *title = field(state->title, "Untitled");
    const zd_font_t *title_font = &zd_font_hero;
    if (zd_canvas_text_width(title_font, title) > text_w) {
        title_font = &zd_font_title;
    }
    zd_canvas_draw_text_elided(canvas, text_x, zd_baseline_for_cap_top(title_font, DETAIL_TITLE_CAP_TOP + offset.y),
                               text_w, title_font, title, ZD_C_TEXT_PRIMARY);

    // Elapsed sits opposite the status on the same baseline, so the row reads
    // as "what it is doing" and "for how long" without a label.
    int status_w = text_w;
    if (state->has_elapsed) {
        char elapsed[16];
        format_duration(elapsed, sizeof(elapsed), state->elapsed_seconds);
        const int elapsed_w = zd_canvas_text_width(&zd_font_section, elapsed);
        draw_text_right(canvas, text_x + text_w,
                        zd_baseline_for_cap_top(&zd_font_section, DETAIL_STATUS_CAP_TOP + offset.y), &zd_font_section,
                        elapsed, ZD_C_TEXT_LABEL);
        status_w = text_w - elapsed_w - ZD_PAD_GUTTER;
    }

    zd_canvas_draw_text_elided(canvas, text_x,
                               zd_baseline_for_cap_top(&zd_font_section, DETAIL_STATUS_CAP_TOP + offset.y), status_w,
                               &zd_font_section, field(state->status, "Playing"), ZD_C_TEXT_VARIANT);
    zd_canvas_draw_text_elided(canvas, text_x,
                               zd_baseline_for_cap_top(&zd_font_body, DETAIL_MESSAGE_CAP_TOP + offset.y), text_w,
                               &zd_font_body, state->message, ZD_C_TEXT_LABEL);
}

// Playing gives the artwork the full height of the safe area and nothing else
// competes with it: no header, no bar, and no plate under the art. Cover art is
// the one thing on this screen a person actually looks at.
//
// The artwork is not a fixed slot. Box art is portrait on some systems and
// landscape on others, so the host sends its own aspect and the detail column
// takes whatever is left. A wide cover therefore reads as wide rather than
// being letterboxed into a portrait hole.
static void draw_playing(zd_canvas_t *canvas, const zapdisplay_state_t *state)
{
    const retention_offset_t offset = retention_offset();
    const artwork_t art = artwork_of(state);
    const int text_x = COVER_X + art.width + ZD_PAD_GUTTER + offset.x;
    const int text_w = ZD_CONTENT_RIGHT - (COVER_X + art.width + ZD_PAD_GUTTER);

    zd_canvas_fill_active(canvas, ZD_C_BG_DEEP);

    // Artwork too wide to leave a readable column gets the art view whether it
    // was asked for or not. Cropping the cover or squeezing the text to fit
    // would both look like a bug; showing the artwork alone looks deliberate.
    //
    // The static assert below means this cannot currently trigger. It is kept
    // because the alternative to a cheap guard is a layout that breaks silently
    // the day the artwork bounds grow.
    if (state->view == ZAPDISPLAY_VIEW_ART || text_w < ZD_DETAIL_MIN_W) {
        draw_playing_art(canvas, state, offset);
        return;
    }

    draw_playing_columns(canvas, state, offset, &art, text_x, text_w);
}

// The panel is modal chrome: a fill and a radius, no border. An
// error is signalled by the colour of its heading, not by outlining the box.
static void draw_panel(zd_canvas_t *canvas, const char *heading, uint16_t heading_color, retention_offset_t offset)
{
    zd_canvas_fill_active(canvas, ZD_C_BG_DEEP);
    zd_canvas_fill_round_rect(canvas, ZD_PANEL_X + offset.x, ZD_PANEL_Y + offset.y, ZD_PANEL_W, ZD_PANEL_H,
                              ZD_RADIUS_MD, ZD_C_BG_PANEL);
    zd_canvas_draw_section_header(canvas, ZD_PANEL_INNER_X + offset.x,
                                  zd_baseline_for_cap_top(&zd_font_caption, PANEL_HEADING_CAP_TOP + offset.y),
                                  ZD_PANEL_INNER_W, &zd_font_caption, heading, heading_color, ZD_PAD_LABEL_RULE,
                                  ZD_STROKE_RULE, ZD_C_BORDER_MID);
}

static void draw_launching(zd_canvas_t *canvas, const zapdisplay_state_t *state)
{
    const retention_offset_t offset = retention_offset();

    draw_panel(canvas, field(state->system, "Unknown system"), ZD_C_TEXT_LABEL, offset);

    zd_canvas_draw_text_elided(canvas, ZD_PANEL_INNER_X + offset.x,
                               zd_baseline_for_cap_top(&zd_font_hero, PANEL_TITLE_CAP_TOP + offset.y), ZD_PANEL_INNER_W,
                               &zd_font_hero, field(state->title, "Untitled"), ZD_C_TEXT_PRIMARY);
    // Falls back through message so a host that sets only one still reads.
    zd_canvas_draw_text_elided(
        canvas, ZD_PANEL_INNER_X + offset.x, zd_baseline_for_cap_top(&zd_font_section, PANEL_STATUS_CAP_TOP + offset.y),
        ZD_PANEL_INNER_W, &zd_font_section, field(state->status, field(state->message, "Starting")), ZD_C_TEXT_VARIANT);
    zd_canvas_draw_progress_track(canvas, ZD_PANEL_INNER_X + offset.x, PANEL_TRACK_Y + offset.y, ZD_TRACK_CELLS,
                                  ZD_TRACK_FILLED, ZD_TRACK_CELL_W, ZD_TRACK_CELL_H, ZD_TRACK_GAP, ZD_RADIUS_SM,
                                  zd_theme_accent(state->accent_color, state->has_accent), ZD_C_BORDER_SUBTLE);
}

static void draw_error(zd_canvas_t *canvas, const zapdisplay_state_t *state)
{
    const retention_offset_t offset = retention_offset();

    draw_panel(canvas, "Something went wrong", ZD_C_ERROR, offset);

    zd_canvas_draw_text_elided(
        canvas, ZD_PANEL_INNER_X + offset.x, zd_baseline_for_cap_top(&zd_font_title, ERROR_MESSAGE_CAP_TOP + offset.y),
        ZD_PANEL_INNER_W, &zd_font_title, field(state->message, "The host reported a failure"), ZD_C_TEXT_PRIMARY);
    zd_canvas_draw_text_elided(canvas, ZD_PANEL_INNER_X + offset.x,
                               zd_baseline_for_cap_top(&zd_font_section, ERROR_DETAIL_CAP_TOP + offset.y),
                               ZD_PANEL_INNER_W, &zd_font_section, state->status, ZD_C_TEXT_VARIANT);
    zd_canvas_draw_text_elided(canvas, ZD_PANEL_INNER_X + offset.x,
                               zd_baseline_for_cap_top(&zd_font_body, ERROR_ADVICE_CAP_TOP + offset.y),
                               ZD_PANEL_INNER_W, &zd_font_body, "Check Zaparoo Core", ZD_C_TEXT_LABEL);
}

// A toast is a transient band over whatever is already on screen. It composes
// with every scene, which is why token scan feedback is an overlay rather than
// a scene of its own: a scan should not throw away what you were looking at,
// and it must not need a state machine to put it back.
static void draw_toast(zd_canvas_t *canvas, const zapdisplay_state_t *state)
{
    const retention_offset_t offset = retention_offset();
    const int text_w = zd_canvas_text_width(&zd_font_body, state->toast);
    int width = text_w + 2 * TOAST_PAD_X;

    if (width > TOAST_MAX_W) {
        width = TOAST_MAX_W;
    }

    const int x = (ZD_SCREEN_W - width) / 2 + offset.x;
    const int y = TOAST_Y + offset.y;

    zd_canvas_fill_round_rect(canvas, x, y, width, TOAST_H, ZD_RADIUS_MD, ZD_C_SURFACE_CARD);
    zd_canvas_draw_text_centered(canvas, x + width / 2, zd_baseline_centered(&zd_font_body, y, TOAST_H),
                                 width - 2 * TOAST_PAD_X, &zd_font_body, state->toast, ZD_C_TEXT_PRIMARY);
}

void zd_scenes_draw(zd_canvas_t *canvas, const zapdisplay_state_t *state)
{
    switch (state->scene) {
    case ZAPDISPLAY_SCENE_LAUNCHING:
        draw_launching(canvas, state);
        break;
    case ZAPDISPLAY_SCENE_PLAYING:
        draw_playing(canvas, state);
        break;
    case ZAPDISPLAY_SCENE_ERROR:
        draw_error(canvas, state);
        break;
    case ZAPDISPLAY_SCENE_IDLE:
    default:
        draw_idle(canvas, state);
        break;
    }

    if (state->has_toast && state->toast[0] != '\0') {
        draw_toast(canvas, state);
    }
}
