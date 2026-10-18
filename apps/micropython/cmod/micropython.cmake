# SPDX-License-Identifier: GPL-3.0-or-later

# The esdr module: the radio and the board, for the gnuradio shim. The ESP32-C5's (no display,
# no PSRAM) is esdr_c5.c; both have the packet modem (esdr_fsk.c, esdr_link.c).
add_library(usermod_esdr INTERFACE)
if(CONFIG_SOC_SERIES_ESP32C5)
  target_sources(usermod_esdr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/esdr_c5.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_fsk.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_link.c)
else()
  target_sources(usermod_esdr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modesdr.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_rx.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_tx.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_gui.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_fsk.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_link.c)
endif()
# app_crash.h of apps/common.
target_include_directories(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
target_link_libraries(usermod INTERFACE usermod_esdr)
