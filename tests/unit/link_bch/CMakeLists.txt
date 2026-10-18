# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/link/src/bch.c (binary BCH, the OFDM frames' light codes)
# on native_sim. As for link_rs: bch.c takes the __ZEPHYR__ branch (IRAM_ATTR
# from hal_espressif) inside any Zephyr app, so it is undefined for this file.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(link_bch_unit)

set(BCH_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/link/src/bch.c)

target_sources(app PRIVATE src/main.c ${BCH_SRC})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../apps/link/src
  ${CMAKE_CURRENT_LIST_DIR}/../../../include ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
set_source_files_properties(${BCH_SRC} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
