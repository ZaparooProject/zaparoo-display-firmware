// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "display/hd458002c40.h"

#include "display/display_profile.h"

#include <string.h>

#include "board/board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "settings/settings.h"
#include "ui/scenes.h"
#include "ui/theme.h"

static const char *TAG = "display";

// ST7701S register sequence for this panel, adapted from Adafruit's published
// Qualia / HD458002C40 reference material. See NOTICE.md.
//
// Encoding is flat: command byte, length byte, then that many parameter bytes.
// Bit 0x80 in the length byte means one delay-in-milliseconds byte follows the
// parameters.
// clang-format off
static const uint8_t HD458002C40_INIT_SEQUENCE[] = {
    0xFF, 0x05, 0x77, 0x01, 0x00, 0x00, 0x13,
    0xEF, 0x01, 0x08,
    0xFF, 0x05, 0x77, 0x01, 0x00, 0x00, 0x10,
    0xC0, 0x02, 0x77, 0x00,
    0xC1, 0x02, 0x09, 0x08,
    0xC2, 0x02, 0x01, 0x02,
    0xC3, 0x01, 0x02,
    0xCC, 0x01, 0x10,
    0xB0, 0x10, 0x40, 0x14, 0x59, 0x10, 0x12, 0x08, 0x03, 0x09, 0x05, 0x1E, 0x05, 0x14, 0x10, 0x68, 0x33, 0x15,
    0xB1, 0x10, 0x40, 0x08, 0x53, 0x09, 0x11, 0x09, 0x02, 0x07, 0x09, 0x1A, 0x04, 0x12, 0x12, 0x64, 0x29, 0x29,
    0xFF, 0x05, 0x77, 0x01, 0x00, 0x00, 0x11,
    0xB0, 0x01, 0x6D,
    0xB1, 0x01, 0x1D,
    0xB2, 0x01, 0x87,
    0xB3, 0x01, 0x80,
    0xB5, 0x01, 0x49,
    0xB7, 0x01, 0x85,
    0xB8, 0x01, 0x20,
    0xC1, 0x01, 0x78,
    0xC2, 0x01, 0x78,
    0xD0, 0x01, 0x88,
    0xE0, 0x03, 0x00, 0x00, 0x02,
    0xE1, 0x0B, 0x02, 0x8C, 0x00, 0x00, 0x03, 0x8C, 0x00, 0x00, 0x00, 0x33, 0x33,
    0xE2, 0x0D, 0x33, 0x33, 0x33, 0x33, 0xC9, 0x3C, 0x00, 0x00, 0xCA, 0x3C, 0x00, 0x00, 0x00,
    0xE3, 0x04, 0x00, 0x00, 0x33, 0x33,
    0xE4, 0x02, 0x44, 0x44,
    0xE5, 0x10, 0x05, 0xCD, 0x82, 0x82, 0x01, 0xC9, 0x82, 0x82, 0x07, 0xCF, 0x82, 0x82, 0x03, 0xCB, 0x82, 0x82,
    0xE6, 0x04, 0x00, 0x00, 0x33, 0x33,
    0xE7, 0x02, 0x44, 0x44,
    0xE8, 0x10, 0x06, 0xCE, 0x82, 0x82, 0x02, 0xCA, 0x82, 0x82, 0x08, 0xD0, 0x82, 0x82, 0x04, 0xCC, 0x82, 0x82,
    0xEB, 0x07, 0x08, 0x01, 0xE4, 0xE4, 0x88, 0x00, 0x40,
    0xEC, 0x03, 0x00, 0x00, 0x00,
    0xED, 0x10, 0xFF, 0xF0, 0x07, 0x65, 0x4F, 0xFC, 0xC2, 0x2F, 0xF2, 0x2C, 0xCF, 0xF4, 0x56, 0x70, 0x0F, 0xFF,
    0xEF, 0x06, 0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F,
    0xFF, 0x05, 0x77, 0x01, 0x00, 0x00, 0x00,
    0x11, 0x80, 0x78,
    0x35, 0x01, 0x00,
    0x3A, 0x81, 0x66, 0x64,
    0x29, 0x00,
};
// clang-format on

// clang-format off
static const zd_display_profile_t DISPLAY_PROFILE = {
    .name = ZD_DISPLAY_PROFILE_NAME,
    .controller = "ST7701S",
    .timing = {
        .active_width = 400,
        .active_height = 960,
        .visible_width = 320,
        .visible_height = 960,
        .overscan_left = 80,
        .pixel_clock_hz = 16000000,
        .hsync_pulse_width = 10,
        .hsync_front_porch = 50,
        .hsync_back_porch = 30,
        .vsync_pulse_width = 2,
        // The Adafruit Qualia reference lists these the other way round (front
        // 17, back 14). Swapped is what produces a stable image on this panel;
        // re-measure against a second unit before changing them back.
        .vsync_front_porch = 14,
        .vsync_back_porch = 17,
        .hsync_idle_low = false,
        .vsync_idle_low = false,
        .de_idle_high = false,
        .pclk_active_high = false,
        .pclk_idle_high = false,
    },
};
// clang-format on

