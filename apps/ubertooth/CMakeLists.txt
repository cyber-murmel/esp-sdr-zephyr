# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_ubertooth)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE
  src/gfsk_demod.c
  src/gfsk_mod.c
  src/le_conn.c
  src/main.c
  src/shell.c
  src/ubt_br.c
  src/ubt_br_run.c
  src/ubt_clock.c
  src/ubt_ctrl.c
  src/ubt_dev.c
  src/ubt_le.c
  src/ubt_mode.c
  src/ubt_plat.c
  src/ubt_pie.c
  src/ubt_pie_copy.S
  src/gfsk_pie.S
  src/gfsk_slice.S
  src/gfsk_groups.S
  src/ubt_pack8.S
  src/ubt_queue.c
  src/ubt_radio.c
  src/ubt_regs.c
  src/ubt_rssi.c
  src/ubt_rx_run.c
  src/ubt_rx_sess.c
  src/ubt_specan.c
  src/ubt_specan_run.c
  src/ubt_tx.c
  src/ubt_tx_run.c
  src/ubt_usb.c
  src/upstream/bluetooth_le.c
  src/ubt_pack.c)

# Per-sample loops of the DSP.
set_source_files_properties(src/ubt_specan.c src/gfsk_mod.c src/ubt_le.c src/ubt_br.c
  PROPERTIES COMPILE_OPTIONS "-O3")
# The demodulator's copy loops must stay loops: memcpy() here is the ROM's, a byte at a time.
set_source_files_properties(src/gfsk_demod.c
  PROPERTIES COMPILE_OPTIONS "-O3;-fno-tree-loop-distribute-patterns")
# The ring reader's int8 copy too (the rest of the file keeps the default -Os).
set_source_files_properties(src/ubt_radio.c
  PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

# The frozen reference demodulator for the bench's comparison, and the profiling split.
target_sources_ifdef(CONFIG_APP_UBERTOOTH_GFSK_REF app PRIVATE src/gfsk_ref.c)
if(CONFIG_APP_UBERTOOTH_GFSK_PROF)
  target_compile_definitions(app PRIVATE GFSK_PROF)
endif()

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

# CPU 0's hot flash code as one block at a fixed instruction cache position (CONFIG_ESP_SDR_HOT_TEXT,
# lib/esp_sdr/place.cmake): the list of hot_text_s3.ld.
esp_sdr_hot_text(hot_text_s3.ld)

# The app's placement rules (place.h) apply to its own sources.
target_compile_definitions(app PRIVATE ESP_SDR_PLACE_SCOPE=ubertooth)
