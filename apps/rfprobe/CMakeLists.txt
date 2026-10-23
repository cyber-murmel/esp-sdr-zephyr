# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

# The crystal calibration ("probe cal") kept in settings on the storage partition, with the
# die temperature ("probe temp").
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/calstore.conf)
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/calstore.overlay)
list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/../common/dietemp.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_rfprobe)

# The ESP32-S3: USB shell and DFU on the OTG port.
if(CONFIG_USB_DEVICE_STACK_NEXT)
  include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)
endif()

target_sources(app PRIVATE src/main.c)
