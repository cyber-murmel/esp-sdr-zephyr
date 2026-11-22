# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the radio arbiter's wake predictor (lib/esp_sdr/src/esp_sdr_arb_pred.h, plain C)
# on synthetic Wi-Fi enable times, on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(arb_pred_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src)
