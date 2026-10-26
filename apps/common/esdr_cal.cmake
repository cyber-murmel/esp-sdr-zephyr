# SPDX-License-Identifier: GPL-3.0-or-later
#
# The protocol's crystal calibration requests on the library's correction (esdr_cal.h), for an
# app with the radio on its chip: osmosdr, osmosdr_serial, MicroPython's local radio.

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../osmosdr/src)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/esdr_cal.c)
