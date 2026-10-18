# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/link/src/qam.c (with its FEC, rs.c and hamming.c) on native_sim: frames
# looped through a simulated channel and decoded, as tools/qam_sim.c does on the host. The
# sources are built with -U__ZEPHYR__, their host branch (no ESP-IDF attributes).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(link_qam_unit)

set(LINK_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/link/src)
set(QAM_SRCS ${LINK_SRC}/qam.c ${LINK_SRC}/rs.c ${LINK_SRC}/hamming.c)
target_sources(app PRIVATE src/main.c ${QAM_SRCS})
target_include_directories(app PRIVATE
  ${LINK_SRC}
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
set_source_files_properties(${QAM_SRCS} PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
