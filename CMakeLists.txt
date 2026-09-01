cmake_minimum_required(VERSION 3.16)

# PROJECT_VER is embedded in the app descriptor and reported by the zapdisplay
# INFO and READY responses, so releases must bump it here.
set(PROJECT_VER "0.1.0")

# The only supported target. Set here rather than left to `idf.py set-target`
# so a fresh checkout builds without a manual step; without it the build falls
# back to esp32 and fails on every ESP32-S3 GPIO.
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zaparoo_display_firmware)
