# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of tests/common/src/esdr_pfb_ref.c (the float model of the P4's channelizer and burst
# detector, the reference of tests/unit/esdr_pfbq) on native_sim. tests/common/src/host_clock.c
# runs on the host side of the simulator: wall clock and core cycles for the benchmark.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esdr_pfb_unit)

include(${CMAKE_CURRENT_LIST_DIR}/../../common/esdr_pfb_ref.cmake)
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../../common/src/host_clock.c)
