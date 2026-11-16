# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/pcap/src/dec_ble.c (the Bluetooth LE decoder of the pcap app) on native_sim,
# with the vendored upstream whitening and CRC it uses (apps/ubertooth/src/upstream/bluetooth_le.c).
# Both are plain C and compiled in as extra sources; the benchmark reads the host's clock
# (tests/common/src/host_clock.c).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pcap_ble_unit)

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../../..)

target_sources(app PRIVATE
  src/main.c
  ${REPO}/apps/pcap/src/dec_ble.c
  ${REPO}/apps/ubertooth/src/upstream/bluetooth_le.c)
target_include_directories(app PRIVATE
  ${REPO}/include
  ${REPO}/apps/pcap/src
  ${REPO}/apps/ubertooth/src/upstream
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
if(CONFIG_ARCH_POSIX)
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../../common/src/host_clock.c)
endif()
