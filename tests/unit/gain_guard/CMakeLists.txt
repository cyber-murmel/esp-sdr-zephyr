# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the receive gain's clip guard (lib/esp_sdr/src/esp_sdr_gain_guard.c) on native_sim,
# with apps/ubertooth's thresholds.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gain_guard_unit)

target_sources(app PRIVATE src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src/esp_sdr_gain_guard.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
