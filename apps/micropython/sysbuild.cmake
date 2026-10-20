# SPDX-License-Identifier: GPL-3.0-or-later

if(BOARD STREQUAL "why2025_badge" AND BOARD_QUALIFIERS MATCHES "esp32p4")
  # sysbuild/mcuboot.overlay gives MCUboot the S3 boards' flash layout; the WHY2025 badge's
  # ESP32-P4 has its own. Its radio is the badge's ESP32-C6: no radio.conf.
  set(mcuboot_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_why2025_p4.overlay
      CACHE INTERNAL "MCUboot's devicetree overlay")
else()
  # The radio on the chip (CMakeLists.txt adds radio.conf itself without sysbuild, where it
  # knows the board).
  set(${DEFAULT_IMAGE}_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/radio.conf
      CACHE INTERNAL "the radio on the chip")
endif()
