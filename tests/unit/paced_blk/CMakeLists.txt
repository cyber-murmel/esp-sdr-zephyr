# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the paced transmit's block fill (lib/esp_sdr/src/esp_sdr_paced_blk.c) on native_sim:
# the DAC words equal those of the per-word path it replaces (a copy in the test), for every
# interpolation, random block sizes and fill steps, missing inputs and saturation.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(paced_blk_unit)

target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src/esp_sdr_paced_blk.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src)
# The ring's length: -DPBLOCKS=<n> (testcase.yaml runs 2 and 4).
if(NOT DEFINED PBLOCKS)
  set(PBLOCKS 4)
endif()
# The block size in words: -DPWORDS=<n>, a multiple of 8 (the miss maps are a bit an input).
if(NOT DEFINED PWORDS)
  set(PWORDS 128)
endif()
target_compile_definitions(app PRIVATE CONFIG_ESP_SDR_PACED_BLOCK_WORDS=${PWORDS}
  CONFIG_ESP_SDR_PACED_BLOCKS=${PBLOCKS})
