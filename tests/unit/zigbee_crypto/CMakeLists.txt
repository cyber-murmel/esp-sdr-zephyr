# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of lib/zigbee/zb_crypto.c (AES-128, CCM*, the MMO hash) against vectors from
# independent implementations, on native_sim. The library has no Zephyr dependency but the
# MIN macro, so it is compiled directly as an extra source.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_crypto_unit)

target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../lib/zigbee/zb_crypto.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
