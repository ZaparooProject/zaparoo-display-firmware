// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "board/board.h"

#include <math.h>

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "board";

// The backlight LED string is driven through the board's AP3032 boost
// converter. 20 kHz keeps the switching out of the audible band and well clear
// of any beat with the panel's 16 MHz dot clock; 10 bits of duty is the most
// the 80 MHz source can resolve at that frequency with headroom to spare.
#define BACKLIGHT_TIMER LEDC_TIMER_0
#define BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_RESOLUTION LEDC_TIMER_10_BIT
#define BACKLIGHT_FREQ_HZ 20000
#define BACKLIGHT_MAX_DUTY ((1u << BACKLIGHT_RESOLUTION) - 1u)

// Boost converters stop regulating cleanly at very low duty and the string
// starts to flicker, so a non-zero brightness never asks for less than this.
// The floor is set conservatively at ~2%; measure the real knee on hardware
// before lowering it.
#define BACKLIGHT_MIN_DUTY 20u

// Perceived brightness is roughly the 2.2 power of luminance, so a linear duty
// ramp spends most of its range in a region the eye reads as "bright". Map the
// percentage through the inverse so the steps feel even.
#define BACKLIGHT_GAMMA 2.2f

static uint8_t s_backlight_percent;
static bool s_backlight_ready;

// clang-format off
static const zd_board_profile_t BOARD_PROFILE = {
    .backlight = GPIO_NUM_6,
    .backlight_active_high = true,
    .spi = {
        .sclk = GPIO_NUM_2,
        .mosi = GPIO_NUM_1,
        .cs = GPIO_NUM_42,
        .reset = GPIO_NUM_NC,
    },
    .rgb = {
        .pclk = GPIO_NUM_41,
        .de = GPIO_NUM_40,
        .hsync = GPIO_NUM_38,
        .vsync = GPIO_NUM_39,
        // ESP-IDF RGB565 order: B0..B4, G0..G5, R0..R4.
        // The 40-pin connector exposes RGB565 as B1..B5, G0..G5, R1..R5.
        .data = {
            GPIO_NUM_5,  // B1 -> RGB565 B0
            GPIO_NUM_45, // B2 -> RGB565 B1
            GPIO_NUM_48, // B3 -> RGB565 B2
            GPIO_NUM_47, // B4 -> RGB565 B3
            GPIO_NUM_21, // B5 -> RGB565 B4
            GPIO_NUM_14, // G0 -> RGB565 G0
            GPIO_NUM_13, // G1 -> RGB565 G1
            GPIO_NUM_12, // G2 -> RGB565 G2
            GPIO_NUM_11, // G3 -> RGB565 G3
            GPIO_NUM_10, // G4 -> RGB565 G4
            GPIO_NUM_9,  // G5 -> RGB565 G5
            GPIO_NUM_46, // R1 -> RGB565 R0
            GPIO_NUM_3,  // R2 -> RGB565 R1
            GPIO_NUM_8,  // R3 -> RGB565 R2
            GPIO_NUM_18, // R4 -> RGB565 R3
            GPIO_NUM_17, // R5 -> RGB565 R4
        },
    },
};
// clang-format on

const zd_board_profile_t *zd_board_get_profile(void)
{
    return &BOARD_PROFILE;
}

// percent_to_duty maps 0..100 to the raw duty the LEDC channel wants, applying
// the gamma curve, the floor, and the board's active level.
static uint32_t percent_to_duty(uint8_t percent)
{
    uint32_t duty;

    if (percent == 0) {
        duty = 0;
    } else {
        if (percent > ZD_BACKLIGHT_MAX_PERCENT) {
            percent = ZD_BACKLIGHT_MAX_PERCENT;
        }
        const float scaled = powf((float)percent / (float)ZD_BACKLIGHT_MAX_PERCENT, BACKLIGHT_GAMMA);
        const float span = (float)(BACKLIGHT_MAX_DUTY - BACKLIGHT_MIN_DUTY);
        duty = BACKLIGHT_MIN_DUTY + (uint32_t)(scaled * span + 0.5f);
        if (duty > BACKLIGHT_MAX_DUTY) {
            duty = BACKLIGHT_MAX_DUTY;
        }
    }

    return BOARD_PROFILE.backlight_active_high ? duty : BACKLIGHT_MAX_DUTY - duty;
}

