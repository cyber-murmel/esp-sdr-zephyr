# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared by the apps: USB device with the CDC-ACM shell and DFU through
# MCUboot (app_usb.h), system thread pinning (app_cpu.h), crash records
# across resets (app_crash.h).

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_sources(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/src/app_usb.c
  ${CMAKE_CURRENT_LIST_DIR}/src/app_cpu.c
  ${CMAKE_CURRENT_LIST_DIR}/src/app_crash.c)
