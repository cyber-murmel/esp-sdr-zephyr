# SPDX-License-Identifier: GPL-3.0-or-later

# Upstream ESPARGOS esp-sdr (west project "esp-sdr", see west.yml): only its
# header-only helpers (tuning, LO plan, bandwidth curve) are used.
set(ESP_SDR_UPSTREAM_DIR "${WEST_TOPDIR}/modules/lib/esp-sdr" CACHE PATH
    "Checkout of github.com/ESPARGOS/esp-sdr")
if(NOT EXISTS ${ESP_SDR_UPSTREAM_DIR}/main/common/rx_tuning.h)
  message(FATAL_ERROR "esp-sdr: upstream checkout not found at ${ESP_SDR_UPSTREAM_DIR} "
                      "(west update, or set ESP_SDR_UPSTREAM_DIR)")
endif()

zephyr_library()
zephyr_library_sources(
  src/esp_sdr.c
  src/esp_sdr_rx.c
)
zephyr_library_sources_ifdef(CONFIG_SOC_SERIES_ESP32S3 src/esp_sdr_tx.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_PACED src/esp_sdr_paced.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_TRIG src/esp_sdr_trig.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_GAPLESS src/esp_sdr_gapless.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_DIAG src/esp_sdr_diag.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_REPLAY src/esp_sdr_replay.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_TX src/esp_sdr_tx_stream.c src/esp_sdr_tx_dac.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_TX_DAC_SIMD src/esp_sdr_pie.S)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_RING src/esp_sdr_ring.c src/esp_sdr_ring_fir.S)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_RING_FIR src/esp_sdr_ring_dot.S)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_CHANCAP src/esp_sdr_chancap.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_TEMP src/esp_sdr_temp.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_CALSTORE src/esp_sdr_calstore.c)
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_LTE_CAL src/esp_sdr_lte.c)
if(CONFIG_ESP_SDR_LTE_CAL)
  # The correlation and demodulation loops.
  set_source_files_properties(src/esp_sdr_lte.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
zephyr_library_sources_ifdef(CONFIG_ESP_SDR_CHANCAP_SIMD src/esp_sdr_chancap_pie.S)
if(CONFIG_ESP_SDR_TRIG)
  # The snapshot packing loop.
  set_source_files_properties(src/esp_sdr_trig.c PROPERTIES COMPILE_OPTIONS "-O3")
endif()
if(CONFIG_ESP_SDR_GAPLESS)
  # The run's hot path (the default -Os costs it a third of its free time).
  set_source_files_properties(src/esp_sdr_gapless.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(CONFIG_ESP_SDR_CHANCAP)
  # Per-sample loops of the channelizer and the sink.
  set_source_files_properties(src/esp_sdr_chancap.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=fast")
endif()
zephyr_library_include_directories(${ESP_SDR_UPSTREAM_DIR}/main/common)
if(CONFIG_ESP_SDR_DIAG)
  # The hal's C5 glue leaves the PARLIO low-level headers out of the include path.
  set(esp_sdr_hal ${WEST_TOPDIR}/modules/hal/espressif/components/esp_hal_parlio)
  zephyr_library_include_directories(${esp_sdr_hal}/include ${esp_sdr_hal}/${CONFIG_SOC_SERIES}/include)
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
else()
  zephyr_linker_sources(SECTIONS esp_sdr_bank_s3.ld)
endif()

if(CONFIG_ESP_SDR_PHY_ONLY)
  # What the hal builds for the PHY when Wi-Fi, BT or 802.15.4 is on.
  set(esp_sdr_hal ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR})
  zephyr_library_sources(
    ${esp_sdr_hal}/components/esp_phy/src/phy_init.c
    ${esp_sdr_hal}/components/esp_phy/src/lib_printf.c
    ${esp_sdr_hal}/components/esp_phy/src/phy_common.c
    ${esp_sdr_hal}/components/esp_phy/src/phy_override.c
    ${esp_sdr_hal}/components/esp_phy/${CONFIG_SOC_SERIES}/phy_init_data.c
  )
  # Zephyr defines the PHY's options only with a radio driver on.
  zephyr_library_compile_definitions(CONFIG_ESP_PHY_MAX_TX_POWER=20)
  esp_blobs_link(phy)
  zephyr_link_libraries(
    gcc
    -T${esp_sdr_hal}/components/esp_rom/${CONFIG_SOC_SERIES}/ld/${CONFIG_SOC_SERIES}.rom.phy.ld
  )
endif()

if(CONFIG_ESP_SDR_RFTEST)
  # Espressif RF test library, fetched with "west blobs fetch esp-sdr-zephyr".
  set(esp_sdr_rftest
      ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/blobs/lib/${CONFIG_SOC_SERIES}/librftest.a)
  zephyr_blobs_verify(FILES ${esp_sdr_rftest} REQUIRED)
  zephyr_link_libraries(${esp_sdr_rftest})
endif()
