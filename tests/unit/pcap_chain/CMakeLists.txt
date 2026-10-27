# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the pcap app's survey path for Bluetooth LE on native_sim: packets GFSK modulated
# at 80 MS/s off the centre, quantized to int8 as the C6 sends them, through the fixed-point
# channelizer (apps/common/esdr_pfbq.cmake) into one bin's burst, decoded by
# apps/pcap/src/dec_ble.c at the bin's 4 MS/s.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pcap_chain_unit)

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)

include(${REPO}/apps/common/esdr_pfbq.cmake)
target_sources(app PRIVATE
  src/main.c
  ${REPO}/apps/pcap/src/dec_ble.c
  ${REPO}/apps/ubertooth/src/upstream/bluetooth_le.c)
target_include_directories(app PRIVATE
  ${REPO}/include
  ${REPO}/apps/pcap/src
  ${REPO}/apps/ubertooth/src/upstream
  ${REPO}/tests/unit/ubertooth_le/src
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
