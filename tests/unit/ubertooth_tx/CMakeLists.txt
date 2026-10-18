# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the transmit side of apps/ubertooth: the GFSK modulator (gfsk_mod.c) checked
# with an independent receiver and an ideal model, and the packet builders (ubt_tx.c) checked
# against the vendored upstream whitening/CRC and a bit-serial reference, on native_sim. The
# portable sources take the __ZEPHYR__ branch of ubt_attr.h (IRAM_ATTR from hal_espressif)
# inside any Zephyr app, so it is undefined for them.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ubertooth_tx_unit)

set(UBT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/ubertooth/src)
set(UBT_PORTABLE
  ${UBT_SRC}/gfsk_mod.c
  ${UBT_SRC}/ubt_tx.c
  ${UBT_SRC}/upstream/bluetooth_le.c)

target_sources(app PRIVATE src/main.c ${UBT_PORTABLE})
target_include_directories(app PRIVATE ${UBT_SRC} ${CMAKE_CURRENT_LIST_DIR}/../../../include)
set_source_files_properties(${UBT_PORTABLE} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
