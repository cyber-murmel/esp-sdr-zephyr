# SPDX-License-Identifier: GPL-3.0-or-later
#
# The channelizer's definition (esdr_pfb.h) and its prototype design, for esdr_pfbq.cmake and
# the tests' float reference. The design builds with the same flags everywhere (-O2, multiply-adds
# fused, which -std=c17 alone keeps apart), so that every app and test of a target gets the same
# taps bit for bit.

include_guard(DIRECTORY)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../../include)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfb.c)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfb.c PROPERTIES
  COMPILE_OPTIONS "-O2;-ffp-contract=fast")
