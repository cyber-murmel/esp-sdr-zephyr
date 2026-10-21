# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/link's modem and codecs on native_sim, one ztest suite each: RS(255,223)
# (link_rs), binary BCH (link_bch), extended Hamming(128,120) (link_hamming), the QAM modem
# (link_qam) and the OFDM modem (link_ofdm), frames looped through a simulated channel and
# decoded as tools/qam_sim.c and tools/ofdm_sim.c do on the host. The sources
# (apps/link/sources.cmake) take the __ZEPHYR__ branch (IRAM_ATTR from hal_espressif's
# esp_attr.h, not available here) inside any Zephyr app, native_sim included: they are built
# with -U__ZEPHYR__, their host branch (float FFT, no esp-dsp), at the test's optimization
# rather than the app's -O3. The sources are otherwise unmodified.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(link_unit)

include(${CMAKE_CURRENT_LIST_DIR}/../../../apps/link/sources.cmake)

target_sources(app PRIVATE src/rs.c src/bch.c src/hamming.c src/qam.c src/ofdm.c
  ${LINK_PORTABLE_SOURCES})
target_include_directories(app PRIVATE
  ${LINK_SRC_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
set_source_files_properties(${LINK_PORTABLE_SOURCES} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
