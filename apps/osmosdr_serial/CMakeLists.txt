# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_osmosdr_serial)

# The protocol header is the osmosdr app's; the crystal calibration requests (ESDR_REQ_GET_CAL,
# ESDR_REQ_SET_CAL) are the shared ones. No settings store here: the WHY2025 badge's C6 has no
# RAM for one beside its gapless ring, its P4 keeps the record (apps/micropython's ltecal).
target_include_directories(app PRIVATE ../osmosdr/src)
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_cal.cmake)
target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_APP_LINK_SDIO app PRIVATE src/link_sdio.c)
target_sources_ifdef(CONFIG_APP_FW_UPDATE app PRIVATE src/fw_update.c)
