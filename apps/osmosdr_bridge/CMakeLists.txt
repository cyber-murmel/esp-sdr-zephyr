# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_osmosdr_bridge)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# The USB function and the protocol are the osmosdr app's.
target_include_directories(app PRIVATE ../osmosdr/src)
target_sources(app PRIVATE src/main.c ../osmosdr/src/usb_sdr.c)
# The radio over SDIO, shared with the MicroPython app.
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_sdio_host.cmake)
