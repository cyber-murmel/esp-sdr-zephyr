# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT CONFIG_ESP_SDR_AS_EXTENSION)
  include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
  zephyr_library()
  zephyr_library_sources(${IEEE154_SOURCES})
  # multiapp's stage macros (include/multiapp/stage.h): plain calls in this standalone build.
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/../../apps/multiapp/include)
endif()
