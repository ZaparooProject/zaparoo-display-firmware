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

// Which way up the canvas under test is drawn. The canvas factory and the
// readback both consult it, so a test that sweeps orientations sets one thing.
static uint16_t rotation = 0;

// Every rotation the device offers. A layout that only clears its edges one way
// up is not safe, and the overscan is on one physical side only.
static const uint16_t ROTATIONS[] = {0, 180};

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
        .rotation = rotation,
    };
}

// Reads a logical pixel out of an arbitrary frame through the same mapping the
// canvas writes with. Deliberately spelled out rather than shared with
// canvas.c: a test that reused the code under test would pass no matter which
// way the mapping went.
static uint16_t logical_pixel_in(const uint16_t *frame, uint16_t at_rotation, int x, int y)
{
    if (at_rotation == 180) {
        return frame[(size_t)(ACTIVE_H - 1 - x) * ACTIVE_W + OVERSCAN + y];
    }
    return frame[(size_t)x * ACTIVE_W + OVERSCAN + (VISIBLE_W - 1 - y)];
}

static uint16_t logical_pixel(int x, int y)
{
    return logical_pixel_in(framebuffer, rotation, x, y);
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
    // The last-played slot is filled independently of the session fields,
    // because that is the whole point of it: it outlives a CLEAR.
    snprintf(state->last_title, sizeof(state->last_title), "Banjo-Kazooie");
    snprintf(state->last_system, sizeof(state->last_system), "Nintendo 64");
}

