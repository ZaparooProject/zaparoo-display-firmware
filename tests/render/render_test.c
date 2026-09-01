// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

// Scene rendering regression tests.
//
// These compile the firmware's real canvas and scene code for the host and draw
// into an ordinary buffer, so layout is checked in pixels without a panel
// attached. That is possible because scenes are pure functions of
// zapdisplay_state_t: they call no driver, read no clock and touch no hardware.
//
// The checks are structural rather than golden images. A golden image fails on
// every deliberate design change and tells you nothing about why; these assert
// the properties that must hold whatever the design looks like.

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ui/canvas.h"
#include "ui/scenes.h"
#include "ui/theme.h"

// Panel geometry, mirroring main/display/hd458002c40.c. The framebuffer is
// wider than the visible panel and the logical UI is rotated onto it.
#define ACTIVE_W 400
#define ACTIVE_H 960
#define VISIBLE_W 320
#define OVERSCAN 80

// Retention offsets cycle through nine positions; every check sweeps all of
// them, because a layout that only clears the edges at rest is not safe.
#define RETENTION_POSITIONS 9

static uint16_t framebuffer[ACTIVE_W * ACTIVE_H];
static uint16_t cover[ZD_COVER_MAX_WIDTH * ZD_COVER_MAX_HEIGHT];

static int failures;
static int checks;

