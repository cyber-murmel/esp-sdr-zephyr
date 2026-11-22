# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the TX sweep synthesis (lib/esp_sdr/src/esp_sdr_sweep.h) on native_sim: amplitude and
# phase against a double reference, and the DAC words against the old float recursion.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tx_sweep_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src)
