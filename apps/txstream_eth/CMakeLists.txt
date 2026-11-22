# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_txstream_eth)

include(${CMAKE_CURRENT_LIST_DIR}/../../lib/esp_sdr/place.cmake)

set(txs_dir ${CMAKE_CURRENT_LIST_DIR}/../../lib/esp_sdr/src/txs)
# The cookie protocol of the control port and the header templates are stream_eth's, compiled from there.
set(eth_dir ${CMAKE_CURRENT_LIST_DIR}/../stream_eth/src)
target_sources(app PRIVATE src/main.c src/tx_boot.c src/tx_shell.c src/tx_stream.c src/tx_tone.c
  src/tx_net.c src/tx_sess.c ${eth_dir}/ctlproto.c ${eth_dir}/netframe.c
  ${txs_dir}/txs_flip.c ${txs_dir}/txs_jit.c ${txs_dir}/txs_credit.c
  ${txs_dir}/txs_wire.c ${txs_dir}/txs_expand.c)
target_sources_ifdef(CONFIG_APP_TXS_INJECT app PRIVATE src/tx_inject.c)
target_include_directories(app PRIVATE ${txs_dir} ${eth_dir})

# The PIE kernel: preprocessed by Zephyr's compiler (the placement macros), assembled by Espressif's
# assembler (xespv2p2), the rest by Zephyr's.
if(CONFIG_ESP_SDR_TXS_PIE)
  find_program(ESP_AS riscv32-esp-elf-as)
  if(NOT ESP_AS)
    message(FATAL_ERROR "CONFIG_ESP_SDR_TXS_PIE needs riscv32-esp-elf-as (the dev shell has it)")
  endif()
  set(pie_s ${CMAKE_CURRENT_BINARY_DIR}/txs_pie.s)
  set(pie_o ${CMAKE_CURRENT_BINARY_DIR}/txs_pie.o)
  add_custom_command(OUTPUT ${pie_o}
    COMMAND ${CMAKE_C_COMPILER} -E -P -x assembler-with-cpp -imacros ${AUTOCONF_H}
            -I${ZEPHYR_BINARY_DIR}/include/generated -I${CMAKE_CURRENT_LIST_DIR}/../../include
            -o ${pie_s} ${txs_dir}/txs_pie.S
    COMMAND ${ESP_AS} -march=rv32imafc_zicsr_zifencei_xespv2p2 -mabi=ilp32f -o ${pie_o} ${pie_s}
    DEPENDS ${txs_dir}/txs_pie.S ${ZEPHYR_BINARY_DIR}/include/generated/esp_sdr_place_rules.h
    VERBATIM)
  target_sources(app PRIVATE ${pie_o} ${txs_dir}/txs_pie.c)
  set_source_files_properties(${pie_o} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endif()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)

# net_buf_unref() and what it calls run on CPU 1, per packet: IRAM, not the flash cache
zephyr_linker_sources(RAMFUNC_SECTION ${CMAKE_CURRENT_LIST_DIR}/ram_netbuf.ld)

# The layout of the transmit image is checked after every link: PSRAM only where it is safe (a store fault at
# boot, or CPU 1 and the interrupts reaching it, otherwise), the rest of scripts/tcm_groups.py --tx. A failure
# fails the build.
if(CONFIG_ESP_SDR_S31_TXMEM)
  add_custom_target(txs_layout_check ALL
    COMMAND ${CMAKE_COMMAND} -E env READELF=${CMAKE_READELF} ${PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/../../scripts/tcm_groups.py
            ${ZEPHYR_BINARY_DIR}/${CONFIG_KERNEL_BIN_NAME}.elf --tx
    VERBATIM)
  add_dependencies(txs_layout_check ${logical_target_for_zephyr_elf})
endif()
target_compile_options(app PRIVATE -O2)