static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void check(bool ok, const char *fmt, ...)
{
    va_list args;

    checks++;
    if (ok) {
        return;
    }
    failures++;
    fputs("  FAIL ", stdout);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

static zd_canvas_t make_canvas(void)
{
    return (zd_canvas_t){
        .pixels = framebuffer,
        .active_width = ACTIVE_W,
        .active_height = ACTIVE_H,
        .visible_width = VISIBLE_W,
        .visible_height = ACTIVE_H,
        .logical_width = ACTIVE_H,
        .logical_height = VISIBLE_W,
        .overscan_left = OVERSCAN,
    };
}

// Reads a logical pixel back through the same rotation the canvas writes with.
static uint16_t logical_pixel(int x, int y)
{
    return framebuffer[(size_t)x * ACTIVE_W + OVERSCAN + (VISIBLE_W - 1 - y)];
}

static bool is_background(uint16_t pixel)
{
    return pixel == ZD_C_BG_DEEP;
}

// -- Fixtures ---------------------------------------------------------------

static void fill_cover(uint16_t color, int width, int height)
{
    for (int i = 0; i < width * height; i++) {
        cover[i] = color;
    }
}

static void populated_state(zapdisplay_state_t *state)
{
    zapdisplay_state_set_defaults(state);
    snprintf(state->title, sizeof(state->title), "A Reasonably Long Game Title");
    snprintf(state->system, sizeof(state->system), "Nintendo Entertainment System");
    snprintf(state->status, sizeof(state->status), "Playing");
    snprintf(state->message, sizeof(state->message), "Secondary message line");
    snprintf(state->year, sizeof(state->year), "1998");
    snprintf(state->developer, sizeof(state->developer), "Rareware Limited");
    snprintf(state->genre, sizeof(state->genre), "Platformer");
    snprintf(state->players, sizeof(state->players), "1-4");
    state->has_elapsed = true;
    state->elapsed_seconds = 4980;
    state->clock_minutes = 875;
}

static void attach_cover(zapdisplay_state_t *state, int width, int height)
{
    fill_cover(0xF800, width, height);
    state->cover_rgb565 = cover;
    state->cover_width = (uint16_t)width;
    state->cover_height = (uint16_t)height;
}

// -- Measurements -----------------------------------------------------------

typedef struct {
    int min_x, min_y, max_x, max_y;
    bool any;
} ink_bounds_t;

// Bounding box of everything drawn over the page background.
static ink_bounds_t ink_bounds(void)
{
    ink_bounds_t b = {ZD_SCREEN_W, ZD_SCREEN_H, -1, -1, false};

    for (int y = 0; y < ZD_SCREEN_H; y++) {
        for (int x = 0; x < ZD_SCREEN_W; x++) {
            if (is_background(logical_pixel(x, y))) {
                continue;
            }
            b.any = true;
            if (x < b.min_x)
                b.min_x = x;
            if (y < b.min_y)
                b.min_y = y;
            if (x > b.max_x)
                b.max_x = x;
            if (y > b.max_y)
                b.max_y = y;
        }
    }
    return b;
}

static bool frames_identical(const uint16_t *a, const uint16_t *b)
{
    return memcmp(a, b, sizeof(framebuffer)) == 0;
}

// -- Cases ------------------------------------------------------------------

typedef struct {
    const char *name;
    zapdisplay_scene_t scene;
    zapdisplay_view_t view;
    zapdisplay_idle_screen_t idle_screen;
} scene_case_t;

static const scene_case_t CASES[] = {
    {"idle/brand", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, ZAPDISPLAY_IDLE_BRAND},
    {"idle/clock", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, ZAPDISPLAY_IDLE_CLOCK},
    {"idle/last-played", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, ZAPDISPLAY_IDLE_LAST_PLAYED},
    {"launching", ZAPDISPLAY_SCENE_LAUNCHING, ZAPDISPLAY_VIEW_DETAIL, ZAPDISPLAY_IDLE_BRAND},
    {"error", ZAPDISPLAY_SCENE_ERROR, ZAPDISPLAY_VIEW_DETAIL, ZAPDISPLAY_IDLE_BRAND},
    {"playing/art", ZAPDISPLAY_SCENE_PLAYING, ZAPDISPLAY_VIEW_ART, ZAPDISPLAY_IDLE_BRAND},
    {"playing/detail", ZAPDISPLAY_SCENE_PLAYING, ZAPDISPLAY_VIEW_DETAIL, ZAPDISPLAY_IDLE_BRAND},
    {"playing/meta", ZAPDISPLAY_SCENE_PLAYING, ZAPDISPLAY_VIEW_META, ZAPDISPLAY_IDLE_BRAND},
};

// Cover aspects that actually turn up: portrait Japanese box art, landscape US
// N64 art, a 4:3 screenshot, and a 3D box render too wide for a text column.
static const struct {
    const char *name;
    int width, height;
} COVERS[] = {
    {"portrait", 192, 288},   {"japanese box art", 211, 288}, {"us box art", 394, 288},
    {"screenshot", 384, 288}, {"wide render", 512, 276},
};

static void draw_case(zapdisplay_state_t *state, const scene_case_t *c)
{
    zd_canvas_t canvas = make_canvas();

    state->scene = c->scene;
    state->view = c->view;
    state->idle_screen = c->idle_screen;
    zd_scenes_draw(&canvas, state);
}

// Nothing may sit within the safe margin of a canvas edge, at any retention
// offset. This is what stops content clipping when the anti-retention shift
// moves it.
static void test_edge_clearance(void)
{
    puts("edge clearance across every scene, view and retention offset");

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        for (size_t j = 0; j < sizeof(COVERS) / sizeof(COVERS[0]); j++) {
            for (int phase = 0; phase < RETENTION_POSITIONS; phase++) {
                zapdisplay_state_t state;
                populated_state(&state);
                attach_cover(&state, COVERS[j].width, COVERS[j].height);
                state.has_toast = true;
                snprintf(state.toast, sizeof(state.toast), "Scanned  A Reasonably Long Game Title");
                draw_case(&state, &CASES[i]);

                const ink_bounds_t b = ink_bounds();
                check(b.any, "%s/%s phase %d drew nothing at all", CASES[i].name, COVERS[j].name, phase);
                if (!b.any) {
                    zd_scenes_retention_advance();
                    continue;
                }
                const int clearance_left = b.min_x;
                const int clearance_top = b.min_y;
                const int clearance_right = ZD_SCREEN_W - 1 - b.max_x;
                const int clearance_bottom = ZD_SCREEN_H - 1 - b.max_y;
                int worst = clearance_left;
                if (clearance_top < worst)
                    worst = clearance_top;
                if (clearance_right < worst)
                    worst = clearance_right;
                if (clearance_bottom < worst)
                    worst = clearance_bottom;

                check(worst >= ZD_RETENTION_STEP * 2, "%s/%s phase %d: ink %dpx from an edge, needs at least %d",
                      CASES[i].name, COVERS[j].name, phase, worst, ZD_RETENTION_STEP * 2);
                zd_scenes_retention_advance();
            }
        }
    }
}

