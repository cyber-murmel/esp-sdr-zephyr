# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_micropython)

if(CONFIG_SOC_SERIES_ESP32C5)
  # The ESP32-C5: the REPL on the USB-Serial-JTAG console, MicroPython in the main thread
  # (the port's own start), its static heap in SRAM block 0 (sorted before the bank's snippet).
  set(MICROPY_APP_START ON)
  zephyr_linker_sources(RAM_SECTIONS SORT_KEY app src/c5_low.ld)
else()
  include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

  target_sources(app PRIVATE src/boot.c src/setjmp.S src/usb_disks.c)
  # ff.h: usb_disks.c refreshes a FatFs volume without remounting it.
  target_link_libraries(app PRIVATE ELMFAT)
  # See setjmp.S.
  zephyr_link_libraries("-Wl,--wrap=setjmp")
endif()

# MicroPython's Zephyr port with this app as the application (mpy_port.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/mpy_port.cmake)
