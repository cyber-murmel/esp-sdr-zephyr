# SPDX-License-Identifier: GPL-3.0-or-later

if(BOARD_QUALIFIERS MATCHES "esp32s31")
  # sysbuild/mcuboot.overlay gives MCUboot the S3 boards' flash layout; the ESP32-S31 has its own.
  set(mcuboot_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_s31.overlay
      CACHE INTERNAL "MCUboot's devicetree overlay")
endif()
