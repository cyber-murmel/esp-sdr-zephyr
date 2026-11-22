# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the DMA ring's slab bookkeeping (lib/esp_sdr/src/esdr_dma_ring.c, no hardware: a
# fake descriptor table stands in) on native_sim: ownership of every slab, swap order, restart
# with a slab out, no spare.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dma_ring_unit)

target_sources(app PRIVATE src/main.c ${CMAKE_CURRENT_LIST_DIR}/../../../lib/esp_sdr/src/esdr_dma_ring.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
