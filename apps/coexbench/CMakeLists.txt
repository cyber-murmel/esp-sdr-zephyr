# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_coexbench)

# USB: the CDC-ACM shell and DFU (app_usb.h).
include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)

target_sources(app PRIVATE src/main.c src/phylog.c src/espnow.c src/sdrwin.c)

# The PHY event log: the blob's channel calls go through src/phylog.c first; its PHY power calls
# are wrapped by the radio arbiter (CONFIG_ESP_SDR_ARB), which reports them to phylog.c.
zephyr_ld_options(
  -Wl,--wrap=phy_change_channel
  -Wl,--wrap=chip_v7_set_chan
  -Wl,--wrap=chip_v7_set_chan_misc
  -Wl,--wrap=set_chanfreq)
