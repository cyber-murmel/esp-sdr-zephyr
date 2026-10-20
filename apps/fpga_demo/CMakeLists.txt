# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_fpga_demo)

target_sources(app PRIVATE src/main.c)

# A bitstream from gateware/ (python -m esdr_fpga.build <variant>), embedded in the image.
set(ESDR_FPGA_IMAGE "" CACHE FILEPATH "iCE40 bitstream to embed")
if(ESDR_FPGA_IMAGE)
  set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(app ${ESDR_FPGA_IMAGE} ${gen_dir}/fpga_image.inc)
  target_compile_definitions(app PRIVATE ESDR_FPGA_IMAGE_EMBEDDED)
endif()
