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
# The gapless stream's channelizer (ESDR_REQ_SET_CHANNELIZER), and the CPU clock.
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_pfbq.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../common/app_clock.cmake)
# Cycle counts of the channelizer's kernels (esdr_pfbq_prof[]) for ESDR_REQ_SET_CHANNELIZER's
# statistics.
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/../common/src/esdr_pfbq.c APPEND PROPERTY
  COMPILE_DEFINITIONS ESDR_PFBQ_PROFILE)
