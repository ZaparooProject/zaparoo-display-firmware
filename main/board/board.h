// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define ZD_BOARD_NAME "waveshare_esp32_s3_lcd_driver_board"

// The panel is driven over 3-wire SPI, so the data/command bit is carried in
// the 9-bit word rather than on a DC line, and there is no read path.
typedef struct {
    gpio_num_t sclk;
    gpio_num_t mosi;
    gpio_num_t cs;
    // The panel's reset is behind the board's I2C IO expander rather than on a
    // GPIO, so it is left unconnected here and the controller relies on
    // power-on reset. See docs/hardware.md.
    gpio_num_t reset;
} zd_board_spi_pins_t;

typedef struct {
    gpio_num_t pclk;
    gpio_num_t de;
    gpio_num_t hsync;
    gpio_num_t vsync;
    gpio_num_t data[16];
} zd_board_rgb565_pins_t;

typedef struct {
    gpio_num_t backlight;
    bool backlight_active_high;
    zd_board_spi_pins_t spi;
    zd_board_rgb565_pins_t rgb;
} zd_board_profile_t;

const zd_board_profile_t *zd_board_get_profile(void);
void zd_board_log_profile(void);

// The backlight is a PWM-dimmed LED string, not a switch: GPIO6 is the board's
// BL_PWM net driving a FET on the LED cathode return. Brightness is expressed
// as a perceptual percentage; the board layer maps it through a gamma curve and
// clamps to a duty floor below which the boost converter is unstable.
#define ZD_BACKLIGHT_MAX_PERCENT 100

esp_err_t zd_board_backlight_init(void);
esp_err_t zd_board_backlight_set_percent(uint8_t percent);
// Ramps to percent over ms. Dimming and waking read as deliberate rather than
// as a fault when they are not instantaneous.
esp_err_t zd_board_backlight_fade_percent(uint8_t percent, uint32_t ms);
uint8_t zd_board_backlight_get_percent(void);
