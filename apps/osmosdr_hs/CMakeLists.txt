# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

# The crystal calibration kept in settings on the storage partition, with the die temperature.
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/calstore.conf)
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/calstore.overlay)
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/dietemp.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_osmosdr_hs)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# The USB function and the protocol are the osmosdr app's.
target_include_directories(app PRIVATE ../osmosdr/src)
target_sources(app PRIVATE src/main.c ../osmosdr/src/usb_sdr.c)
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_cal.cmake)
