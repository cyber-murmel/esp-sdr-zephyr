# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT CONFIG_ESP_SDR_AS_EXTENSION)
  include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
  zephyr_library()
  zephyr_library_sources(${IEEE154_SOURCES})
endif()
