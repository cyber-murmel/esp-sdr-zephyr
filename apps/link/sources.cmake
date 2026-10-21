# SPDX-License-Identifier: GPL-3.0-or-later
#
# The link app's sources and their flags, for its own build (CMakeLists.txt), multiapp's link
# extension and the unit tests (tests/unit/link). include() it: source properties hold per
# directory.
#
# LINK_SOURCES: the whole app; LINK_PORTABLE_SOURCES: the modem and the codecs, plain C that the
# unit tests build on the host as well.

set(LINK_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(LINK_PORTABLE_SOURCES
  ${LINK_SRC_DIR}/qam.c
  ${LINK_SRC_DIR}/rs.c
  ${LINK_SRC_DIR}/bch.c
  ${LINK_SRC_DIR}/hamming.c
  ${LINK_SRC_DIR}/ofdm.c
  ${LINK_SRC_DIR}/ofdm_frame.c)
set(LINK_SOURCES
  ${LINK_SRC_DIR}/main.c
  ${LINK_SRC_DIR}/mac.c
  ${LINK_SRC_DIR}/perf.c
  ${LINK_PORTABLE_SOURCES}
  ${LINK_SRC_DIR}/ofdm_test.c)
# Per-sample and per-byte loops of the modem and the codec. Zephyr's ISO C mode turns contraction
# off: fused multiply-adds halve the FPU work.
set_source_files_properties(${LINK_SRC_DIR}/qam.c ${LINK_SRC_DIR}/ofdm.c PROPERTIES
  COMPILE_OPTIONS "-O3;-ffp-contract=fast")
set_source_files_properties(${LINK_SRC_DIR}/rs.c ${LINK_SRC_DIR}/bch.c ${LINK_SRC_DIR}/hamming.c
  PROPERTIES COMPILE_OPTIONS -O3)

# Only the ESP32-S3 float FFT kernel of esp-dsp, for CONFIG_APP_OFDM_ESP_DSP: its IDF scaffolding
# stays out. Sets out_var to the kernel's files.
function(link_esp_dsp_fft out_var)
  set(dir "${WEST_TOPDIR}/modules/lib/esp-dsp")
  set(files "${dir}/modules/fft/float/dsps_fft2r_fc32_aes3_.S"
            "${dir}/modules/fft/fixed/dsps_fft2r_sc16_aes3.S")
  foreach(f ${files})
    if(NOT EXISTS "${f}")
      message(FATAL_ERROR "esp-dsp not found at ${dir} (west project esp-dsp)")
    endif()
  endforeach()
  # Its platform header enables the S3 kernel by the IDF target macro.
  set_source_files_properties(${files} PROPERTIES
    COMPILE_DEFINITIONS CONFIG_IDF_TARGET_ESP32S3=1
    INCLUDE_DIRECTORIES "${dir}/modules/fft/include")
  set(${out_var} ${files} PARENT_SCOPE)
endfunction()
