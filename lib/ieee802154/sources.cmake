# SPDX-License-Identifier: GPL-3.0-or-later
#
# The driver's sources and flags, for its own build and for apps/multiapp's module.

set(IEEE154_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/ieee154_phy.c
  ${CMAKE_CURRENT_LIST_DIR}/ieee154_esp_sdr.c
)
# Runs at 4 MS/s on one core.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ieee154_phy.c PROPERTIES COMPILE_OPTIONS -O3)
