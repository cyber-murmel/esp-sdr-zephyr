# SPDX-License-Identifier: GPL-3.0-or-later
#
# The library's sources and their flags, for its own build and for apps/multiapp, which builds
# them as modules (CONFIG_ESP_SDR_AS_EXTENSION). ESP_SDR_SOURCES: the library. ESP_SDR_INCLUDES:
# its private include directories. ESP_SDR_PHY_SOURCES and ESP_SDR_PHY_DEFINES: the radio's PHY
# without the Wi-Fi driver (CONFIG_ESP_SDR_PHY_ONLY).

# Upstream ESPARGOS esp-sdr (west project "esp-sdr", see west.yml): only its
# header-only helpers (tuning, LO plan, bandwidth curve) are used.
set(ESP_SDR_UPSTREAM_DIR "${WEST_TOPDIR}/modules/lib/esp-sdr" CACHE PATH
    "Checkout of github.com/ESPARGOS/esp-sdr")
if(NOT EXISTS ${ESP_SDR_UPSTREAM_DIR}/main/common/rx_tuning.h)
  message(FATAL_ERROR "esp-sdr: upstream checkout not found at ${ESP_SDR_UPSTREAM_DIR} "
                      "(west update, or set ESP_SDR_UPSTREAM_DIR)")
endif()

set(esp_sdr_src ${CMAKE_CURRENT_LIST_DIR}/src)
set(ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr.c ${esp_sdr_src}/esp_sdr_rx.c)
if(CONFIG_SOC_SERIES_ESP32S3)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_tx.c)
endif()
if(CONFIG_SOC_SERIES_ESP32S31)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_s31.c)
endif()
if(CONFIG_ESP_SDR_ARB)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_arb.c)
endif()
if(CONFIG_ESP_SDR_DIAG16)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_diag16.c)
endif()
if(CONFIG_ESP_SDR_PACED)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_paced.c)
endif()
if(CONFIG_ESP_SDR_TRIG)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_trig.c)
endif()
if(CONFIG_ESP_SDR_GAPLESS)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_gapless.c)
endif()
if(CONFIG_ESP_SDR_DIAG)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_diag.c)
endif()
if(CONFIG_ESP_SDR_REPLAY)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_replay.c)
endif()
if(CONFIG_ESP_SDR_DUMPRING)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_dumpring.c)
endif()
if(CONFIG_ESP_SDR_TX)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_tx_stream.c ${esp_sdr_src}/esp_sdr_tx_dac.c)
endif()
if(CONFIG_ESP_SDR_TX_DAC_SIMD)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_pie.S)
endif()
if(CONFIG_ESP_SDR_RING)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_ring.c ${esp_sdr_src}/esp_sdr_ring_fir.S)
endif()
if(CONFIG_ESP_SDR_RING_FIR)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_ring_dot.S)
endif()
if(CONFIG_ESP_SDR_RING_STAGE)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_ring_stage.c)
  set_source_files_properties(${esp_sdr_src}/esp_sdr_ring_stage.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(CONFIG_ESP_SDR_CHANCAP)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_chancap.c)
endif()
if(CONFIG_ESP_SDR_CPU_PROF)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_prof.c)
endif()
if(CONFIG_ESP_SDR_GAIN_GUARD)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_gain_guard.c)
endif()
if(CONFIG_ESP_SDR_TEMP)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_temp.c)
endif()
if(CONFIG_ESP_SDR_CALSTORE)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_calstore.c)
endif()
if(CONFIG_ESP_SDR_LTE_CAL)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_lte.c)
  # The correlation and demodulation loops.
  set_source_files_properties(${esp_sdr_src}/esp_sdr_lte.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(CONFIG_ESP_SDR_CHANCAP_SIMD)
  list(APPEND ESP_SDR_SOURCES ${esp_sdr_src}/esp_sdr_chancap_pie.S)
endif()
if(CONFIG_ESP_SDR_TRIG)
  # The snapshot packing loop.
  set_source_files_properties(${esp_sdr_src}/esp_sdr_trig.c PROPERTIES COMPILE_OPTIONS "-O3")
endif()
if(CONFIG_ESP_SDR_PACED AND CONFIG_SOC_SERIES_ESP32S31)
  # The poll's FIR: at 320 cycles a write (250 kS/s x4) the default -Os leaves little slack.
  set_source_files_properties(${esp_sdr_src}/esp_sdr_paced.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(CONFIG_ESP_SDR_GAPLESS)
  # The run's hot path (the default -Os costs it a third of its free time).
  set_source_files_properties(${esp_sdr_src}/esp_sdr_gapless.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(CONFIG_ESP_SDR_CHANCAP)
  # Per-sample loops of the channelizer and the sink.
  set_source_files_properties(${esp_sdr_src}/esp_sdr_chancap.c PROPERTIES COMPILE_OPTIONS
                              "-O3;-ffp-contract=fast")
endif()

set(ESP_SDR_INCLUDES ${ESP_SDR_UPSTREAM_DIR}/main/common)
if(CONFIG_ESP_SDR_DIAG OR CONFIG_ESP_SDR_DIAG16)
  # The hal's glue leaves the PARLIO low-level headers out of the include path.
  set(esp_sdr_hal ${WEST_TOPDIR}/modules/hal/espressif/components/esp_hal_parlio)
  list(APPEND ESP_SDR_INCLUDES ${esp_sdr_hal}/include ${esp_sdr_hal}/${CONFIG_SOC_SERIES}/include)
endif()

# What the hal builds for the PHY when Wi-Fi, BT or 802.15.4 is on.
set(esp_sdr_hal ${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR})
set(ESP_SDR_PHY_SOURCES
  ${esp_sdr_hal}/components/esp_phy/src/phy_init.c
  ${esp_sdr_hal}/components/esp_phy/src/lib_printf.c
  ${esp_sdr_hal}/components/esp_phy/src/phy_common.c
  ${esp_sdr_hal}/components/esp_phy/src/phy_override.c
  ${esp_sdr_hal}/components/esp_phy/${CONFIG_SOC_SERIES}/phy_init_data.c
)
# Zephyr defines the PHY's options only with a radio driver on.
set(ESP_SDR_PHY_DEFINES CONFIG_ESP_PHY_MAX_TX_POWER=20)
if(CONFIG_SOC_SERIES_ESP32S3)
  # The USB controller runs from the PLL the PHY calibrates.
  list(APPEND ESP_SDR_PHY_DEFINES CONFIG_ESP_PHY_ENABLE_USB)
endif()
if(CONFIG_SOC_SERIES_ESP32S31)
  # No PHY task may touch the radio behind a burst: as upstream's firmware.
  list(APPEND ESP_SDR_PHY_DEFINES CONFIG_ESP_PHY_DISABLE_PLL_TRACK=1)
endif()