static void attach_cover(zapdisplay_state_t *state, int width, int height)
{
    fill_cover(0xF800, width, height);
    state->cover_rgb565 = cover;
    state->cover_width = (uint16_t)width;
    state->cover_height = (uint16_t)height;
    // Both point at the same committed slot on the device, so the fixture does
    // too.
    state->last_cover_rgb565 = cover;
    state->last_cover_width = (uint16_t)width;
    state->last_cover_height = (uint16_t)height;
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

// Bounding box of every pixel painted in one exact colour. Used to find the
// progress track without the test knowing where scenes.c puts it: the accent is
// drawn nowhere else in the launching scene, and no blend of the panel against
// a text colour can reach it, because none of them carry a full blue channel.
static ink_bounds_t bounds_of_color(uint16_t color)
{
    ink_bounds_t b = {ZD_SCREEN_W, ZD_SCREEN_H, -1, -1, false};

    for (int y = 0; y < ZD_SCREEN_H; y++) {
        for (int x = 0; x < ZD_SCREEN_W; x++) {
            if (logical_pixel(x, y) != color) {
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

#define IDLE_MARK (1u << ZAPDISPLAY_IDLE_BRAND)
#define IDLE_CLOCK (1u << ZAPDISPLAY_IDLE_CLOCK)
#define IDLE_LAST (1u << ZAPDISPLAY_IDLE_LAST_PLAYED)
#define IDLE_ALL (IDLE_MARK | IDLE_CLOCK | IDLE_LAST)

typedef struct {
    const char *name;
    zapdisplay_scene_t scene;
    zapdisplay_view_t view;
    uint8_t idle_elements;
} scene_case_t;

// Idle is one frame whose rows drop out as the device knows less, so the cases
// sweep the element mask rather than a list of screens.
static const scene_case_t CASES[] = {
    {"idle/all", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, IDLE_ALL},
    {"idle/mark-only", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, IDLE_MARK},
    {"idle/clock-only", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, IDLE_CLOCK},
    {"idle/last-only", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, IDLE_LAST},
    {"idle/no-elements", ZAPDISPLAY_SCENE_IDLE, ZAPDISPLAY_VIEW_DETAIL, 0},
    {"launching", ZAPDISPLAY_SCENE_LAUNCHING, ZAPDISPLAY_VIEW_DETAIL, IDLE_ALL},
    {"error", ZAPDISPLAY_SCENE_ERROR, ZAPDISPLAY_VIEW_DETAIL, IDLE_ALL},
    {"playing/art", ZAPDISPLAY_SCENE_PLAYING, ZAPDISPLAY_VIEW_ART, IDLE_ALL},
    {"playing/detail", ZAPDISPLAY_SCENE_PLAYING, ZAPDISPLAY_VIEW_DETAIL, IDLE_ALL},
    {"playing/meta", ZAPDISPLAY_SCENE_PLAYING, ZAPDISPLAY_VIEW_META, IDLE_ALL},
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
    state->idle_elements = c->idle_elements;
    zd_scenes_draw(&canvas, state);
}

// Nothing may sit within the safe margin of a canvas edge, at any retention
// offset. This is what stops content clipping when the anti-retention shift
// moves it.
static void test_edge_clearance(void)
{
    puts("edge clearance across every scene, view, rotation and retention offset");

    for (size_t r = 0; r < sizeof(ROTATIONS) / sizeof(ROTATIONS[0]); r++) {
        rotation = ROTATIONS[r];
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
                    check(b.any, "%s/%s rotation %u phase %d drew nothing at all", CASES[i].name, COVERS[j].name,
                          rotation, phase);
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

                    check(worst >= ZD_RETENTION_STEP * 2,
                          "%s/%s rotation %u phase %d: ink %dpx from an edge, needs at least %d", CASES[i].name,
                          COVERS[j].name, rotation, phase, worst, ZD_RETENTION_STEP * 2);
                    zd_scenes_retention_advance();
                }
            }
        }
    }
    rotation = 0;
}

// The framebuffer is wider than the panel. Scenes clear the whole thing and
// then draw only inside the visible area, so the hidden overscan column must
// come out pure background -- anything else is ink that escaped its clip, and it
// would be invisible when testing on hardware.
static void test_overscan_holds_only_background(void)
{
    puts("no ink escapes into the hidden overscan column, either way up");

    for (size_t r = 0; r < sizeof(ROTATIONS) / sizeof(ROTATIONS[0]); r++) {
        rotation = ROTATIONS[r];
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
            check(clean, "%s at rotation %u leaked ink into the overscan column", CASES[i].name, rotation);
        }
    }
    rotation = 0;
}

// Idle drops rows as the device knows less, and falls back to the resting mark
// when it knows nothing at all. What must not happen is a frame with holes in
// it, or a mask that quietly stops being honoured.
static void test_idle_degrades(void)
{
    puts("idle drops rows as the device knows less");

    static uint16_t resting[ACTIVE_W * ACTIVE_H];
    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();

    // Knows nothing: no host time, nothing played, no artwork.
    populated_state(&state);
    state.scene = ZAPDISPLAY_SCENE_IDLE;
    state.cover_rgb565 = NULL;
    state.last_cover_rgb565 = NULL;
    state.clock_minutes = -1;
    state.last_title[0] = '\0';
    state.idle_elements = IDLE_MARK;
    zd_scenes_draw(&canvas, &state);
    memcpy(resting, framebuffer, sizeof(resting));

    state.idle_elements = IDLE_ALL;
    zd_scenes_draw(&canvas, &state);
    check(frames_identical(framebuffer, resting),
          "with no clock and nothing played, every element enabled should still rest on the mark");

    state.idle_elements = 0;
    zd_scenes_draw(&canvas, &state);
    check(frames_identical(framebuffer, resting), "an empty mask should mean the mark alone");

    // A clock the device has been told about takes it out of the resting form.
    state.idle_elements = IDLE_ALL;
    state.clock_minutes = 875;
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, resting), "a clock that is set should leave the resting form");

    // ...but only if the mask allows it.
    static uint16_t with_clock[ACTIVE_W * ACTIVE_H];
    memcpy(with_clock, framebuffer, sizeof(with_clock));
    state.idle_elements = IDLE_ALL & ~IDLE_CLOCK;
    zd_scenes_draw(&canvas, &state);
    check(frames_identical(framebuffer, resting), "the clock bit off should put idle back to resting");
    check(!frames_identical(framebuffer, with_clock), "the clock bit should actually be honoured");

    // Something played is enough on its own, with no clock.
    state.clock_minutes = -1;
    state.idle_elements = IDLE_ALL;
    snprintf(state.last_title, sizeof(state.last_title), "Banjo-Kazooie");
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, resting), "a last-played title should leave the resting form");

    static uint16_t with_last[ACTIVE_W * ACTIVE_H];
    memcpy(with_last, framebuffer, sizeof(with_last));
    state.idle_elements = IDLE_ALL & ~IDLE_LAST;
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, with_last), "the last-played bit should actually be honoured");
}

