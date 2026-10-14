# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the portable core of apps/ubertooth: the vendor request
# dispatcher, the Bluetooth clock, the USB packet queue and the register file,
# on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ubertooth_core_unit)

set(UBT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/ubertooth/src)

target_sources(app PRIVATE
  src/main.c
  ${UBT_SRC}/ubt_clock.c
  ${UBT_SRC}/ubt_ctrl.c
  ${UBT_SRC}/ubt_dev.c
  ${UBT_SRC}/ubt_queue.c
  ${UBT_SRC}/ubt_regs.c)
target_include_directories(app PRIVATE ${UBT_SRC})
