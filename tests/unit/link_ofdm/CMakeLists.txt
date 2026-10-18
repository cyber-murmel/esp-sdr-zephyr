# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/link/src/ofdm.c and ofdm_frame.c (with qam.c for the CRCs, rs.c, bch.c) on
# native_sim: frames looped through a simulated channel and decoded, as tools/ofdm_sim.c does
# on the host. The sources are built with -U__ZEPHYR__, their host branch (float FFT, no
# esp-dsp).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(link_ofdm_unit)

set(LINK_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/link/src)
set(OFDM_SRCS ${LINK_SRC}/ofdm.c ${LINK_SRC}/ofdm_frame.c ${LINK_SRC}/qam.c ${LINK_SRC}/rs.c
  ${LINK_SRC}/hamming.c ${LINK_SRC}/bch.c)
target_sources(app PRIVATE src/main.c ${OFDM_SRCS})
target_include_directories(app PRIVATE
  ${LINK_SRC}
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
set_source_files_properties(${OFDM_SRCS} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
