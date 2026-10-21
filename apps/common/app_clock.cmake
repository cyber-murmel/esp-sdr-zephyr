# SPDX-License-Identifier: GPL-3.0-or-later
#
# The CPU at its top clock (app_clock.h): 400 MHz on ESP32-P4 silicon v3.0 and later, in a
# build for that family; elsewhere the clock stays as the devicetree set it.

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/app_clock.c)
