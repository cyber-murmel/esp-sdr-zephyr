# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/pcap's follow mode decisions (follow.c, plain C) on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pcap_follow_unit)

set(PCAP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../apps/pcap/src)

target_sources(app PRIVATE src/main.c ${PCAP_DIR}/follow.c)
target_include_directories(app PRIVATE ${PCAP_DIR})
