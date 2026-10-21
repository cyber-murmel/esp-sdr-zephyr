# SPDX-License-Identifier: GPL-3.0-or-later
#
# The fixed-point channelizer and detector (esdr_pfbq.h) for an app, with the prototype design
# (esdr_pfb.cmake). On the ESP32-P4 its kernels are PIE SIMD in esdr_pfbq_pie.S, which only
# Espressif's assembler takes (the Zephyr SDK's knows no xesppie): with riscv32-esp-elf-as on the
# PATH (default.nix's dev shell has it) they are assembled and linked, else the C kernels run.

include(${CMAKE_CURRENT_LIST_DIR}/esdr_pfb.cmake)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfbq.c)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfbq.c PROPERTIES
  COMPILE_OPTIONS "-O2")

if(CONFIG_SOC_SERIES_ESP32P4)
  find_program(ESDR_ESP_RISCV_AS riscv32-esp-elf-as)
  if(ESDR_ESP_RISCV_AS)
    set(pie_src ${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfbq_pie.S)
    set(pie_obj ${CMAKE_CURRENT_BINARY_DIR}/esdr_pfbq_pie.o)
    # No relaxation: the hardware loops' ends are words the assembler resolves itself.
    add_custom_command(OUTPUT ${pie_obj}
      COMMAND ${ESDR_ESP_RISCV_AS} -march=rv32imafc_zicsr_zifencei_xesppie -mabi=ilp32f
              -mno-relax ${pie_src} -o ${pie_obj}
      DEPENDS ${pie_src}
      COMMENT "Assembling esdr_pfbq_pie.S (PIE)")
    target_sources(app PRIVATE ${pie_obj})
    set_source_files_properties(${pie_obj} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/esdr_pfbq.c PROPERTIES
      COMPILE_DEFINITIONS ESDR_PFBQ_PIE)
  else()
    message(WARNING "esdr_pfbq: riscv32-esp-elf-as not found: the C reference kernels run "
      "on the P4, some 40 times slower than PIE (use default.nix's dev shell)")
  endif()
endif()
