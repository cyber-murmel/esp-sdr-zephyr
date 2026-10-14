# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_ubertooth)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE
  src/main.c
  src/shell.c
  src/ubt_clock.c
  src/ubt_ctrl.c
  src/ubt_dev.c
  src/ubt_mode.c
  src/ubt_plat.c
  src/ubt_queue.c
  src/ubt_regs.c
  src/ubt_usb.c
  src/upstream/bluetooth_le.c)
