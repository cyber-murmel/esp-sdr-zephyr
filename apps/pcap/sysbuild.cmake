# SPDX-License-Identifier: GPL-3.0-or-later

# MCUboot with the ESP32-P4 boards' flash layout, the one apps/micropython has there: the two
# apps update each other over DFU and leave its FAT volume in the upper 8 MB alone.
set(mcuboot_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_p4.overlay
    CACHE INTERNAL "MCUboot's devicetree overlay")
