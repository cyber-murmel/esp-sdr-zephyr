# SPDX-License-Identifier: GPL-3.0-or-later

zephyr_library()
zephyr_library_sources(esdr_fpga.c esdr_fpga_dsp.c)

if(CONFIG_ESDR_FPGA_IMAGE_EMBEDDED)
  set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(${ZEPHYR_CURRENT_LIBRARY} ${CONFIG_ESDR_FPGA_IMAGE}
                               ${gen_dir}/esdr_fpga_image.inc)
endif()

zephyr_library_sources_ifdef(CONFIG_MIPI_DBI_ESDR_FPGA mipi_dbi_esdr_fpga.c)
