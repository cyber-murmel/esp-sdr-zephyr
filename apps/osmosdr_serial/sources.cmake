# SPDX-License-Identifier: GPL-3.0-or-later
#
# The app's sources and include paths, for its own CMakeLists and for
# tests/regression/stream_gapless, which builds the app unmodified as its DUT.
#
# The protocol header is the osmosdr app's; the crystal calibration requests (ESDR_REQ_GET_CAL,
# ESDR_REQ_SET_CAL) are the shared ones. No settings store here: the WHY2025 badge's C6 has no
# RAM for one beside its gapless ring, its P4 keeps the record (apps/micropython's ltecal).

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../osmosdr/src
  ${CMAKE_CURRENT_LIST_DIR}/../../include)
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_cal.cmake)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/main.c)
target_sources_ifdef(CONFIG_APP_LINK_SDIO app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/link_sdio.c)
target_sources_ifdef(CONFIG_APP_FW_UPDATE app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/fw_update.c)
