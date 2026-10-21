# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/common/src/esdr_pfb.c (the P4's channelizer and burst detector) on native_sim,
# built as apps/common/esdr_pfb.cmake builds it for the apps. src/host_clock.c runs on the host
# side of the simulator: wall clock and core cycles for the benchmark.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esdr_pfb_unit)

include(${CMAKE_CURRENT_LIST_DIR}/../../../apps/common/esdr_pfb.cmake)
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/host_clock.c)
