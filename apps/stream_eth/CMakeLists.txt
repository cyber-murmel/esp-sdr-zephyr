# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_stream_eth)

include(${CMAKE_CURRENT_LIST_DIR}/../../lib/esp_sdr/place.cmake)

target_sources(app PRIVATE
  src/main.c
  src/radio.c
  src/txpath.c
  src/ctl.c
  src/shell.c
  src/vrt_pkt.c
  src/vrt_cells.c
  src/ctlproto.c
  src/netframe.c)
target_compile_options(app PRIVATE -O2)
