# SPDX-License-Identifier: GPL-3.0-or-later
#
# Unit test of the spectrum analyser math of apps/ubertooth (ubt_specan.c) on native_sim: synthetic
# 80 MS/s captures (tones, noise, DC and a spur, 10 bit) through the frame decode, the reference
# 16 bit FFT, the channel binning and the SPECAN packet packer. The portable source takes the
# __ZEPHYR__ branch of ubt_attr.h (IRAM_ATTR from hal_espressif) inside any Zephyr app, so it is
# undefined for it.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ubertooth_specan_unit)

set(UBT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../../apps/ubertooth/src)

target_sources(app PRIVATE src/main.c ${UBT_SRC}/ubt_specan.c)
target_include_directories(app PRIVATE ${UBT_SRC} ${CMAKE_CURRENT_LIST_DIR}/../../../include)
set_source_files_properties(${UBT_SRC}/ubt_specan.c PROPERTIES COMPILE_OPTIONS "-U__ZEPHYR__")
