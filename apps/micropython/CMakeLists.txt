# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_micropython)

include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE src/boot.c src/setjmp.S src/usb_disks.c)
# ff.h: usb_disks.c refreshes a FatFs volume without remounting it.
target_link_libraries(app PRIVATE ELMFAT)
# See setjmp.S.
zephyr_link_libraries("-Wl,--wrap=setjmp")

# MicroPython's Zephyr port with this app as the application (mpy_port.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/mpy_port.cmake)