const zd_display_profile_t *zd_display_get_profile(void)
{
    return &DISPLAY_PROFILE;
}

void zd_display_log_profile(void)
{
    const zd_display_timing_t *timing = &DISPLAY_PROFILE.timing;

    ESP_LOGI(TAG, "Display profile: %s controller=%s", DISPLAY_PROFILE.name, DISPLAY_PROFILE.controller);
    ESP_LOGI(TAG, "Active area: %ux%u visible=%ux%u overscan_left=%u", timing->active_width, timing->active_height,
             timing->visible_width, timing->visible_height, timing->overscan_left);
    ESP_LOGI(TAG, "RGB timing: pclk=%luHz hsync=%u/%u/%u vsync=%u/%u/%u", (unsigned long)timing->pixel_clock_hz,
             timing->hsync_pulse_width, timing->hsync_front_porch, timing->hsync_back_porch, timing->vsync_pulse_width,
             timing->vsync_front_porch, timing->vsync_back_porch);
}

static void spi_delay(void)
{
    esp_rom_delay_us(1);
}

static esp_err_t spi_write_bit(const zd_board_spi_pins_t *spi, int bit)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(spi->sclk, 0), TAG, "set sclk low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(spi->mosi, bit ? 1 : 0), TAG, "set mosi failed");
    spi_delay();
    ESP_RETURN_ON_ERROR(gpio_set_level(spi->sclk, 1), TAG, "set sclk high failed");
    spi_delay();
    return ESP_OK;
}

static esp_err_t spi_write_9bit(const zd_board_spi_pins_t *spi, bool is_data, uint8_t value)
{
    ESP_RETURN_ON_ERROR(spi_write_bit(spi, is_data ? 1 : 0), TAG, "write dc bit failed");
    for (int bit = 7; bit >= 0; --bit) {
        ESP_RETURN_ON_ERROR(spi_write_bit(spi, (value >> bit) & 1), TAG, "write value bit failed");
    }
    return ESP_OK;
}

static esp_err_t init_soft_3wire_spi(const zd_board_spi_pins_t *spi)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << spi->sclk) | (1ULL << spi->mosi) | (1ULL << spi->cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure soft SPI pins failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(spi->cs, 1), TAG, "set cs idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(spi->sclk, 0), TAG, "set sclk idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(spi->mosi, 0), TAG, "set mosi idle failed");
    return ESP_OK;
}

