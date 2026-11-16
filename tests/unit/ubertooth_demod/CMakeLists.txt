# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the receive side of apps/ubertooth below the modes on native_sim: the GFSK
# demodulator (the frozen reference gfsk_ref.c, and gfsk_demod.c against it bit for bit in diff.c)
# against a GFSK generator of its own (gen.h), the RX_SYMBOLS block builder and
# hop sequence (ubt_br.c), the RSSI conventions and carrier sense (ubt_rssi.c). ubt_attr.h takes its
# __ZEPHYR__ branch (IRAM placement from hal_espressif) inside any Zephyr app, so it is undefined
# for the sources.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ubertooth_demod_unit)

set(UBT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/ubertooth/src)
set(UBT_DEMOD_SRC ${UBT_SRC}/gfsk_demod.c ${UBT_SRC}/gfsk_ref.c
  ${UBT_SRC}/ubt_br.c
  ${UBT_SRC}/ubt_rssi.c)

target_sources(app PRIVATE src/main.c src/diff.c ${UBT_DEMOD_SRC})
target_include_directories(app PRIVATE ${UBT_SRC}
  ${CMAKE_CURRENT_LIST_DIR}/../../../include ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
set_source_files_properties(${UBT_DEMOD_SRC} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
