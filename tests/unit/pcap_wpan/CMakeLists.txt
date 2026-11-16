# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/pcap/src/dec_wpan.c (the 802.15.4 decoder of the pcap app) and of the
# receiver instances of lib/ieee802154/ieee154_phy.c, end to end through the fixed-point
# channelizer (apps/common/esdr_pfbq.cmake) on native_sim. The PHY and the decoder have no Zephyr
# dependencies and are compiled in as extra sources, as tests/unit/ieee154_phy does.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pcap_wpan_unit)

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)

include(${REPO}/apps/common/esdr_pfbq.cmake)
target_sources(app PRIVATE
  src/main.c
  ${REPO}/lib/ieee802154/ieee154_phy.c
  ${REPO}/apps/pcap/src/dec_wpan.c)
target_include_directories(app PRIVATE
  ${REPO}/include
  ${REPO}/apps/pcap/src
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
