# SPDX-License-Identifier: GPL-3.0-or-later

# The esdr module: the radio and the board, for the gnuradio shim.
add_library(usermod_esdr INTERFACE)
target_sources(usermod_esdr INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/modesdr.c
  ${CMAKE_CURRENT_LIST_DIR}/esdr_rx.c
  ${CMAKE_CURRENT_LIST_DIR}/esdr_tx.c
  ${CMAKE_CURRENT_LIST_DIR}/esdr_gui.c)
# app_crash.h of apps/common.
target_include_directories(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
target_link_libraries(usermod INTERFACE usermod_esdr)
