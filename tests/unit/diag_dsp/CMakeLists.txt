# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of lib/esp_sdr/src/esp_sdr_diag_dsp.h (the diagnostic-bus ring's box16 and CIC
# decimator) on native_sim. Header-only.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(diag_dsp_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