esp_err_t zd_board_backlight_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = BACKLIGHT_MODE,
        .timer_num = BACKLIGHT_TIMER,
        .duty_resolution = BACKLIGHT_RESOLUTION,
        .freq_hz = BACKLIGHT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_config);
    if (ret != ESP_OK) {
        return ret;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = BOARD_PROFILE.backlight,
        .speed_mode = BACKLIGHT_MODE,
        .channel = BACKLIGHT_CHANNEL,
        .timer_sel = BACKLIGHT_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = percent_to_duty(0),
        .hpoint = 0,
    };
    ret = ledc_channel_config(&channel_config);
    if (ret != ESP_OK) {
        return ret;
    }

    // Fades run from the LEDC ISR, so brightness ramps do not block the
    // protocol loop. Installing twice is a hard error, so tolerate the case
    // where something else got there first.
    ret = ledc_fade_func_install(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    s_backlight_percent = 0;
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t zd_board_backlight_set_percent(uint8_t percent)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > ZD_BACKLIGHT_MAX_PERCENT) {
        percent = ZD_BACKLIGHT_MAX_PERCENT;
    }

    esp_err_t ret = ledc_set_duty(BACKLIGHT_MODE, BACKLIGHT_CHANNEL, percent_to_duty(percent));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ledc_update_duty(BACKLIGHT_MODE, BACKLIGHT_CHANNEL);
    if (ret != ESP_OK) {
        return ret;
    }

    s_backlight_percent = percent;
    return ESP_OK;
}

esp_err_t zd_board_backlight_fade_percent(uint8_t percent, uint32_t ms)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ms == 0) {
        return zd_board_backlight_set_percent(percent);
    }
    if (percent > ZD_BACKLIGHT_MAX_PERCENT) {
        percent = ZD_BACKLIGHT_MAX_PERCENT;
    }

    esp_err_t ret = ledc_set_fade_with_time(BACKLIGHT_MODE, BACKLIGHT_CHANNEL, percent_to_duty(percent), (int)ms);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ledc_fade_start(BACKLIGHT_MODE, BACKLIGHT_CHANNEL, LEDC_FADE_NO_WAIT);
    if (ret != ESP_OK) {
        return ret;
    }

    s_backlight_percent = percent;
    return ESP_OK;
}

uint8_t zd_board_backlight_get_percent(void)
{
    return s_backlight_percent;
}

void zd_board_log_profile(void)
{
    ESP_LOGI(TAG, "Board profile: %s", ZD_BOARD_NAME);
    ESP_LOGI(TAG, "Backlight: GPIO%d active-%s", BOARD_PROFILE.backlight,
             BOARD_PROFILE.backlight_active_high ? "high" : "low");
    ESP_LOGI(TAG, "Panel SPI: SCLK=GPIO%d MOSI=GPIO%d CS=GPIO%d RST=GPIO%d", BOARD_PROFILE.spi.sclk,
             BOARD_PROFILE.spi.mosi, BOARD_PROFILE.spi.cs, BOARD_PROFILE.spi.reset);
    ESP_LOGI(TAG, "Panel RGB: PCLK=GPIO%d DE=GPIO%d HSYNC=GPIO%d VSYNC=GPIO%d", BOARD_PROFILE.rgb.pclk,
             BOARD_PROFILE.rgb.de, BOARD_PROFILE.rgb.hsync, BOARD_PROFILE.rgb.vsync);
}
