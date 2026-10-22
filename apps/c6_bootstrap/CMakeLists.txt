# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_c6_bootstrap)

# The ESP32-C6 image to install: zephyr/zephyr.bin of a build for the C6 with
# CONFIG_ESP_IDF_BOOTLOADER (apps/osmosdr_serial for esp32p4_function_ev_board/esp32c6/hpcore).
# Without one the app only reports the C6's firmware version.
set(C6_IMAGE "" CACHE FILEPATH "ESP32-C6 application image to install")

target_sources(app PRIVATE src/main.c)
if(C6_IMAGE)
  if(NOT EXISTS "${C6_IMAGE}")
    message(FATAL_ERROR "C6_IMAGE '${C6_IMAGE}' does not exist")
  endif()
  target_compile_definitions(app PRIVATE C6_IMAGE_BUILT_IN=1)
  generate_inc_file_for_target(app ${C6_IMAGE}
    ${ZEPHYR_BINARY_DIR}/include/generated/c6_image.inc)
endif()
