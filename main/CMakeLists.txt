idf_component_register(
    SRCS
        "app_main.c"
        "board/waveshare_esp32_s3_lcd_driver_board.c"
        "display/hd458002c40.c"
        "input/button.c"
        "input/input.c"
        "protocol/zapdisplay_protocol.c"
        "runtime/session.c"
        "runtime/tick.c"
        "settings/settings.c"
        "ui/canvas.c"
        "ui/generated_fonts.c"
        "ui/generated_logo.c"
        "ui/scenes.c"
    INCLUDE_DIRS "."
    REQUIRES esp_app_format esp_driver_gpio esp_driver_ledc esp_driver_usb_serial_jtag esp_lcd esp_psram esp_timer nvs_flash spi_flash
)
