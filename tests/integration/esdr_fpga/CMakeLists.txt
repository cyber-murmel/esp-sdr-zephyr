# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)

# The bitstream to test: ESDR_FPGA_IMAGE in the environment, or -DCONFIG_ESDR_FPGA_IMAGE=.
if(DEFINED ENV{ESDR_FPGA_IMAGE} AND NOT DEFINED CONFIG_ESDR_FPGA_IMAGE)
  set(CONFIG_ESDR_FPGA_IMAGE "$ENV{ESDR_FPGA_IMAGE}" CACHE STRING "")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esdr_fpga)
target_sources(app PRIVATE src/main.c)
