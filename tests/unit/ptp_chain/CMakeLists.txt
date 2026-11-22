# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of apps/ptp_chain/src/chain_math.c (stamp to ps, exchange pairing, offset and delay,
# least-squares line) on native_sim. The file has no Zephyr dependency.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ptp_chain_unit)

set(chain_src ${CMAKE_CURRENT_LIST_DIR}/../../../apps/ptp_chain/src)
target_sources(app PRIVATE src/main.c ${chain_src}/chain_math.c)
target_include_directories(app PRIVATE ${chain_src})
