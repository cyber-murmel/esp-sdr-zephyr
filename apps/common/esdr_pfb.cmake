# SPDX-License-Identifier: GPL-3.0-or-later
#
# The polyphase channelizer and burst detector (esdr_pfb.h) for an app on the ESP32-P4: the
# bridge and MicroPython. It is the hot loop of the gapless stream, so it builds at -O2 with
# multiply-adds fused (-std=c17 alone keeps them apart) whatever the app's optimization.

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfb.c)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfb.c PROPERTIES
  COMPILE_OPTIONS "-O2;-ffp-contract=fast")
