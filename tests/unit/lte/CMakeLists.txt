# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of include/esp_sdr/esp_sdr_lte.h (PSS search, SSS decode and the carrier offset from
# the phase between them) on native_sim, with synthetic synchronization symbols. Header-only.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lte_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
