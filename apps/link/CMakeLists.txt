# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_link)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

target_sources(app PRIVATE ${LINK_SOURCES})
# The app's placement rules (place.h) apply to its own sources.
target_compile_definitions(app PRIVATE ESP_SDR_PLACE_SCOPE=link)
target_compile_definitions(app PRIVATE QAM_SPS=${CONFIG_APP_SPS})

if(CONFIG_APP_OFDM_ESP_DSP)
  esp_dsp_s3_fft(esp_dsp_fft)
  target_sources(app PRIVATE ${esp_dsp_fft})
endif()
