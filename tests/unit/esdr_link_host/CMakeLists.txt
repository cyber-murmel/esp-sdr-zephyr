# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/common/src/esdr_link_host.c (the host side of the serial transport: requests
# and the splitter of the radio's stream) on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esdr_link_host_unit)

set(common ${CMAKE_CURRENT_LIST_DIR}/../../../apps/common)
target_sources(app PRIVATE src/main.c ${common}/src/esdr_link_host.c)
target_include_directories(app PRIVATE ${common}/include
  ${CMAKE_CURRENT_LIST_DIR}/../../../apps/osmosdr/src)
