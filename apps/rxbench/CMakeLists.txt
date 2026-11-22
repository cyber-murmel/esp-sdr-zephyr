# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_rxbench)

include(${CMAKE_CURRENT_LIST_DIR}/../../lib/esp_sdr/place.cmake)

target_sources(app PRIVATE src/main.c src/rxb.c src/shell.c)
target_sources_ifdef(CONFIG_ESP_SDR_S31_TXMEM app PRIVATE src/canary.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_compile_options(app PRIVATE -O2)
