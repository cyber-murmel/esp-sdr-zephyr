# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the plain logic of apps/mch2022_coproc (the MCH2022 badge's RP2040): the I2C
# register map and the DTR and RTS reset detectors, on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mch2022_coproc_unit)

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)

target_sources(app PRIVATE
  src/main.c
  ${REPO}/apps/mch2022_coproc/src/regmap.c
  ${REPO}/apps/mch2022_coproc/src/reset_detect.c)
target_include_directories(app PRIVATE
  ${REPO}/apps/mch2022_coproc/src
  ${REPO}/include)