// The framebuffer is wider than the panel. Scenes clear the whole thing and
// then draw only inside the visible area, so the hidden overscan column must
// come out pure background -- anything else is ink that escaped its clip, and it
// would be invisible when testing on hardware.
static void test_overscan_holds_only_background(void)
{
    puts("no ink escapes into the hidden overscan column");

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        zapdisplay_state_t state;
        populated_state(&state);
        attach_cover(&state, 394, 288);
        memset(framebuffer, 0xAB, sizeof(framebuffer));
        draw_case(&state, &CASES[i]);

        bool clean = true;
        for (int row = 0; row < ACTIVE_H && clean; row++) {
            for (int col = 0; col < OVERSCAN; col++) {
                if (!is_background(framebuffer[(size_t)row * ACTIVE_W + col])) {
                    clean = false;
                    break;
                }
            }
        }
        check(clean, "%s leaked ink into the overscan column", CASES[i].name);
    }
}

// A screen with nothing to say falls back to the brand mark rather than
// rendering an empty frame.
static void test_idle_fallbacks(void)
{
    puts("idle screens fall back when they have no content");

    static uint16_t brand[ACTIVE_W * ACTIVE_H];
    zapdisplay_state_t state;

    populated_state(&state);
    state.cover_rgb565 = NULL;
    state.clock_minutes = -1;
    state.idle_screen = ZAPDISPLAY_IDLE_BRAND;
    state.scene = ZAPDISPLAY_SCENE_IDLE;
    zd_canvas_t canvas = make_canvas();
    zd_scenes_draw(&canvas, &state);
    memcpy(brand, framebuffer, sizeof(brand));

    state.idle_screen = ZAPDISPLAY_IDLE_CLOCK;
    zd_scenes_draw(&canvas, &state);
    check(frames_identical(framebuffer, brand), "clock screen with no time set should render the brand screen");

    state.idle_screen = ZAPDISPLAY_IDLE_LAST_PLAYED;
    zd_scenes_draw(&canvas, &state);
    check(frames_identical(framebuffer, brand), "last-played with no cover should render the brand screen");

    // With a time set, the clock screen must differ from the brand screen.
    state.idle_screen = ZAPDISPLAY_IDLE_CLOCK;
    state.clock_minutes = 875;
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, brand), "clock screen with a time set should not render the brand screen");
}

// The widest artwork the device accepts must still leave a usable detail
// column, so the detail view never has to degrade. scenes.c asserts the same
// relationship at compile time; this checks it end to end, in drawn pixels.
static void test_widest_art_still_leaves_a_detail_column(void)
{
    puts("the widest accepted artwork still leaves a readable detail column");

    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();
    static uint16_t as_art[ACTIVE_W * ACTIVE_H];

    populated_state(&state);
    attach_cover(&state, ZD_COVER_MAX_WIDTH, 276);
    state.scene = ZAPDISPLAY_SCENE_PLAYING;

    state.view = ZAPDISPLAY_VIEW_ART;
    zd_scenes_draw(&canvas, &state);
    memcpy(as_art, framebuffer, sizeof(as_art));

    state.view = ZAPDISPLAY_VIEW_DETAIL;
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, as_art),
          "detail view degraded to the art view at maximum cover width; the detail column is too narrow");

    // The text column must start clear of the artwork and still reach the safe
    // area on the right.
    const int column_x = ZD_SAFE_X + ZD_COVER_MAX_WIDTH + ZD_PAD_GUTTER;
    check(ZD_CONTENT_RIGHT - column_x >= ZD_DETAIL_MIN_W, "detail column is %d px, minimum is %d",
          ZD_CONTENT_RIGHT - column_x, ZD_DETAIL_MIN_W);
}

