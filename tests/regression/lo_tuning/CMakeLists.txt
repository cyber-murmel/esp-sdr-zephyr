# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two-board hardware regression test of the LO tuning (pytest/test_lo_tuning.py, see
# doc/testing.md): the ESP32-S3 transmits tones, the ESP32-C6 captures them. The DUT image is
# the apps/rfprobe application, unmodified, as ieee802154_link builds apps/wpan: src/main.c by
# path, APPLICATION_CONFIG_DIR and KCONFIG_ROOT pointing Zephyr's board-aware config lookup at
# the app. Only sysbuild.conf is a copy: sysbuild looks it up here, before this file runs
# (VERSION links the repository's, as the app's does).

cmake_minimum_required(VERSION 3.20.0)

set(RFPROBE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../apps/rfprobe)
set(APPLICATION_CONFIG_DIR ${RFPROBE_DIR})
set(KCONFIG_ROOT ${RFPROBE_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lo_tuning_regression)

# ESP32-S3: USB shell and DFU on the OTG port, as in the app's own CMakeLists.
if(CONFIG_USB_DEVICE_STACK_NEXT)
  include(${RFPROBE_DIR}/../common/common.cmake)
endif()

target_sources(app PRIVATE ${RFPROBE_DIR}/src/main.c)
