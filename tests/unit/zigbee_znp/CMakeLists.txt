# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the ZNP server (lib/zigbee: MT framing, dispatch, SYS/UTIL, NV store) on
# native_sim, through the CONFIG_ZIGBEE_ZNP library.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_znp_unit)

target_sources(app PRIVATE src/main.c)
