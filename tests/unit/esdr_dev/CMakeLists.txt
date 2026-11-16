# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the osmosdr protocol helpers (apps/osmosdr/src/esdr_dev.h and the block header
# check of esdr_proto.h) on native_sim. Header-only.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esdr_dev_unit)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../../apps/osmosdr/src)
