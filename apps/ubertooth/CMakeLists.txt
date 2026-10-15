# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_ubertooth)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE
  src/gfsk_demod.c
  src/gfsk_mod.c
  src/main.c
  src/shell.c
  src/ubt_clock.c
  src/ubt_ctrl.c
  src/ubt_dev.c
  src/ubt_le.c
  src/ubt_mode.c
  src/ubt_plat.c
  src/ubt_pie.c
  src/ubt_queue.c
  src/ubt_radio.c
  src/ubt_regs.c
  src/ubt_rx_run.c
  src/ubt_specan.c
  src/ubt_specan_run.c
  src/ubt_tx.c
  src/ubt_tx_run.c
  src/ubt_usb.c
  src/upstream/bluetooth_le.c)

# Per-sample loops of the DSP.
set_source_files_properties(src/ubt_specan.c src/gfsk_mod.c src/gfsk_demod.c src/ubt_le.c PROPERTIES COMPILE_OPTIONS "-O3")

# Only the ESP32-S3 16 bit FFT kernel of esp-dsp: its IDF scaffolding stays out (as in apps/link).
set(ESP_DSP_DIR "${WEST_TOPDIR}/modules/lib/esp-dsp")
set(ESP_DSP_FFT "${ESP_DSP_DIR}/modules/fft/fixed/dsps_fft2r_sc16_aes3.S")
if(NOT EXISTS "${ESP_DSP_FFT}")
  message(FATAL_ERROR "esp-dsp not found at ${ESP_DSP_DIR} (west project esp-dsp)")
endif()
target_sources(app PRIVATE "${ESP_DSP_FFT}")
set_source_files_properties(${ESP_DSP_FFT} PROPERTIES
  COMPILE_DEFINITIONS CONFIG_IDF_TARGET_ESP32S3=1
  INCLUDE_DIRECTORIES "${ESP_DSP_DIR}/modules/fft/include")