// The regression this whole slot exists for: a host clears the session state
// before it goes idle, which wipes the title and drops the cover pointer. Idle
// has to keep showing what was last played anyway, or the device sits on
// artwork it can no longer name and the element never appears at all.
static void test_idle_survives_a_cleared_session(void)
{
    puts("idle still shows last played after the session state is cleared");

    static uint16_t before[ACTIVE_W * ACTIVE_H];
    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();

    populated_state(&state);
    attach_cover(&state, 192, 288);
    state.scene = ZAPDISPLAY_SCENE_IDLE;
    state.idle_elements = IDLE_ALL;
    zd_scenes_draw(&canvas, &state);
    memcpy(before, framebuffer, sizeof(before));

    // What CLEAR does to the fields idle draws from: title, system and the
    // session cover pointer go; the last-played slot and the committed cover
    // stay. Status is left alone because a host re-sends it either way, and a
    // different status line is a different frame for legitimate reasons.
    state.title[0] = '\0';
    state.system[0] = '\0';
    state.cover_rgb565 = NULL;
    state.cover_width = 0;
    state.cover_height = 0;
    zd_scenes_draw(&canvas, &state);

    check(frames_identical(framebuffer, before), "clearing the session fields should not change what idle draws");

    // And the slot really is what drives it.
    state.last_title[0] = '\0';
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, before), "idle should be reading the last-played slot, not the session title");
}

// The idle column is centred as a group, so dropping rows must not leave it
// hanging from the top of the panel. Measured with no block beside it, so the
// bounds are the column's own.
static void test_idle_column_stays_centred(void)
{
    puts("the idle column stays centred as rows drop out");

    // Masks chosen to leave the mark and the artwork out, so what is measured
    // is the text column alone: two rows, then four, then five.
    static const struct {
        const char *name;
        uint8_t elements;
        bool clock;
        bool played;
    } STEPS[] = {
        {"clock and status", IDLE_CLOCK, true, false},
        {"status and last played", IDLE_LAST, false, true},
        {"everything but the mark", IDLE_CLOCK | IDLE_LAST, true, true},
    };

    for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
        zapdisplay_state_t state;
        zd_canvas_t canvas = make_canvas();

        populated_state(&state);
        state.scene = ZAPDISPLAY_SCENE_IDLE;
        state.cover_rgb565 = NULL;
        state.last_cover_rgb565 = NULL;
        state.idle_elements = STEPS[i].elements;
        state.clock_minutes = STEPS[i].clock ? 875 : -1;
        if (!STEPS[i].played) {
            state.last_title[0] = '\0';
        }
        zd_scenes_draw(&canvas, &state);

        const ink_bounds_t b = ink_bounds();
        check(b.any, "%s drew nothing", STEPS[i].name);
        if (!b.any) {
            continue;
        }
        const int centre = (b.min_y + b.max_y) / 2;
        // Descenders on the bottom row pull the ink box down a little, so this
        // is a tolerance rather than an equality.
        check(centre >= ZD_SCREEN_H / 2 - 20 && centre <= ZD_SCREEN_H / 2 + 20,
              "%s: column centred at y=%d, panel centre is %d", STEPS[i].name, centre, ZD_SCREEN_H / 2);
    }
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

// Flipping the panel must change nothing about the image itself. Read back
// through its own mapping, a 180 degree render has to be pixel-identical to an
// upright one -- if it is not, some drawing path is still assuming one
// orientation.
static void test_rotation_180_is_an_exact_flip(void)
{
    puts("rotation 180 renders the identical logical image");

    static uint16_t upright[ACTIVE_W * ACTIVE_H];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        for (size_t j = 0; j < sizeof(COVERS) / sizeof(COVERS[0]); j++) {
            zapdisplay_state_t state;

            populated_state(&state);
            attach_cover(&state, COVERS[j].width, COVERS[j].height);
            state.has_toast = true;
            snprintf(state.toast, sizeof(state.toast), "Scanned  A Reasonably Long Game Title");

            rotation = 0;
            draw_case(&state, &CASES[i]);
            memcpy(upright, framebuffer, sizeof(upright));

            rotation = 180;
            draw_case(&state, &CASES[i]);

            int differences = 0;
            for (int y = 0; y < ZD_SCREEN_H; y++) {
                for (int x = 0; x < ZD_SCREEN_W; x++) {
                    if (logical_pixel_in(upright, 0, x, y) != logical_pixel_in(framebuffer, 180, x, y)) {
                        differences++;
                    }
                }
            }
            check(differences == 0, "%s/%s: %d logical pixels differ between upright and flipped", CASES[i].name,
                  COVERS[j].name, differences);
        }
    }
    rotation = 0;
}

