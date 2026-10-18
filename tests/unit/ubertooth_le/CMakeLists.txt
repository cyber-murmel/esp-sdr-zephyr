# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the BLE packet finder, the promiscuous mode helpers and the connection
# follower of apps/ubertooth (ubt_le.c, le_conn.c) against the independent reference in
# apps/ubertooth/tools/le_ref.py, on native_sim. ubt_attr.h takes its __ZEPHYR__ branch (IRAM
# placement from hal_espressif) inside any Zephyr app, so it is undefined for the sources.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ubertooth_le_unit)

set(UBT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/ubertooth/src)
set(UBT_LE_SRC ${UBT_SRC}/ubt_le.c ${UBT_SRC}/le_conn.c ${UBT_SRC}/upstream/bluetooth_le.c)

target_sources(app PRIVATE src/main.c ${UBT_LE_SRC})
target_include_directories(app PRIVATE ${UBT_SRC} src
  ${CMAKE_CURRENT_LIST_DIR}/../../../include ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
set_source_files_properties(${UBT_LE_SRC} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
