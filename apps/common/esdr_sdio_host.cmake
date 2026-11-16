# SPDX-License-Identifier: GPL-3.0-or-later
#
# The WHY2025 badge's ESP32-C6 radio over SDIO, for an app on its ESP32-P4
# (esdr_sdio_host.h): the bridge and MicroPython.

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../osmosdr/src)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/esdr_sdio_host.c)
