// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Zaparoo Project Contributors.

#include "board/board.h"
#include "display/display_profile.h"
#include "display/hd458002c40.h"
#include "protocol/zapdisplay_protocol.h"
#include "runtime/tick.h"
#include "settings/settings.h"
#include "ui/state.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

static const char *TAG = "zapdisplay";

// init_nvs prepares the settings store. A partition that is full or was written
// by a different NVS version is recoverable by erasing it: the only thing
// stored there is device settings, and losing those is far better than booting
// without a working store.
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s); settings will not persist", esp_err_to_name(ret));
    }
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    const size_t psram_size = esp_psram_get_size();

    esp_chip_info(&chip_info);

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGW(TAG, "Unable to detect flash size");
    }

    ESP_LOGI(TAG, "Zaparoo Display Firmware");
    ESP_LOGI(TAG, "ESP-IDF: %s", IDF_VER);
    ESP_LOGI(TAG, "Chip cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Flash size: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, "PSRAM size: %lu MB", (unsigned long)(psram_size / (1024 * 1024)));
    zd_board_log_profile();
    zd_display_log_profile();
    init_nvs();
    zd_settings_init();

    zapdisplay_state_t state;
    zapdisplay_state_set_defaults(&state);

    ESP_ERROR_CHECK(zd_board_backlight_init());
    ESP_ERROR_CHECK(zd_hd458002c40_init());
    ESP_ERROR_CHECK(zd_hd458002c40_render(&state));
    // Only now is there something worth lighting. zd_tick_init brings the
    // backlight up at the configured brightness, so the panel never flashes at
    // full and never shows an uninitialised framebuffer.
    zd_tick_init();
    zapdisplay_protocol_run();
}
