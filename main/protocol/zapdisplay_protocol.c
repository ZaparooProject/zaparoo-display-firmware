// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "protocol/zapdisplay_protocol.h"

#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "display/display_profile.h"
#include "display/hd458002c40.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "input/button.h"
#include "input/input.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "runtime/session.h"
#include "runtime/tick.h"
#include "ui/scenes.h"
#include "settings/settings.h"
#include "ui/state.h"

static const char *TAG = "zapdisplay.protocol";

typedef struct {
    bool active;
    bool complete;
    char id[48];
    char mime[32];
    uint8_t *data;
    uint32_t expected_bytes;
    uint32_t received_bytes;
    uint32_t expected_crc;
    uint32_t crc;
    uint32_t next_chunk;
} asset_upload_t;

// cover_slot_t holds the cover the renderer is allowed to read. It is separate
// from the upload staging buffer so an in-flight upload can never free pixels
// a later SHOW would dereference.
typedef struct {
    uint8_t *data;
    char id[48];
    uint16_t width;
    uint16_t height;
} cover_slot_t;

enum {
    // Give up on a host that announced payload bytes and then stopped sending.
    // Without this the protocol loop blocks until the device is power cycled.
    STREAM_READ_TIMEOUT_US = 5 * 1000 * 1000,
};

static zapdisplay_state_t s_pending_state;
// The last state actually pushed to the panel. SHOW compares against this so a
// host refresh that redraws the identical frame does not count as activity and
// does not hold the panel at full brightness. Copied with memcpy rather than
// struct assignment so padding is carried verbatim and memcmp stays meaningful.
static zapdisplay_state_t s_shown_state;
static bool s_has_shown;
static asset_upload_t s_asset;
static cover_slot_t s_cover;

// What was last actually played, kept beside the cover slot for the same reason
// the slot exists: it outlives the pending state a host resets with CLEAR.
// Idle draws from this, so the caption stays with the picture instead of the
// device holding artwork it can no longer name.
static char s_last_title[64];
static char s_last_system[48];
static bool s_quiet_mode;
// Device-to-host event lines are off until a host asks for them. Every existing
// host parses responses by prefix and treats any unexpected line as noise, so
// an unsolicited EVENT would at best be discarded and at worst be mistaken for
// a response. Opting in is what makes this addition safe to deploy.
static bool s_events_enabled;

// copy_text_field truncates to fit the destination, then drops a UTF-8 sequence
// the truncation split. Fields are byte buffers but hosts think in characters,
// so a long accented title lands mid-sequence; without this the renderer would
// draw the stray bytes as a replacement glyph on the end of the text.
static void copy_text_field(char *dst, size_t dst_size, const char *src)
{
    strlcpy(dst, src, dst_size);

    const size_t length = strlen(dst);
    for (size_t back = 0; back < 4 && back < length; ++back) {
        const uint8_t byte = (uint8_t)dst[length - 1 - back];
        size_t expected;

        if ((byte & 0xC0) == 0x80) {
            continue; // continuation byte; keep walking back to the lead
        }
        if ((byte & 0x80) == 0) {
            expected = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            expected = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            expected = 3;
        } else {
            expected = 4;
        }
        if (back + 1 < expected) {
            dst[length - 1 - back] = '\0';
        }
        return;
    }
}

