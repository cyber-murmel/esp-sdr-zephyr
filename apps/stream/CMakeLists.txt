# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_stream)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE
  src/main.c
  src/vrt_rx.c
  src/iq_pack.c
  src/transport_udp.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/shell.c)
target_sources_ifdef(CONFIG_APP_TX app PRIVATE src/vrt_tx.c)
# Per-sample loops on the stream's critical path.
set_source_files_properties(src/iq_pack.c PROPERTIES COMPILE_OPTIONS -O3)
