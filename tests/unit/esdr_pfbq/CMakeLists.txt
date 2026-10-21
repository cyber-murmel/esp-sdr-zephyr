# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/common/src/esdr_pfbq.c (the fixed-point channelizer and detector) against the
# float bank of esdr_pfb.c, on native_sim with the C kernels and on the ESP32-P4 with its PIE
# kernels, built as apps/common/esdr_pfbq.cmake builds them for the apps. On native_sim the
# benchmark reads the host's clock (../esdr_pfb/src/host_clock.c).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esdr_pfbq_unit)

include(${CMAKE_CURRENT_LIST_DIR}/../../../apps/common/esdr_pfb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../apps/common/esdr_pfbq.cmake)
target_sources(app PRIVATE src/main.c)
# The benchmark prints where the cycles go (esdr_pfbq_prof[]).
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../../apps/common/src/esdr_pfbq.c APPEND PROPERTY
  COMPILE_DEFINITIONS ESDR_PFBQ_PROFILE)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
if(CONFIG_ARCH_POSIX)
  target_sources(native_simulator INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../esdr_pfb/src/host_clock.c)
endif()
