# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the gapless receiver's threshold servo decision (lib/esp_sdr/src/
# esp_sdr_gapless_servo.h, plain C), on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gapless_servo_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src)
