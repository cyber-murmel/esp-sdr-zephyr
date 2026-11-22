# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/stream_eth: the packetizer (vrt_pkt.c), the staging cells (vrt_cells.c) and the
# frame templates (netframe.c), against libvrt's own encoding, on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vrt_eth_unit)

set(APP ${CMAKE_CURRENT_LIST_DIR}/../../../apps/stream_eth/src)
target_sources(app PRIVATE
  src/main.c
  ${APP}/vrt_pkt.c
  ${APP}/vrt_cells.c
  ${APP}/ctlproto.c
  ${APP}/netframe.c)
target_include_directories(app PRIVATE ${APP} ${CMAKE_CURRENT_LIST_DIR}/../../../include)
