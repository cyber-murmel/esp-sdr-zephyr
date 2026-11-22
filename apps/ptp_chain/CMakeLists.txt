# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_ptp_chain)

include(${CMAKE_CURRENT_LIST_DIR}/../../lib/esp_sdr/place.cmake)

target_sources(app PRIVATE src/main.c src/chain.c src/chain_math.c src/stats.c src/shell.c
  src/pps.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_compile_options(app PRIVATE -O2)

# The hook's helpers in IRAM (the hook itself by place.h)
zephyr_linker_sources(RAMFUNC_SECTION ${CMAKE_CURRENT_LIST_DIR}/chain_ram.ld)
