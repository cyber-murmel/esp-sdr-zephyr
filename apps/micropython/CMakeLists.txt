# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

# The radio is on the chip (radio.conf) except on the WHY2025 badge's ESP32-P4, whose radio is
# the badge's ESP32-C6 behind a link.
if(NOT BOARD MATCHES "^why2025_badge/esp32p4")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/radio.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_micropython)

if(CONFIG_SOC_SERIES_ESP32C5)
  # The ESP32-C5: the REPL on the USB-Serial-JTAG console, MicroPython in the main thread
  # (the port's own start), its static heap in SRAM block 0 (sorted before the bank's snippet).
  set(MICROPY_APP_START ON)
  zephyr_linker_sources(RAM_SECTIONS SORT_KEY app src/c5_low.ld)
elseif(CONFIG_SOC_SERIES_ESP32P4)
  # The ESP32-P4 (WHY2025 badge): the REPL on the UART console, MicroPython in the main thread
  # (the port's own start), its heap in PSRAM (cmod/esdr_p4.c).
  set(MICROPY_APP_START ON)
  # The link to the radio, plain Zephyr code (outside MicroPython's include path, whose
  # assert.h hides the HAL's static_assert).
  target_sources(app PRIVATE cmod/esdr_remote.c)
  target_include_directories(app PRIVATE cmod ../osmosdr/src)
  # Its SDIO side, shared with the bridge app.
  include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_sdio_host.cmake)
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