// The playing views must actually differ; a view switch that changed nothing
// would be a silent regression the device could not report.
static void test_views_differ(void)
{
    puts("playing views render differently from one another");

    static uint16_t art[ACTIVE_W * ACTIVE_H];
    static uint16_t detail[ACTIVE_W * ACTIVE_H];
    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();

    populated_state(&state);
    attach_cover(&state, 192, 288);
    state.scene = ZAPDISPLAY_SCENE_PLAYING;

    state.view = ZAPDISPLAY_VIEW_ART;
    zd_scenes_draw(&canvas, &state);
    memcpy(art, framebuffer, sizeof(art));

    state.view = ZAPDISPLAY_VIEW_DETAIL;
    zd_scenes_draw(&canvas, &state);
    memcpy(detail, framebuffer, sizeof(detail));
    check(!frames_identical(art, detail), "art and detail views render identically");

    state.view = ZAPDISPLAY_VIEW_META;
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, detail), "detail and meta views render identically");
}

// Cover pixels must reach the panel unaltered apart from the corner rounding.
static void test_cover_is_blitted(void)
{
    puts("cover artwork reaches the framebuffer");

    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();

    populated_state(&state);
    attach_cover(&state, 192, 288);
    state.scene = ZAPDISPLAY_SCENE_PLAYING;
    state.view = ZAPDISPLAY_VIEW_DETAIL;
    zd_scenes_draw(&canvas, &state);

    int matched = 0;
    for (int y = ZD_SAFE_Y; y < ZD_SAFE_Y + 288; y++) {
        for (int x = ZD_SAFE_X; x < ZD_SAFE_X + 192; x++) {
            if (logical_pixel(x, y) == 0xF800) {
                matched++;
            }
        }
    }
    // Rounded corners and the retention offset lose a few pixels; the bulk of
    // the artwork must still be there.
    check(matched > 192 * 288 * 95 / 100, "only %d of %d cover pixels landed", matched, 192 * 288);
}

// Long text must be elided rather than overrunning the column it was given.
static void test_long_text_is_elided(void)
{
    puts("over-long text stays inside its column");

    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();

    populated_state(&state);
    attach_cover(&state, 192, 288);
    memset(state.title, 'W', sizeof(state.title) - 1);
    state.title[sizeof(state.title) - 1] = '\0';
    memset(state.developer, 'W', sizeof(state.developer) - 1);
    state.developer[sizeof(state.developer) - 1] = '\0';
    state.scene = ZAPDISPLAY_SCENE_PLAYING;

    for (int view = 0; view < ZAPDISPLAY_VIEW_COUNT; view++) {
        state.view = (zapdisplay_view_t)view;
        zd_scenes_draw(&canvas, &state);
        const ink_bounds_t b = ink_bounds();
        check(b.max_x < ZD_SCREEN_W - ZD_SAFE_X / 2, "view %d: text reached x=%d, past the safe area", view, b.max_x);
    }
}

// Every text field is a fixed-size buffer the protocol truncates into. Drawing
// must survive whatever ends up in one, including nothing at all.
static void test_empty_state_still_renders(void)
{
    puts("empty and default state still renders");

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        zapdisplay_state_t state;
        zapdisplay_state_set_defaults(&state);
        draw_case(&state, &CASES[i]);
        const ink_bounds_t b = ink_bounds();
        check(b.any, "%s with default state drew nothing", CASES[i].name);
    }
}

int main(void)
{
    test_edge_clearance();
    test_overscan_holds_only_background();
    test_idle_fallbacks();
    test_widest_art_still_leaves_a_detail_column();
    test_views_differ();
    test_cover_is_blitted();
    test_long_text_is_elided();
    test_empty_state_still_renders();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
