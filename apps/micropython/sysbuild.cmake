# SPDX-License-Identifier: GPL-3.0-or-later

if(BOARD_QUALIFIERS MATCHES "esp32p4")
  # sysbuild/mcuboot.overlay gives MCUboot the S3 boards' flash layout; the ESP32-P4 boards
  # have their own. Their radio is an ESP32-C6 behind a link: no radio.conf.
  set(mcuboot_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_p4.overlay
      CACHE INTERNAL "MCUboot's devicetree overlay")
else()
  # The radio on the chip (CMakeLists.txt adds radio.conf itself without sysbuild, where it
  # knows the board).
  set(${DEFAULT_IMAGE}_EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/radio.conf
      CACHE INTERNAL "the radio on the chip")
endif()
