# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of lib/zigbee/zb_frame.c (NWK and APS frames, the security header, the Transport-Key
# command) on native_sim. Compiled directly, with the crypto it uses.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_frame_unit)

target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/zigbee/zb_frame.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/zigbee/zb_crypto.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
