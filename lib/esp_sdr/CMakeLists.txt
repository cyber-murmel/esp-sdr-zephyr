# SPDX-License-Identifier: GPL-3.0-or-later

# The placement rules of include/esp_sdr/esp_sdr_place.h.
include(${CMAKE_CURRENT_LIST_DIR}/place.cmake)

if(NOT CONFIG_ESP_SDR_AS_EXTENSION)
  include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
  zephyr_library()
  zephyr_library_sources(${ESP_SDR_SOURCES})
  zephyr_library_include_directories(${ESP_SDR_INCLUDES})
endif()

# The dump banks. The S3 and C6 snippets pick their variant by Kconfig: on the
# S3 three banks for the continuous ring, two for loop playback ping-pong and
# captures into either; on the C6 two blocks for the gapless ring.
if(CONFIG_SOC_SERIES_ESP32C6)
  zephyr_linker_sources(SECTIONS esp_sdr_bank_c6.ld)
elseif(CONFIG_SOC_SERIES_ESP32C5)
  # Among the RAM sections, so that .noinit and .bss follow the bank.
  zephyr_linker_sources(RAM_SECTIONS esp_sdr_bank_c5.ld)
elseif(CONFIG_SOC_SERIES_ESP32)
  zephyr_linker_sources(RAM_SECTIONS esp_sdr_bank_esp32.ld)
elseif(CONFIG_SOC_SERIES_ESP32S31)
  zephyr_linker_sources(SECTIONS esp_sdr_bank_s31.ld)
elseif(CONFIG_SOC_SERIES_ESP32S3)
  zephyr_linker_sources(SECTIONS esp_sdr_bank_s3.ld)
else()
  message(FATAL_ERROR "esp-sdr: no capture bank for ${CONFIG_SOC_SERIES}")
endif()

if(CONFIG_ESP_SDR_AS_EXTENSION)
  return()
endif()

if(CONFIG_ESP_SDR_PHY_ONLY)
  zephyr_library_sources(${ESP_SDR_PHY_SOURCES})
  zephyr_library_compile_definitions(${ESP_SDR_PHY_DEFINES})
  esp_blobs_link(phy)
  zephyr_link_libraries(gcc)
  if(NOT CONFIG_SOC_SERIES_ESP32S3)
    zephyr_link_libraries(
      -T${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/esp_rom/${CONFIG_SOC_SERIES}/ld/${CONFIG_SOC_SERIES}.rom.phy.ld
    )
  endif()
endif()

if(CONFIG_SOC_SERIES_ESP32S31)
  # esp_sdr_s31.c: the PHY library's calls into the ROM.
  zephyr_link_libraries(-Wl,--wrap=phy_set_channel_rfpll_freq -Wl,--wrap=phy_write_gain_mem)
endif()

if(CONFIG_ESP_SDR_RFTEST)
  # Espressif RF test library, fetched with "west blobs fetch esp-sdr-zephyr".
  set(esp_sdr_rftest
      ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/blobs/lib/${CONFIG_SOC_SERIES}/librftest.a)
  zephyr_blobs_verify(FILES ${esp_sdr_rftest} REQUIRED)
  zephyr_link_libraries(${esp_sdr_rftest})
endif()