static bool parse_hex_nibble(char c, uint8_t *value)
{
    if (c >= '0' && c <= '9') {
        *value = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *value = (uint8_t)(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *value = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

static bool parse_accent_rgb565(const char *text, uint16_t *color)
{
    if (text[0] == '#') {
        ++text;
    }
    if (strlen(text) != 6) {
        return false;
    }

    uint8_t nibbles[6];
    for (size_t i = 0; i < 6; ++i) {
        if (!parse_hex_nibble(text[i], &nibbles[i])) {
            return false;
        }
    }

    const uint8_t r = (uint8_t)((nibbles[0] << 4) | nibbles[1]);
    const uint8_t g = (uint8_t)((nibbles[2] << 4) | nibbles[3]);
    const uint8_t b = (uint8_t)((nibbles[4] << 4) | nibbles[5]);
    *color = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return true;
}

static bool parse_scene(const char *text, zapdisplay_scene_t *scene)
{
    if (strcmp(text, "idle") == 0) {
        *scene = ZAPDISPLAY_SCENE_IDLE;
    } else if (strcmp(text, "launching") == 0) {
        *scene = ZAPDISPLAY_SCENE_LAUNCHING;
    } else if (strcmp(text, "playing") == 0) {
        *scene = ZAPDISPLAY_SCENE_PLAYING;
    } else if (strcmp(text, "error") == 0) {
        *scene = ZAPDISPLAY_SCENE_ERROR;
    } else {
        return false;
    }
    return true;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    uint32_t parsed = 0;

    if (*text == '\0') {
        return false;
    }

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(*cursor - '0');
        if (parsed > (UINT32_MAX - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }

    *value = parsed;
    return true;
}

static bool parse_hex_u32(const char *text, uint32_t *value)
{
    uint32_t parsed = 0;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (*text == '\0') {
        return false;
    }

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        uint8_t nibble;
        if (!parse_hex_nibble(*cursor, &nibble)) {
            return false;
        }
        parsed = (parsed << 4) | nibble;
    }

    *value = parsed;
    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool base64_value(char c, uint8_t *value)
{
    if (c >= 'A' && c <= 'Z') {
        *value = (uint8_t)(c - 'A');
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        *value = (uint8_t)(c - 'a' + 26);
        return true;
    }
    if (c >= '0' && c <= '9') {
        *value = (uint8_t)(c - '0' + 52);
        return true;
    }
    if (c == '+') {
        *value = 62;
        return true;
    }
    if (c == '/') {
        *value = 63;
        return true;
    }
    return false;
}

static bool base64_decode_len(const char *text, size_t length, uint8_t *out, size_t out_capacity, size_t *out_length)
{
    size_t written = 0;

    if (length == 0 || (length % 4) != 0) {
        return false;
    }

    for (size_t offset = 0; offset < length; offset += 4) {
        uint8_t values[4] = {0};
        int padding = 0;

        for (int i = 0; i < 4; ++i) {
            const char c = text[offset + i];
            if (c == '=') {
                values[i] = 0;
                ++padding;
            } else if (padding > 0 || !base64_value(c, &values[i])) {
                return false;
            }
        }
        if (padding > 2) {
            return false;
        }

        const uint8_t decoded[3] = {
            (uint8_t)((values[0] << 2) | (values[1] >> 4)),
            (uint8_t)((values[1] << 4) | (values[2] >> 2)),
            (uint8_t)((values[2] << 6) | values[3]),
        };
        const size_t bytes = 3u - (size_t)padding;
        if (written + bytes > out_capacity) {
            return false;
        }
        memcpy(out + written, decoded, bytes);
        written += bytes;
    }

    *out_length = written;
    return true;
}

// read_exact fills buffer with length bytes of raw payload. The deadline is
// refreshed on every successful read, so a slow link is fine but a silent one
// is not.
static bool read_exact(void *buffer, size_t length)
{
    uint8_t *cursor = buffer;
    size_t remaining = length;
    int64_t deadline_us = esp_timer_get_time() + STREAM_READ_TIMEOUT_US;

    while (remaining > 0) {
        const size_t got = fread(cursor, 1, remaining, stdin);
        if (got == 0) {
            if (esp_timer_get_time() > deadline_us) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        cursor += got;
        remaining -= got;
        deadline_us = esp_timer_get_time() + STREAM_READ_TIMEOUT_US;
    }
    return true;
}

static void asset_reset(void)
{
    if (s_asset.data != NULL) {
        heap_caps_free(s_asset.data);
    }
    memset(&s_asset, 0, sizeof(s_asset));
}

static void strip_line_endings(char *line)
{
    line[strcspn(line, "\r\n")] = '\0';
}

// firmware_version reports the version baked in at build time from
// PROJECT_VER, so the wire response can never drift from the running image.
static const char *firmware_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown";
}

static void send_ready(void)
{
    const zd_display_profile_t *profile = zd_display_get_profile();

    printf("READY %s fw=%s display=%s_%ux%u\n", ZAPDISPLAY_PROTOCOL, firmware_version(), profile->name,
           profile->timing.visible_width, profile->timing.visible_height);
    fflush(stdout);
}

static void send_info(void)
{
    const zd_display_profile_t *profile = zd_display_get_profile();

    printf("INFO %s fw=%s display=%s visible=%ux%u transport=cdc "
           "features=scenes,assets-b64,assets-raw,quiet,settings,views,meta,toast,clock,input\n",
           ZAPDISPLAY_PROTOCOL, firmware_version(), profile->name, profile->timing.visible_width,
           profile->timing.visible_height);
    fflush(stdout);
}

static void ok(void)
{
    puts("OK");
    fflush(stdout);
}

static void err(const char *reason)
{
    printf("ERR %s\n", reason);
    fflush(stdout);
}

static void handle_benchmark(uint32_t expected_bytes)
{
    enum { BENCH_MAX_BYTES = 262144 };
    uint8_t buffer[256];
    uint32_t remaining = expected_bytes;
    uint32_t checksum = 0;

    if (expected_bytes == 0 || expected_bytes > BENCH_MAX_BYTES) {
        err("bad-bench-size");
        return;
    }

    puts("OK BENCH_BEGIN");
    fflush(stdout);

    const int64_t start_us = esp_timer_get_time();
    int64_t deadline_us = start_us + STREAM_READ_TIMEOUT_US;
    while (remaining > 0) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const size_t got = fread(buffer, 1, wanted, stdin);
        if (got == 0) {
            if (esp_timer_get_time() > deadline_us) {
                err("timeout");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        for (size_t i = 0; i < got; ++i) {
            checksum = (checksum + buffer[i]) & 0xFFFFFFFFu;
        }
        remaining -= (uint32_t)got;
        deadline_us = esp_timer_get_time() + STREAM_READ_TIMEOUT_US;
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const uint32_t elapsed_ms = (uint32_t)((elapsed_us + 999) / 1000);
    const uint32_t rate_bps = elapsed_us > 0 ? (uint32_t)(((uint64_t)expected_bytes * 1000000ULL) / elapsed_us) : 0;

    printf("BENCH bytes=%lu ms=%lu rate=%luBps checksum=%08lx\n", (unsigned long)expected_bytes,
           (unsigned long)elapsed_ms, (unsigned long)rate_bps, (unsigned long)checksum);
    fflush(stdout);
}

static void handle_asset_begin(const char *args)
{
    enum { ASSET_MAX_BYTES = ZD_COVER_MAX_BYTES };
    char id[sizeof(s_asset.id)] = {0};
    char mime[sizeof(s_asset.mime)] = {0};
    char bytes_text[16] = {0};
    char crc_text[16] = {0};
    uint32_t expected_bytes;
    uint32_t expected_crc;

    if (sscanf(args, "%47s %31s %15s %15s", id, mime, bytes_text, crc_text) != 4 ||
        !parse_u32(bytes_text, &expected_bytes) || !parse_hex_u32(crc_text, &expected_crc)) {
        err("bad-asset-begin");
        return;
    }
    if (expected_bytes == 0 || expected_bytes > ASSET_MAX_BYTES) {
        err("overflow");
        return;
    }

    asset_reset();
    s_asset.data = heap_caps_malloc(expected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_asset.data == NULL) {
        err("no-memory");
        return;
    }

    s_asset.active = true;
    s_asset.complete = false;
    strlcpy(s_asset.id, id, sizeof(s_asset.id));
    strlcpy(s_asset.mime, mime, sizeof(s_asset.mime));
    s_asset.expected_bytes = expected_bytes;
    s_asset.expected_crc = expected_crc;
    s_asset.crc = 0;
    s_asset.received_bytes = 0;
    s_asset.next_chunk = 0;

    puts("OK ASSET_BEGIN");
    fflush(stdout);
}

// render_launch_band repaints the launching scene's progress track in place.
//
// It works from the frame that is on the panel, not from the state the host is
// assembling. Those differ for as long as a launch takes: a host puts the
// launching frame up, then CLEARs the pending state and spends seconds
// uploading artwork before it shows anything again. Core does exactly that, so
// keying this off the pending scene froze the bar for the length of every
// upload -- the one stretch it exists to cover.
static void render_launch_band(void)
{
    if (!s_has_shown || s_shown_state.scene != ZAPDISPLAY_SCENE_LAUNCHING) {
        return;
    }

    // s_shown_state is a content snapshot, so it carries everything the
    // launching scene draws -- system, title, status, accent -- but not the
    // phase, which is derived rather than shown.
    zapdisplay_state_t frame = s_shown_state;

    frame.progress_phase = zd_tick_progress_phase();
    zd_hd458002c40_render_progress(&frame);
}

// step_launch_animation keeps the track moving while an upload is in flight.
// The loop sits inside framed payload windows for the length of a transfer and
// the tick never runs there, so a bar that only stepped from service_tick would
// freeze for exactly the seconds someone is watching it.
//
// Called only once a chunk has been answered, while the host is preparing the
// next one. It deliberately does not go through service_tick: that polls input
// and flushes events, and an event line emitted inside a payload window would
// be read by the host as data.
//
// This has to stay cheap or it turns into a tax on every upload. Repainting the
// whole frame here measured at 540 ms a step, which took a 227 KB cover from
// 4.8 s to 8.6 s; the band is under 5000 pixels.
static void step_launch_animation(void)
{
    if (zd_tick_progress_step()) {
        render_launch_band();
    }
}

static bool append_asset_chunk(uint32_t index, const char *encoded, size_t encoded_length)
{
    static uint8_t decoded[768];
    size_t decoded_length = 0;

    if (!s_asset.active) {
        err("bad-state");
        return false;
    }
    if (index != s_asset.next_chunk) {
        err("bad-chunk-index");
        return false;
    }
    if (!base64_decode_len(encoded, encoded_length, decoded, sizeof(decoded), &decoded_length)) {
        err("bad-base64");
        return false;
    }
    if (s_asset.received_bytes + decoded_length > s_asset.expected_bytes) {
        err("overflow");
        return false;
    }

    memcpy(s_asset.data + s_asset.received_bytes, decoded, decoded_length);
    s_asset.received_bytes += (uint32_t)decoded_length;
    s_asset.crc = crc32_update(s_asset.crc, decoded, decoded_length);
    ++s_asset.next_chunk;
    return true;
}

static void handle_asset_chunk(const char *args)
{
    char index_text[16] = {0};
    uint32_t index;
    const char *payload = strchr(args, ' ');

    if (payload == NULL || (size_t)(payload - args) >= sizeof(index_text)) {
        err("bad-asset-chunk");
        return;
    }

    memcpy(index_text, args, (size_t)(payload - args));
    index_text[payload - args] = '\0';
    ++payload;

    if (!parse_u32(index_text, &index)) {
        err("bad-asset-chunk");
        return;
    }
    if (append_asset_chunk(index, payload, strlen(payload))) {
        printf("OK ASSET_CHUNK %lu\n", (unsigned long)index);
        fflush(stdout);
        step_launch_animation();
    }
}

static void handle_asset_chunk_data(const char *args)
{
    enum { ENCODED_MAX_BYTES = 1024 };
    char index_text[16] = {0};
    char length_text[16] = {0};
    uint32_t index;
    uint32_t encoded_length;
    // Static rather than automatic: the protocol loop runs on the main task and
    // a kilobyte of stack here is a meaningful share of its budget.
    static char encoded[ENCODED_MAX_BYTES];

    if (sscanf(args, "%15s %15s", index_text, length_text) != 2 || !parse_u32(index_text, &index) ||
        !parse_u32(length_text, &encoded_length) || encoded_length == 0 || encoded_length > sizeof(encoded) ||
        (encoded_length % 4) != 0) {
        err("bad-asset-chunk");
        return;
    }

    puts("OK ASSET_CHUNK_DATA");
    fflush(stdout);

    if (!read_exact(encoded, encoded_length)) {
        err("timeout");
        asset_reset();
        return;
    }

    if (append_asset_chunk(index, encoded, encoded_length)) {
        printf("OK ASSET_CHUNK %lu\n", (unsigned long)index);
        fflush(stdout);
        step_launch_animation();
    }
}

// handle_asset_chunk_raw takes payload bytes with no encoding. It reads
// straight into the asset buffer, so a chunk can be as large as the host cares
// to send and costs no intermediate storage. Uploads spend most of their time
// on per-chunk round trips, so larger chunks matter more than raw throughput.
static void handle_asset_chunk_raw(const char *args)
{
    char index_text[16] = {0};
    char length_text[16] = {0};
    uint32_t index;
    uint32_t length;

    if (sscanf(args, "%15s %15s", index_text, length_text) != 2 || !parse_u32(index_text, &index) ||
        !parse_u32(length_text, &length) || length == 0) {
        err("bad-asset-chunk");
        return;
    }
    if (!s_asset.active) {
        err("bad-state");
        return;
    }
    if (index != s_asset.next_chunk) {
        err("bad-chunk-index");
        return;
    }
    if (length > s_asset.expected_bytes - s_asset.received_bytes) {
        err("overflow");
        return;
    }

    puts("OK ASSET_CHUNK_RAW");
    fflush(stdout);

    uint8_t *destination = s_asset.data + s_asset.received_bytes;
    if (!read_exact(destination, length)) {
        err("timeout");
        asset_reset();
        return;
    }

    s_asset.received_bytes += length;
    s_asset.crc = crc32_update(s_asset.crc, destination, length);
    ++s_asset.next_chunk;
    printf("OK ASSET_CHUNK %lu\n", (unsigned long)index);
    fflush(stdout);
    step_launch_animation();
}

static void handle_asset_end(const char *id)
{
    if (!s_asset.active) {
        err("bad-state");
        return;
    }
    if (strcmp(id, s_asset.id) != 0) {
        err("bad-asset-id");
        return;
    }
    if (s_asset.received_bytes != s_asset.expected_bytes) {
        err("bad-asset-size");
        return;
    }
    if (s_asset.crc != s_asset.expected_crc) {
        err("crc");
        asset_reset();
        return;
    }

    s_asset.active = false;
    s_asset.complete = true;
    printf("OK ASSET %s bytes=%lu crc=%08lx\n", s_asset.id, (unsigned long)s_asset.received_bytes,
           (unsigned long)s_asset.crc);
    fflush(stdout);
}

// select_cover points the pending render state at the committed cover.
static void select_cover(void)
{
    s_pending_state.cover_rgb565 = (const uint16_t *)s_cover.data;
    s_pending_state.cover_width = s_cover.width;
    s_pending_state.cover_height = s_cover.height;
}

// commit_cover moves ownership of a validated upload into the cover slot. The
// previous cover is released only once the new one is in place and the render
// state no longer refers to it.
static void commit_cover(uint16_t width, uint16_t height)
{
    uint8_t *previous = s_cover.data;

    s_pending_state.cover_rgb565 = NULL;
    s_pending_state.cover_width = 0;
    s_pending_state.cover_height = 0;

    s_cover.data = s_asset.data;
    s_cover.width = width;
    s_cover.height = height;
    strlcpy(s_cover.id, s_asset.id, sizeof(s_cover.id));

    // Ownership has moved, so asset_reset() must not free this buffer.
    s_asset.data = NULL;
    s_asset.active = false;
    s_asset.complete = false;

    if (previous != NULL) {
        heap_caps_free(previous);
    }
}

static void handle_asset_use(const char *args)
{
    char role[16] = {0};
    char id[sizeof(s_asset.id)] = {0};
    char width_text[8] = {0};
    char height_text[8] = {0};
    char format[16] = {0};
    uint32_t width;
    uint32_t height;

    if (sscanf(args, "%15s %47s %7s %7s %15s", role, id, width_text, height_text, format) != 5 ||
        !parse_u32(width_text, &width) || !parse_u32(height_text, &height)) {
        err("bad-asset-use");
        return;
    }
    if (strcmp(role, "cover") != 0 || strcmp(format, "rgb565") != 0) {
        err("bad-asset-use");
        return;
    }
    // Any aspect within bounds is allowed: box art is portrait on some systems
    // and landscape on others, and the scene lays out around whatever arrives.
    if (width == 0 || height == 0 || width > ZD_COVER_MAX_WIDTH || height > ZD_COVER_MAX_HEIGHT) {
        err("bad-asset-size");
        return;
    }

    // Already committed. Re-selecting costs nothing, so a host that cleared the
    // scene or reconnected does not have to upload the same cover again.
    if (s_cover.data != NULL && strcmp(id, s_cover.id) == 0) {
        select_cover();
        ok();
        return;
    }

    if (!s_asset.complete || strcmp(id, s_asset.id) != 0) {
        err("asset-missing");
        return;
    }
    if (strcmp(s_asset.mime, "image/rgb565") != 0) {
        err("bad-asset-mime");
        return;
    }
    if (s_asset.received_bytes != width * height * sizeof(uint16_t)) {
        err("bad-asset-size");
        return;
    }

    commit_cover((uint16_t)width, (uint16_t)height);
    select_cover();
    ok();
}

// refresh_derived_fields fills in everything the device works out for itself
// rather than being told: the session clock, the wall clock, the live toast and
// which parts idle is allowed to draw. Scenes stay pure functions of the state
// struct so the host render harness can exercise them unchanged, which is why
// the idle_screens setting is resolved here rather than read from the scene.
static void refresh_derived_fields(void)
{
    const char *toast = zd_session_toast();

    s_pending_state.has_elapsed = zd_session_elapsed(&s_pending_state.elapsed_seconds);
    s_pending_state.clock_minutes = zd_session_clock_minutes();
    s_pending_state.idle_elements = (uint8_t)zd_settings_get(ZD_SETTING_IDLE_SCREENS);
    strlcpy(s_pending_state.last_title, s_last_title, sizeof(s_pending_state.last_title));
    strlcpy(s_pending_state.last_system, s_last_system, sizeof(s_pending_state.last_system));
    // Points at the committed slot, never at the upload buffer, so an upload in
    // flight cannot free pixels this render is about to read.
    s_pending_state.last_cover_rgb565 = (const uint16_t *)s_cover.data;
    s_pending_state.last_cover_width = s_cover.width;
    s_pending_state.last_cover_height = s_cover.height;
    s_pending_state.progress_phase = zd_tick_progress_phase();
    s_pending_state.has_toast = toast != NULL;
    if (toast != NULL) {
        strlcpy(s_pending_state.toast, toast, sizeof(s_pending_state.toast));
    } else {
        s_pending_state.toast[0] = '\0';
    }
}

// content_snapshot strips the fields that move on their own. A clock ticking, a
// session counter rolling over or the progress track marching are not somebody
// using the device, and counting them as activity would hold the panel at full
// brightness for as long as it stayed powered. The track is the sharpest case:
// it moves four times a second.
//
// idle_elements is deliberately kept. It only changes when someone changes the
// setting, and that really is a different frame.
static void content_snapshot(zapdisplay_state_t *out, const zapdisplay_state_t *in)
{
    memcpy(out, in, sizeof(*out));
    out->elapsed_seconds = 0;
    out->has_elapsed = false;
    out->clock_minutes = -1;
    out->progress_phase = 0;
    out->has_toast = false;
    memset(out->toast, 0, sizeof(out->toast));
}

// render_state pushes state to the panel and keeps the idle policy honest: a
// frame whose content differs from the last one is activity, an identical
// refresh is not.
static esp_err_t render_state(void)
{
    zapdisplay_state_t snapshot;

    refresh_derived_fields();

    const esp_err_t ret = zd_hd458002c40_render(&s_pending_state);
    if (ret != ESP_OK) {
        return ret;
    }

    content_snapshot(&snapshot, &s_pending_state);
    if (!s_has_shown || memcmp(&s_shown_state, &snapshot, sizeof(s_shown_state)) != 0) {
        zd_tick_notify_activity();
    }
    memcpy(&s_shown_state, &snapshot, sizeof(s_shown_state));
    s_has_shown = true;
    return ESP_OK;
}

// Events are emitted from the loop body only, never from inside handle_line.
// That keeps them out of the framed windows where the device is reading raw
// payload bytes, where an interleaved line would be read as data.
#define EVENT_QUEUE_LEN 8
static zd_input_event_t s_event_queue[EVENT_QUEUE_LEN];
static uint8_t s_event_count;

static void queue_event(const zd_input_event_t *event)
{
    if (!s_events_enabled) {
        return;
    }
    if (s_event_count >= EVENT_QUEUE_LEN) {
        // A host that stopped reading is not a reason to grow a buffer without
        // bound. Dropping the newest keeps the oldest, which is the one the
        // host is still waiting to act on.
        return;
    }
    s_event_queue[s_event_count++] = *event;
}

static void flush_events(void)
{
    char text[64];

    for (uint8_t i = 0; i < s_event_count; i++) {
        zd_input_format(&s_event_queue[i], text, sizeof(text));
        printf("EVENT %s\n", text);
    }
    if (s_event_count > 0) {
        s_event_count = 0;
        fflush(stdout);
    }
}

// send_state reports what the device is doing rather than what it was told.
// It exists because the panel's own behaviour -- dimming, blanking, stepping
// the retention offset -- is otherwise invisible to a host and untestable
// without watching the screen.
static void send_state(void)
{
    uint32_t elapsed = 0;
    const bool has_elapsed = zd_session_elapsed(&elapsed);

    printf("STATE scene=%s view=%s panel=%s brightness=%u idle=%lu silent=%lu retention=%u idle_elements=%u "
           "elapsed=%ld clock=%d toast=%d\n",
           zapdisplay_scene_name(s_pending_state.scene), zapdisplay_view_name(s_pending_state.view),
           zd_tick_panel_level_name(zd_tick_panel_level()), zd_board_backlight_get_percent(),
           (unsigned long)zd_tick_idle_seconds(), (unsigned long)zd_tick_silent_seconds(), zd_scenes_retention_phase(),
           (unsigned)zd_settings_get(ZD_SETTING_IDLE_SCREENS), has_elapsed ? (long)elapsed : -1L,
           zd_session_clock_minutes(), zd_session_toast() != NULL ? 1 : 0);
    fflush(stdout);
}

// remember_last_played captures the caption for the cover the device is already
// holding. Done on the way to showing a playing frame rather than at ASSET_USE:
// a host uploads the artwork before it sends the title, and a launch that ends
// in an error should not be remembered as something that played.
static void remember_last_played(void)
{
    if (s_pending_state.scene != ZAPDISPLAY_SCENE_PLAYING || s_pending_state.title[0] == '\0') {
        return;
    }
    strlcpy(s_last_title, s_pending_state.title, sizeof(s_last_title));
    strlcpy(s_last_system, s_pending_state.system, sizeof(s_last_system));
}

static void handle_show(void)
{
    remember_last_played();

    const esp_err_t ret = render_state();

    if (ret == ESP_OK) {
        ok();
    } else {
        if (!s_quiet_mode) {
            ESP_LOGE(TAG, "render failed: %s", esp_err_to_name(ret));
        }
        err("render-failed");
    }
}

// CLEAR drops what the host told us, not what the device was configured to do.
// The view is a device preference; a host clearing the screen has no business
// resetting it.
static void reset_pending_state(void)
{
    zapdisplay_state_set_defaults(&s_pending_state);
    s_pending_state.view = (zapdisplay_view_t)zd_settings_get(ZD_SETTING_VIEW);
    zd_session_clear_elapsed();
    zd_session_clear_toast();
}

// Brightness steps a person can find in the dark. Cycling rather than stepping
// so one key covers the whole range.
static const uint8_t BRIGHTNESS_STEPS[] = {100, 60, 30, 10};

// apply_binding carries out a display-local action. Anything the device cannot
// act on itself returns false so the caller forwards it to the host instead.
static bool apply_binding(zd_input_binding_t binding, bool *redraw)
{
    switch (binding) {
    case ZD_BIND_NONE:
        return true;
    case ZD_BIND_WAKE:
        zd_tick_wake();
        return true;
    case ZD_BIND_VIEW_NEXT:
        s_pending_state.view = (zapdisplay_view_t)((s_pending_state.view + 1) % ZAPDISPLAY_VIEW_COUNT);
        *redraw = true;
        return true;
    case ZD_BIND_VIEW_PREV:
        s_pending_state.view =
            (zapdisplay_view_t)((s_pending_state.view + ZAPDISPLAY_VIEW_COUNT - 1) % ZAPDISPLAY_VIEW_COUNT);
        *redraw = true;
        return true;
    case ZD_BIND_BRIGHTNESS_CYCLE: {
        const uint32_t current = zd_settings_get(ZD_SETTING_BRIGHTNESS);
        size_t next = 0;
        for (size_t i = 0; i < sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]); i++) {
            if (BRIGHTNESS_STEPS[i] == current) {
                next = (i + 1) % (sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]));
                break;
            }
        }
        zd_settings_set(ZD_SETTING_BRIGHTNESS, BRIGHTNESS_STEPS[next]);
        zd_tick_apply_brightness();
        return true;
    }
    case ZD_BIND_SCREEN_TOGGLE:
        zd_tick_toggle_blank();
        return true;
    case ZD_BIND_FORWARD:
    default:
        return false;
    }
}

// dispatch_input is the single path every event takes, whoever raised it.
static void dispatch_input(const zd_input_event_t *event)
{
    bool redraw = false;

    if (!apply_binding(zd_input_dispatch(event), &redraw)) {
        queue_event(event);
    }
    if (redraw) {
        render_state();
    }
}

static void handle_input_command(const char *args)
{
    zd_input_event_t event;

    if (!zd_input_parse(args, &event)) {
        err("bad-input");
        return;
    }
    // Answer before acting on it. A view change repaints the whole framebuffer,
    // and a host waiting on OK should not be held up by a redraw it did not ask
    // for.
    ok();
    dispatch_input(&event);
}

static void handle_bind(const char *args)
{
    zd_input_event_t event;
    zd_input_binding_t binding;
    char shape[48];
    const char *last_space = strrchr(args, ' ');

    if (last_space == NULL || (size_t)(last_space - args) >= sizeof(shape)) {
        err("bad-bind");
        return;
    }
    memcpy(shape, args, (size_t)(last_space - args));
    shape[last_space - args] = '\0';

    if (!zd_input_parse(shape, &event) || !zd_input_binding_lookup(last_space + 1, &binding)) {
        err("bad-bind");
        return;
    }
    if (!zd_input_bind(&event, binding)) {
        err("bad-bind");
        return;
    }
    ok();
}

static bool parse_view(const char *text, zapdisplay_view_t *view)
{
    for (int i = 0; i < ZAPDISPLAY_VIEW_COUNT; i++) {
        if (strcmp(text, zapdisplay_view_name((zapdisplay_view_t)i)) == 0) {
            *view = (zapdisplay_view_t)i;
            return true;
        }
    }
    return false;
}

// TOAST <seconds> <text>. A scan is unambiguously a person doing something, so
// it counts as activity and wakes a dimmed panel even though the banner itself
// is excluded from the frame comparison.
static void handle_toast(const char *args)
{
    const char *space = strchr(args, ' ');
    uint32_t seconds;
    char digits[12];

    if (space == NULL || (size_t)(space - args) >= sizeof(digits)) {
        err("bad-toast");
        return;
    }
    memcpy(digits, args, (size_t)(space - args));
    digits[space - args] = '\0';
    if (!parse_u32(digits, &seconds) || seconds == 0) {
        err("bad-toast");
        return;
    }

    zd_session_set_toast(space + 1, seconds);
    zd_tick_notify_activity();
    ok();
}

// TIME <epoch_seconds> <utc_offset_seconds>. Signed, because both a pre-epoch
// clock and a negative UTC offset are legitimate.
static void handle_time(const char *args)
{
    long long epoch;
    long offset;

    if (sscanf(args, "%lld %ld", &epoch, &offset) != 2) {
        err("bad-time");
        return;
    }
    if (offset < -50400 || offset > 50400) {
        err("bad-time");
        return;
    }

    zd_session_set_clock((int64_t)epoch, (int32_t)offset);
    ok();
}

static void send_setting(zd_setting_t setting)
{
    printf("SETTING %s %lu\n", zd_settings_name(setting), (unsigned long)zd_settings_get(setting));
}

static void send_all_settings(void)
{
    for (int i = 0; i < ZD_SETTING_COUNT; i++) {
        send_setting((zd_setting_t)i);
    }
    ok();
}

// apply_setting_side_effects re-applies anything a setting controls outside its
// own stored value. Brightness and the dim level change the panel now rather
// than at the next timeout.
//
// Returns true when the frame itself has to be redrawn. The caller does that
// after answering rather than here: a repaint pushes the whole framebuffer, and
// a host waiting on OK would notice the delay.
static bool apply_setting_side_effects(zd_setting_t setting)
{
    if (setting == ZD_SETTING_BRIGHTNESS || setting == ZD_SETTING_DIM_LEVEL) {
        zd_tick_apply_brightness();
        return false;
    }
    if (setting == ZD_SETTING_ROTATION) {
        zd_hd458002c40_set_rotation(zd_settings_get(setting));
        return true;
    }
    return false;
}

static void handle_setting_set(const char *args)
{
    char name[32];
    const char *space = strchr(args, ' ');
    uint32_t value;
    zd_setting_t setting;

    if (space == NULL || (size_t)(space - args) >= sizeof(name)) {
        err("bad-setting");
        return;
    }
    memcpy(name, args, (size_t)(space - args));
    name[space - args] = '\0';

    if (!zd_settings_lookup(name, &setting)) {
        err("bad-setting");
        return;
    }
    if (!parse_u32(space + 1, &value)) {
        err("bad-value");
        return;
    }
    // A value that cannot be persisted still takes effect for this session, so
    // report the range failure and let a storage failure be a log line rather
    // than pretending the setting was rejected.
    if (zd_settings_set(setting, value) == ESP_ERR_INVALID_ARG) {
        err("bad-value");
        return;
    }

    const bool needs_redraw = apply_setting_side_effects(setting);

    ok();
    if (needs_redraw) {
        render_state();
    }
}

static void handle_setting_get(const char *args)
{
    zd_setting_t setting;

    if (!zd_settings_lookup(args, &setting)) {
        err("bad-setting");
        return;
    }
    send_setting(setting);
}

static void handle_line(const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if (!s_quiet_mode) {
        ESP_LOGI(TAG, "RX: %s", line);
    }

    // Any command at all is evidence the host is alive, including keepalives
    // that change nothing on screen. Whether the *frame* changed is decided
    // separately, in render_state.
    zd_tick_notify_host_contact();

    if (strcmp(line, "HELLO " ZAPDISPLAY_PROTOCOL) == 0) {
        puts("OK " ZAPDISPLAY_PROTOCOL);
    } else if (strcmp(line, "PING") == 0) {
        puts("PONG");
    } else if (strcmp(line, "READY?") == 0) {
        send_ready();
    } else if (strcmp(line, "INFO") == 0) {
        send_info();
    } else if (strcmp(line, "QUIET") == 0) {
        s_quiet_mode = true;
        ok();
    } else if (strcmp(line, "VERBOSE") == 0) {
        s_quiet_mode = false;
        ok();
    } else if (strcmp(line, "CLEAR") == 0) {
        reset_pending_state();
        ok();
    } else if (strcmp(line, "SHOW") == 0) {
        handle_show();
    } else if (strcmp(line, "STATE") == 0) {
        send_state();
    } else if (strcmp(line, "SETTINGS") == 0) {
        send_all_settings();
    } else if (strncmp(line, "SET ", 4) == 0) {
        handle_setting_set(line + 4);
    } else if (strncmp(line, "GET ", 4) == 0) {
        handle_setting_get(line + 4);
    } else if (strncmp(line, "SCENE ", 6) == 0) {
        zapdisplay_scene_t scene;
        if (parse_scene(line + 6, &scene)) {
            s_pending_state.scene = scene;
            ok();
        } else {
            err("bad-scene");
        }
    } else if (strncmp(line, "TITLE ", 6) == 0) {
        copy_text_field(s_pending_state.title, sizeof(s_pending_state.title), line + 6);
        ok();
    } else if (strncmp(line, "SYSTEM ", 7) == 0) {
        copy_text_field(s_pending_state.system, sizeof(s_pending_state.system), line + 7);
        ok();
    } else if (strncmp(line, "STATUS ", 7) == 0) {
        copy_text_field(s_pending_state.status, sizeof(s_pending_state.status), line + 7);
        ok();
    } else if (strncmp(line, "MESSAGE ", 8) == 0) {
        copy_text_field(s_pending_state.message, sizeof(s_pending_state.message), line + 8);
        ok();
    } else if (strncmp(line, "YEAR ", 5) == 0) {
        copy_text_field(s_pending_state.year, sizeof(s_pending_state.year), line + 5);
        ok();
    } else if (strncmp(line, "DEVELOPER ", 10) == 0) {
        copy_text_field(s_pending_state.developer, sizeof(s_pending_state.developer), line + 10);
        ok();
    } else if (strncmp(line, "GENRE ", 6) == 0) {
        copy_text_field(s_pending_state.genre, sizeof(s_pending_state.genre), line + 6);
        ok();
    } else if (strncmp(line, "PLAYERS ", 8) == 0) {
        copy_text_field(s_pending_state.players, sizeof(s_pending_state.players), line + 8);
        ok();
    } else if (strncmp(line, "VIEW ", 5) == 0) {
        zapdisplay_view_t view;
        if (parse_view(line + 5, &view)) {
            s_pending_state.view = view;
            ok();
        } else {
            err("bad-view");
        }
    } else if (strncmp(line, "ELAPSED ", 8) == 0) {
        uint32_t seconds;
        if (parse_u32(line + 8, &seconds)) {
            zd_session_set_elapsed(seconds);
            ok();
        } else {
            err("bad-elapsed");
        }
    } else if (strcmp(line, "ELAPSED") == 0) {
        zd_session_clear_elapsed();
        ok();
    } else if (strncmp(line, "TIME ", 5) == 0) {
        handle_time(line + 5);
    } else if (strncmp(line, "TOAST ", 6) == 0) {
        handle_toast(line + 6);
    } else if (strncmp(line, "INPUT ", 6) == 0) {
        handle_input_command(line + 6);
    } else if (strncmp(line, "BIND ", 5) == 0) {
        handle_bind(line + 5);
    } else if (strcmp(line, "EVENTS on") == 0) {
        s_events_enabled = true;
        ok();
    } else if (strcmp(line, "EVENTS off") == 0) {
        s_events_enabled = false;
        s_event_count = 0;
        ok();
    } else if (strncmp(line, "ACCENT ", 7) == 0) {
        uint16_t color;
        if (parse_accent_rgb565(line + 7, &color)) {
            s_pending_state.accent_color = color;
            s_pending_state.has_accent = true;
            ok();
        } else {
            err("bad-color");
        }
    } else if (strncmp(line, "BENCH_BEGIN ", 12) == 0) {
        uint32_t expected_bytes;
        if (parse_u32(line + 12, &expected_bytes)) {
            handle_benchmark(expected_bytes);
        } else {
            err("bad-bench-size");
        }
    } else if (strncmp(line, "ASSET_BEGIN ", 12) == 0) {
        handle_asset_begin(line + 12);
    } else if (strncmp(line, "ASSET_CHUNK_RAW ", 16) == 0) {
        handle_asset_chunk_raw(line + 16);
    } else if (strncmp(line, "ASSET_CHUNK_DATA ", 17) == 0) {
        handle_asset_chunk_data(line + 17);
    } else if (strncmp(line, "ASSET_CHUNK ", 12) == 0) {
        handle_asset_chunk(line + 12);
    } else if (strncmp(line, "ASSET_END ", 10) == 0) {
        handle_asset_end(line + 10);
    } else if (strncmp(line, "ASSET_USE ", 10) == 0) {
        handle_asset_use(line + 10);
    } else {
        err("unknown-command");
    }
    fflush(stdout);
}

// discard_rest_of_line consumes input up to and including the next newline, so
// the tail of an over-long command is not parsed as further commands.
static void discard_rest_of_line(void)
{
    char scratch[128];
    const int64_t deadline_us = esp_timer_get_time() + STREAM_READ_TIMEOUT_US;

    while (esp_timer_get_time() < deadline_us) {
        if (fgets(scratch, sizeof(scratch), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (strchr(scratch, '\n') != NULL) {
            return;
        }
    }
}

typedef enum {
    LINE_NONE,
    LINE_READY,
    LINE_TOO_LONG,
    LINE_TIMEOUT,
} line_status_t;

// read_line assembles exactly one command line.
//
// The console returns short reads at its RX buffer boundary, so one fgets is not
// one line: a command longer than that boundary arrives in pieces. Acting on a
// piece would truncate the command and answer the remainder with
// ERR unknown-command, leaving a spare response for the host to trip over. Any
// field long enough to push a command past the boundary reaches this, which a
// 58-character TITLE is enough to do.
//
// Returns LINE_NONE when nothing is waiting at all, so the caller's READY
// heartbeat keeps running, but once a partial line has arrived it waits for the
// rest rather than acting on a fragment.
static line_status_t read_line(char *line, size_t size)
{
    size_t used = 0;
    int64_t deadline_us = 0;

    while (true) {
        if (fgets(line + used, (int)(size - used), stdin) != NULL) {
            used += strlen(line + used);
            if (used > 0 && line[used - 1] == '\n') {
                return LINE_READY;
            }
            if (used >= size - 1) {
                return LINE_TOO_LONG;
            }
            deadline_us = esp_timer_get_time() + STREAM_READ_TIMEOUT_US;
            continue;
        }
        if (used == 0) {
            return LINE_NONE;
        }
        if (esp_timer_get_time() > deadline_us) {
            return LINE_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// service_tick runs the periodic panel-care work and acts on what it asks for.
// The tick reports rather than reaches: it has no business touching the pending
// state or the quiet flag, which this module owns.
static void service_tick(void)
{
    zd_input_event_t button;

    while (zd_button_poll(&button)) {
        dispatch_input(&button);
    }

    const zd_tick_result_t result = zd_tick();

    if (result.host_absent) {
        // The host has been silent long enough to presume it is gone. Leaving a
        // session frame up is the one case that can actually mark this panel,
        // and a host that vanished after QUIET would otherwise keep the device
        // silent for whoever connects next.
        ESP_LOGI(TAG, "Host silent; returning to idle");
        reset_pending_state();
        s_quiet_mode = false;
        s_events_enabled = false;
        s_event_count = 0;
        render_state();
        return;
    }

    // Only the scene on screen decides whether a clock or an idle rotation is
    // worth a full framebuffer push.
    const zapdisplay_scene_t scene = s_pending_state.scene;
    bool redraw = result.redraw || result.toast_changed;

    if (result.time_changed && (scene == ZAPDISPLAY_SCENE_PLAYING || scene == ZAPDISPLAY_SCENE_IDLE)) {
        redraw = true;
    }
    // A full frame subsumes the band, so the cheap path only runs when nothing
    // else already needs the whole thing. render_launch_band decides for itself
    // whether the panel is showing something with a band on it.
    if (redraw) {
        render_state();
    } else if (result.progress_stepped) {
        render_launch_band();
    }

    flush_events();
}

void zapdisplay_protocol_run(void)
{
    // Static rather than automatic, for the same stack reason as the chunk
    // buffers: this all runs on the main task.
    static char line[520];

    // The console VFS translates received CR into LF by default, which silently
    // rewrites any 0x0D inside a binary payload -- the reason raw transfers
    // failed their checksum before and everything had to go through base64.
    // Passing bytes through unmodified makes ASSET_CHUNK_RAW possible; the line
    // protocol is unaffected because strip_line_endings already tolerates CR.
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    zd_input_init();
    zd_button_init();
    reset_pending_state();
    ESP_LOGI(TAG, "Protocol loop started");

    while (true) {
        if (!s_quiet_mode) {
            send_ready();
        }

        const TickType_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
            const line_status_t status = read_line(line, sizeof(line));

            if (status == LINE_NONE) {
                service_tick();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (status == LINE_TOO_LONG) {
                discard_rest_of_line();
                err("line-too-long");
                continue;
            }
            if (status == LINE_TIMEOUT) {
                err("timeout");
                continue;
            }
            strip_line_endings(line);
            handle_line(line);
        }
    }
}
