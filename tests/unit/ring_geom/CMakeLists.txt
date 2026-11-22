# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the S3 ring's unit geometry (lib/esp_sdr/src/esp_sdr_ring_geom.h, plain C) for
# every unit length and several lateness limits, on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ring_geom_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src
  ${CMAKE_CURRENT_LIST_DIR}/../../../include)
