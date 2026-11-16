# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/pcap's pcapng writer (pcapng.c) and packet records (records.c) on native_sim.
# Both are plain C without Zephyr dependencies, so they are compiled in directly, as the test of
# lib/ieee802154/ieee154_phy.c does.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pcapng_unit)

set(PCAP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../apps/pcap/src)

target_sources(app PRIVATE
  src/main.c
  ${PCAP_DIR}/pcapng.c
  ${PCAP_DIR}/records.c)
target_include_directories(app PRIVATE
  ${PCAP_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