// The launching track is indeterminate: one cell lit, stepping right, wrapping.
// What has to hold is that exactly one cell is lit, that it advances by one
// cell pitch per phase, and that the track occupies the same width whatever
// phase it is in -- a bar whose extent moved would read as progress.
static void test_progress_track_marches(void)
{
    puts("the launching track marches one cell per phase");

    static uint16_t at_zero[ACTIVE_W * ACTIVE_H];
    zapdisplay_state_t state;
    zd_canvas_t canvas = make_canvas();
    ink_bounds_t previous = {0, 0, 0, 0, false};
    int swept_min_x = ZD_SCREEN_W;
    int swept_max_x = -1;

    populated_state(&state);
    state.scene = ZAPDISPLAY_SCENE_LAUNCHING;

    for (int phase = 0; phase < ZD_TRACK_CELLS; phase++) {
        state.progress_phase = (uint8_t)phase;
        zd_scenes_draw(&canvas, &state);
        if (phase == 0) {
            memcpy(at_zero, framebuffer, sizeof(at_zero));
        }

        const ink_bounds_t cell = bounds_of_color(ZD_C_ACCENT);
        check(cell.any, "phase %d lit no cell at all", phase);
        if (!cell.any) {
            continue;
        }
        check(cell.max_x - cell.min_x + 1 == ZD_TRACK_CELL_W, "phase %d lit %d px of width, expected %d", phase,
              cell.max_x - cell.min_x + 1, ZD_TRACK_CELL_W);
        check(cell.max_y - cell.min_y + 1 == ZD_TRACK_CELL_H, "phase %d lit %d px of height, expected %d", phase,
              cell.max_y - cell.min_y + 1, ZD_TRACK_CELL_H);

        if (phase > 0) {
            check(cell.min_x - previous.min_x == ZD_TRACK_CELL_W + ZD_TRACK_GAP,
                  "phase %d moved the lit cell %d px, expected one pitch of %d", phase, cell.min_x - previous.min_x,
                  ZD_TRACK_CELL_W + ZD_TRACK_GAP);
            check(cell.min_y == previous.min_y, "phase %d moved the lit cell off the track's baseline", phase);
        }
        previous = cell;

        if (cell.min_x < swept_min_x)
            swept_min_x = cell.min_x;
        if (cell.max_x > swept_max_x)
            swept_max_x = cell.max_x;
    }

    // A full cycle must cover the whole track and no more, which is the same
    // property as the track's width never changing with its state.
    const int expected = ZD_TRACK_CELLS * ZD_TRACK_CELL_W + (ZD_TRACK_CELLS - 1) * ZD_TRACK_GAP;
    check(swept_max_x - swept_min_x + 1 == expected, "a full cycle covered %d px of track, expected %d",
          swept_max_x - swept_min_x + 1, expected);

    // The phase is a free-running counter the tick never resets, so the scene
    // has to fold it back onto the track itself.
    state.progress_phase = ZD_TRACK_CELLS;
    zd_scenes_draw(&canvas, &state);
    check(frames_identical(framebuffer, at_zero), "phase %d should render the same frame as phase 0", ZD_TRACK_CELLS);

    state.progress_phase = 1;
    zd_scenes_draw(&canvas, &state);
    check(!frames_identical(framebuffer, at_zero), "consecutive phases render identically; the track is not animating");
}

// The animation repaints one band rather than the whole frame, so that band has
// to be indistinguishable from a full draw. The trap is the cells' antialiased
// corners: they blend against what is already there, so a partial redraw that
// did not clear first would blend each new cell over the last one's corner and
// drift a little further every step. Redrawing repeatedly is what catches it.
static void test_progress_band_matches_a_full_draw(void)
{
    puts("repainting the track alone matches a full scene draw");

    static uint16_t full[ACTIVE_W * ACTIVE_H];

    for (size_t r = 0; r < sizeof(ROTATIONS) / sizeof(ROTATIONS[0]); r++) {
        rotation = ROTATIONS[r];
        for (int phase = 0; phase < ZD_TRACK_CELLS; phase++) {
            zapdisplay_state_t state;
            zd_canvas_t canvas = make_canvas();

            populated_state(&state);
            state.scene = ZAPDISPLAY_SCENE_LAUNCHING;
            state.progress_phase = (uint8_t)phase;
            zd_scenes_draw(&canvas, &state);
            memcpy(full, framebuffer, sizeof(full));

            // Walk the whole cycle into this frame the way the device does,
            // ending on the phase drawn above.
            state.progress_phase = 0;
            zd_scenes_draw(&canvas, &state);
            for (int step = 1; step <= ZD_TRACK_CELLS + phase; step++) {
                state.progress_phase = (uint8_t)step;
                zd_scenes_draw_progress(&canvas, &state);
            }

            check(frames_identical(framebuffer, full),
                  "rotation %u phase %d: band redraw differs from a full draw after a full cycle", rotation, phase);
        }
    }
    rotation = 0;
}

