# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the transmit stream's plain C parts (lib/esp_sdr/src/txs) on native_sim: position
# model, flip scheduler, expansion kernels, jitter cursor, credits, wire format, and an end to end
# simulation (a model DAC, a model sender, the module in between), and the network run's control state
# machine and the receive hook's filter (apps/txstream_eth/src/tx_sess.c, tx_filter.h).
# -DTXS_DIR=<dir> builds the library from another directory (the test of the tests: a copy with
# a deliberate bug must fail).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(txs_unit)

if(NOT DEFINED TXS_DIR)
  set(TXS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src/txs)
endif()
target_sources(app PRIVATE
  src/test_pos.c src/test_flip.c src/test_jit.c src/test_expand.c src/test_wire.c
  src/test_credit.c src/test_limits.c src/test_sim.c src/test_sess.c src/test_filter.c src/test_tone.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../apps/txstream_eth/src/tx_sess.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../apps/txstream_eth/src/tx_tone.c
  ${TXS_DIR}/txs_flip.c ${TXS_DIR}/txs_jit.c ${TXS_DIR}/txs_credit.c ${TXS_DIR}/txs_wire.c
  ${TXS_DIR}/txs_expand.c)
target_include_directories(app PRIVATE ${TXS_DIR} ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../../apps/txstream_eth/src
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
