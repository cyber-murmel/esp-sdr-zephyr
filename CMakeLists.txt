# SPDX-License-Identifier: GPL-3.0-or-later

if(CONFIG_ESP_SDR)
  zephyr_include_directories(include)
endif()

add_subdirectory(lib)