static esp_err_t send_init_sequence(const zd_board_spi_pins_t *spi)
{
    size_t offset = 0;
    size_t commands = 0;

    ESP_LOGI(TAG, "Send ST7701S init sequence over soft 3-wire SPI");

    while (offset < sizeof(HD458002C40_INIT_SEQUENCE)) {
        const uint8_t command = HD458002C40_INIT_SEQUENCE[offset++];
        uint8_t length = HD458002C40_INIT_SEQUENCE[offset++];
        const bool delay_after = (length & 0x80) != 0;
        length &= 0x7F;

        ESP_RETURN_ON_ERROR(gpio_set_level(spi->cs, 0), TAG, "select panel failed");
        ESP_RETURN_ON_ERROR(spi_write_9bit(spi, false, command), TAG, "write command failed");
        for (uint8_t i = 0; i < length; ++i) {
            ESP_RETURN_ON_ERROR(spi_write_9bit(spi, true, HD458002C40_INIT_SEQUENCE[offset++]), TAG,
                                "write param failed");
        }
        ESP_RETURN_ON_ERROR(gpio_set_level(spi->cs, 1), TAG, "deselect panel failed");
        ++commands;

        if (delay_after) {
            const uint8_t delay_ms = HD458002C40_INIT_SEQUENCE[offset++];
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    ESP_LOGI(TAG, "ST7701S init sequence sent (%u commands)", (unsigned)commands);
    return ESP_OK;
}

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_framebuffer;
static zd_canvas_t s_canvas;

// esp_lcd_panel_draw_bitmap wants its source packed to the window it is given,
// so a band out of the middle of the framebuffer has to be gathered first. Held
// in internal RAM: it is the one buffer in this path that is read rather than
// written, and putting it in PSRAM would add a third crossing of a bus that is
// already the bottleneck.
//
// Sized from the track's own geometry. The rotation transposes the band and the
// retention offset moves it, but neither changes its area.
#define TRACK_BAND_PX ((ZD_TRACK_CELLS * ZD_TRACK_CELL_W + (ZD_TRACK_CELLS - 1) * ZD_TRACK_GAP) * ZD_TRACK_CELL_H)
static uint16_t s_band[TRACK_BAND_PX];

esp_err_t zd_hd458002c40_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    const zd_board_profile_t *board = zd_board_get_profile();
    const zd_display_timing_t *timing = &DISPLAY_PROFILE.timing;
    const size_t framebuffer_size = (size_t)timing->active_width * timing->active_height * sizeof(uint16_t);

    ESP_RETURN_ON_ERROR(zd_board_backlight_set_percent(0), TAG, "turn backlight off failed");
    ESP_RETURN_ON_ERROR(init_soft_3wire_spi(&board->spi), TAG, "init soft SPI failed");
    ESP_RETURN_ON_ERROR(send_init_sequence(&board->spi), TAG, "panel init sequence failed");

    ESP_LOGI(TAG, "Create RGB panel");
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = timing->active_width * 20,
        .dma_burst_size = 64,
        .disp_gpio_num = GPIO_NUM_NC,
        .pclk_gpio_num = board->rgb.pclk,
        .vsync_gpio_num = board->rgb.vsync,
        .hsync_gpio_num = board->rgb.hsync,
        .de_gpio_num = board->rgb.de,
        .timings =
            {
                .pclk_hz = timing->pixel_clock_hz,
                .h_res = timing->active_width,
                .v_res = timing->active_height,
                .hsync_pulse_width = timing->hsync_pulse_width,
                .hsync_back_porch = timing->hsync_back_porch,
                .hsync_front_porch = timing->hsync_front_porch,
                .vsync_pulse_width = timing->vsync_pulse_width,
                .vsync_back_porch = timing->vsync_back_porch,
                .vsync_front_porch = timing->vsync_front_porch,
                .flags =
                    {
                        .hsync_idle_low = timing->hsync_idle_low,
                        .vsync_idle_low = timing->vsync_idle_low,
                        .de_idle_high = timing->de_idle_high,
                        .pclk_active_neg = !timing->pclk_active_high,
                        .pclk_idle_high = timing->pclk_idle_high,
                    },
            },
        .flags.fb_in_psram = true,
    };
    memcpy(panel_config.data_gpio_nums, board->rgb.data, sizeof(board->rgb.data));

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &s_panel), TAG, "create RGB panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset RGB panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init RGB panel failed");

    s_framebuffer = heap_caps_malloc(framebuffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_framebuffer != NULL, ESP_ERR_NO_MEM, TAG, "allocate %u-byte framebuffer failed",
                        (unsigned)framebuffer_size);
    ESP_LOGI(TAG, "Allocated %u-byte framebuffer in PSRAM", (unsigned)framebuffer_size);

    s_canvas = (zd_canvas_t){
        .pixels = s_framebuffer,
        .active_width = timing->active_width,
        .active_height = timing->active_height,
        .visible_width = timing->visible_width,
        .visible_height = timing->visible_height,
        .logical_width = timing->visible_height,
        .logical_height = timing->visible_width,
        .overscan_left = timing->overscan_left,
        // Settings are loaded before the panel comes up, so the first frame is
        // already the right way up rather than flipping once after boot.
        .rotation = (uint16_t)zd_settings_get(ZD_SETTING_ROTATION),
    };

    // The backlight deliberately stays off here. Bringing it up at this point
    // would light whatever the freshly allocated framebuffer happens to hold;
    // the caller turns it on after the first render, at the brightness the
    // device is configured for. See runtime/tick.c.
    ESP_LOGI(TAG, "Display renderer ready");
    return ESP_OK;
}

void zd_hd458002c40_set_rotation(uint32_t degrees)
{
    if (degrees != 0 && degrees != 180) {
        return;
    }
    s_canvas.rotation = (uint16_t)degrees;
}

esp_err_t zd_hd458002c40_render_progress(const zapdisplay_state_t *state)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display renderer not initialized");

    zd_rect_t logical;
    zd_rect_t band;

    // A full frame is always correct, only slow, so anything unexpected about
    // the band falls back to one rather than pushing a window that might be
    // wrong.
    if (!zd_scenes_progress_rect(state, &logical) || !zd_canvas_physical_rect(&s_canvas, logical, &band) ||
        (size_t)band.width * (size_t)band.height > TRACK_BAND_PX) {
        return zd_hd458002c40_render(state);
    }

    zd_scenes_draw_progress(&s_canvas, state);

    for (int row = 0; row < band.height; ++row) {
        memcpy(&s_band[(size_t)row * band.width],
               &s_framebuffer[(size_t)(band.y + row) * s_canvas.active_width + band.x],
               (size_t)band.width * sizeof(uint16_t));
    }

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(s_panel, band.x, band.y, band.x + band.width, band.y + band.height, s_band), TAG,
        "draw progress band failed");
    return ESP_OK;
}

esp_err_t zd_hd458002c40_render(const zapdisplay_state_t *state)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL && s_framebuffer != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "display renderer not initialized");

    const int64_t started_us = esp_timer_get_time();

    zd_scenes_draw(&s_canvas, state);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_canvas.active_width, s_canvas.active_height, s_framebuffer), TAG,
        "draw scene failed");
    // The launching track repaints four times a second, so what a frame costs
    // decides whether the march is affordable. It varies with the scene, which
    // is why this is a log line rather than a number measured once and written
    // into a comment.
    ESP_LOGD(TAG, "Render scene %s: %lld us", zapdisplay_scene_name(state->scene), esp_timer_get_time() - started_us);
    return ESP_OK;
}
