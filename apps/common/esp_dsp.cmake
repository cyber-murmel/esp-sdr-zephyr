# SPDX-License-Identifier: GPL-3.0-or-later
#
# esp-dsp's ESP32-S3 FFT kernels (west project esp-dsp), for the link app's OFDM and the
# MicroPython app's Scan: the kernels alone, its IDF scaffolding stays out. include() it, then
# esp_dsp_s3_fft(<var>) sets <var> to the kernels' files (float and 16-bit radix 2), to add to a
# target's sources. A caller defines what they expect of their surroundings (apps/link's ofdm.c,
# apps/micropython's cmod/esdr_scan_pie.c).

function(esp_dsp_s3_fft out_var)
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
