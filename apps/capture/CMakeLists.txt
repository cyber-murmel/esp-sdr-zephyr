# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_capture)

target_sources(app PRIVATE src/main.c)
# The app's placement rules (place.h, esp_sdr_place.h).
target_compile_definitions(app PRIVATE ESP_SDR_PLACE_SCOPE=capture)