// The band the display layer pushes has to contain the band the scene drew,
// exactly. Too small and the march leaves fragments behind; too large and it
// pushes pixels nothing wrote.
static void test_progress_band_maps_onto_the_framebuffer(void)
{
    puts("the animated band maps onto the framebuffer exactly");

    for (size_t r = 0; r < sizeof(ROTATIONS) / sizeof(ROTATIONS[0]); r++) {
        rotation = ROTATIONS[r];
        for (int phase = 0; phase < RETENTION_POSITIONS; phase++) {
            zapdisplay_state_t state;
            zd_canvas_t canvas = make_canvas();
            zd_rect_t logical;
            zd_rect_t band;

            populated_state(&state);
            state.scene = ZAPDISPLAY_SCENE_LAUNCHING;

            check(zd_scenes_progress_rect(&state, &logical), "launching should report an animated band");
            check(zd_canvas_physical_rect(&canvas, logical, &band), "the band should map onto the framebuffer");

            check((long)band.width * band.height == (long)logical.width * logical.height,
                  "rotation %u offset %d: band is %dx%d px for a %dx%d logical rect", rotation, phase, band.width,
                  band.height, logical.width, logical.height);
            check(band.x >= OVERSCAN && band.x + band.width <= ACTIVE_W,
                  "rotation %u offset %d: band spans columns %d..%d, outside the visible area", rotation, phase, band.x,
                  band.x + band.width - 1);
            check(band.y >= 0 && band.y + band.height <= ACTIVE_H,
                  "rotation %u offset %d: band spans rows %d..%d, off the panel", rotation, phase, band.y,
                  band.y + band.height - 1);

            // Every pixel the scene draws into the band must land inside it.
            memset(framebuffer, 0xAB, sizeof(framebuffer));
            zd_scenes_draw_progress(&canvas, &state);
            bool contained = true;
            for (int row = 0; row < ACTIVE_H && contained; row++) {
                for (int col = 0; col < ACTIVE_W; col++) {
                    if (framebuffer[(size_t)row * ACTIVE_W + col] == 0xABAB) {
                        continue;
                    }
                    if (row < band.y || row >= band.y + band.height || col < band.x || col >= band.x + band.width) {
                        contained = false;
                        break;
                    }
                }
            }
            check(contained, "rotation %u offset %d: the track drew outside the band the display pushes", rotation,
                  phase);
            zd_scenes_retention_advance();
        }
    }
    rotation = 0;
}

// A scene with nothing animating must not claim a band; the display layer would
// push it instead of the frame the scene actually needs.
static void test_only_launching_reports_a_band(void)
{
    puts("only the launching scene reports an animated band");

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        zapdisplay_state_t state;
        zd_rect_t logical;

        populated_state(&state);
        state.scene = CASES[i].scene;
        state.idle_elements = CASES[i].idle_elements;
        state.view = CASES[i].view;

        const bool reported = zd_scenes_progress_rect(&state, &logical);
        check(reported == (CASES[i].scene == ZAPDISPLAY_SCENE_LAUNCHING), "%s %s an animated band", CASES[i].name,
              reported ? "claimed" : "did not claim");
    }
}

int main(void)
{
    test_edge_clearance();
    test_overscan_holds_only_background();
    test_rotation_180_is_an_exact_flip();
    test_progress_track_marches();
    test_progress_band_matches_a_full_draw();
    test_progress_band_maps_onto_the_framebuffer();
    test_only_launching_reports_a_band();
    test_idle_degrades();
    test_idle_survives_a_cleared_session();
    test_idle_column_stays_centred();
    test_widest_art_still_leaves_a_detail_column();
    test_views_differ();
    test_cover_is_blitted();
    test_long_text_is_elided();
    test_empty_state_still_renders();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
