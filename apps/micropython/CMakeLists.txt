# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

# The radio is on the chip (radio.conf) except on an ESP32-P4, whose radio is the board's
# ESP32-C6 behind a link. Under sysbuild the board is not known here yet: sysbuild.cmake adds
# radio.conf there.
if(BOARD AND NOT BOARD MATCHES "/+esp32p4")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/radio.conf)
endif()

# MicroPython is built by its Zephyr module (CONFIG_MICROPYTHON, modules/micropython/zephyr)
# while find_package() configures Zephyr, and provides main(). The module reads two things
# from here: this app's C modules (cmod/) and, for the frozen manifests, the directory of the
# bundled apps' index. The index is written below, once Kconfig says which apps the board
# has; the module's configure-time pass over the manifest only needs the file to exist.
set(USER_C_MODULES ${CMAKE_CURRENT_LIST_DIR}/cmod)
set(MICROPY_MANIFEST_EGGS_INDEX_DIR ${CMAKE_BINARY_DIR}/eggs_index)
set(EGGS_INDEX ${MICROPY_MANIFEST_EGGS_INDEX_DIR}/eggs/__init__.py)
if(NOT EXISTS ${EGGS_INDEX})
  file(WRITE ${EGGS_INDEX} "INDEX = {}\n")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_micropython)

# mpconfigport_esdr.h (CONFIG_MICROPY_CONFIGFILE), for the micropython library and its qstr scan.
zephyr_include_directories(src)

if(CONFIG_SOC_SERIES_ESP32C5)
  # The ESP32-C5: the REPL on the USB-Serial-JTAG console, MicroPython in the main thread
  # (the port's main()), its static heap in SRAM block 0 (sorted before the bank's snippet).
  zephyr_linker_sources(RAM_SECTIONS SORT_KEY app src/c5_low.ld)
elseif(CONFIG_SOC_SERIES_ESP32P4)
  # The ESP32-P4 (WHY2025 badge, ESP32-P4-Function-EV-Board): MicroPython in the main thread
  # (the port's main()), its heap in PSRAM (cmod/esdr_radio.c), the REPL on CDC-ACM and DFU
  # through the shared USB device on the OTG HS port, started by the port's mp_usbd_init()
  # (cmod/esdr_usb.c).
  include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)
  # The link to the radio and the USB glue, plain Zephyr code (outside MicroPython's include
  # path, whose assert.h hides the HAL's static_assert).
  target_sources(app PRIVATE cmod/esdr_remote.c cmod/esdr_mon.c cmod/esdr_usb.c cmod/esdr_gl.c)
  target_include_directories(app PRIVATE cmod ../osmosdr/src)
  # Its SDIO side, shared with the bridge app.
  include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_sdio_host.cmake)
  # The gapless stream's channelizer (cmod/esdr_gl.c): the fixed-point bank on PIE.
  include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_pfbq.cmake)
elseif(CONFIG_SOC_SERIES_ESP32)
  # The classic ESP32 (MCH2022 badge): the REPL on the console UART (the badge's RP2040 bridges
  # it to USB), MicroPython in the main thread (the port's main()), its heap in PSRAM
  # (cmod/esdr_radio.c), the radio on the chip behind the link's interface (cmod/esdr_local.c,
  # plain Zephyr code as esdr_remote.c is on the P4).
  target_sources(app PRIVATE cmod/esdr_local.c cmod/esdr_mon.c src/setjmp.S)
  target_include_directories(app PRIVATE cmod ../osmosdr/src)
  # The crystal calibration requests on the library (apps/common).
  include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_cal.cmake)
  # The ROM's setjmp spills windows by SYSCALL, as on the S3 (setjmp.S).
  zephyr_link_libraries("-Wl,--wrap=setjmp")
else()
  include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

  target_sources(app PRIVATE src/boot.c src/setjmp.S src/usb_disks.c)
  # ff.h: usb_disks.c refreshes a FatFs volume without remounting it.
  target_link_libraries(app PRIVATE ELMFAT)
  # See setjmp.S.
  zephyr_link_libraries("-Wl,--wrap=setjmp")
  # boot.c runs the port's main() as real_main() in a thread of its own.
  set_property(SOURCE ${ZEPHYR_MICROPYTHON_MODULE_DIR}/ports/zephyr/main.c
               TARGET_DIRECTORY micropython APPEND PROPERTY COMPILE_DEFINITIONS main=real_main)
endif()

# What this app changes in the module's build (mpy_module.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/mpy_module.cmake)
