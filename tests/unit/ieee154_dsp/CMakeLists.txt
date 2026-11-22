# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of lib/ieee802154/ieee154_dsp.h (the 802.15.4 driver's signal
# processing: DAC waveform generation and the receive mix/decimate) on
# native_sim. Header-only, but its inline functions call into
# ieee154_phy.c (ieee154_phy_chips()), so that is compiled in too. The driver's
# hot-swap kernels for its waveform generator (apps/wpan/kernels) are linked in.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ieee154_dsp_unit)

set(IEEE154_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../lib/ieee802154)

target_sources(app PRIVATE
  src/main.c
  ${IEEE154_DIR}/ieee154_phy.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../include
  ${IEEE154_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)

# The driver's hot-swap kernels (apps/wpan/kernels, stage tx_gen), linked in as they are: the test
# calls their functions through the descriptors they export, the way the multiapp host does. Both
# export "multiapp_kernel", so each gets a name of its own here.
set(WPAN_KERNELS ${CMAKE_CURRENT_LIST_DIR}/../../../apps/wpan/kernels)
target_sources(app PRIVATE ${WPAN_KERNELS}/tx_gen_ref.c ${WPAN_KERNELS}/tx_gen_alt.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../apps/multiapp/include)
set_source_files_properties(${WPAN_KERNELS}/tx_gen_ref.c PROPERTIES COMPILE_DEFINITIONS
  "MULTIAPP_NAME=tx_gen_ref;MULTIAPP_STAGE_ABI_tx_gen=0x600dU;multiapp_kernel=multiapp_kernel_ref")
set_source_files_properties(${WPAN_KERNELS}/tx_gen_alt.c PROPERTIES COMPILE_DEFINITIONS
  "MULTIAPP_NAME=tx_gen_alt;MULTIAPP_STAGE_ABI_tx_gen=0x600dU;multiapp_kernel=multiapp_kernel_alt")
