# SPDX-License-Identifier: GPL-3.0-or-later

# The esdr module: the radio and the board, for the gnuradio shim. The ESP32-C5's (no display,
# no PSRAM) is esdr_c5.c; the ESP32-P4's (its radio is the WHY2025 badge's ESP32-C6, over a
# link), the classic ESP32's and the ESP32-S31's (bursts from the radio on the chip) are
# esdr_radio.c; all have the board functions (esdr_board.c) and the packet modem (esdr_fsk.c,
# esdr_link.c).
add_library(usermod_esdr INTERFACE)
if(CONFIG_SOC_SERIES_ESP32C5)
  target_sources(usermod_esdr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/esdr_c5.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_board.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_fsk.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_link.c)
elseif(CONFIG_SOC_SERIES_ESP32P4 OR CONFIG_SOC_SERIES_ESP32 OR CONFIG_SOC_SERIES_ESP32S31)
  target_sources(usermod_esdr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/esdr_radio.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_lte.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_chan.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_board.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_gui.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_scan.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_wf.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_fsk.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_link.c)
  # The crash records of apps/common, built on the P4 (common.cmake).
  if(CONFIG_SOC_SERIES_ESP32P4)
    target_sources(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/esdr_crash.c)
  endif()
  # esp_sdr_dsp.h (esdr_gui.c): the library itself (CONFIG_ESP_SDR) is not built on the P4;
  # the link's protocol is apps/osmosdr's (esdr_proto.h).
  target_include_directories(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../osmosdr/src)
else()
  target_sources(usermod_esdr INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modesdr.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_crash.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_board.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_rx.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_tx.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_gui.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_scan.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_wf.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_fsk.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_link.c)
  # The Scan app's FFT on the S3's vector unit: esp-dsp's kernel, which the app's build adds
  # (apps/common/esp_dsp.cmake), and esp_sdr_xtensa.h.
  target_sources(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/esdr_scan_pie.c)
  target_include_directories(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../../../include)
endif()
# The Pcap app's packet capture (esdr_pcap.c) on every port but the C5's (no PSRAM) and an S3's
# without the ring's decimation by 4 (its only rates for packets, esdr.PACKET_RATES; the
# standalone image has no room for it): apps/pcap's
# decoders, records and pcapng writer, plain C at -O2 as there, and the table's dissector
# (esdr_pcap_dissect.c, pcap_view.py's in C); the 802.15.4 PHY of
# lib/ieee802154 (no radio needed) unless the firmware links the library itself (in multiapp the
# library is a module of its own, which MicroPython does not load), the Bluetooth LE whitening
# and CRC the vendored Ubertooth tables.
if(NOT CONFIG_SOC_SERIES_ESP32C5 AND (NOT CONFIG_SOC_SERIES_ESP32S3 OR CONFIG_ESP_SDR_RING_BOX4))
  set(ESDR_PCAP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../pcap/src)
  set(ESDR_PCAP_DSP
    ${CMAKE_CURRENT_LIST_DIR}/esdr_pcap_core.c
    ${CMAKE_CURRENT_LIST_DIR}/esdr_pcap_dissect.c
    ${ESDR_PCAP_DIR}/dec_ble.c
    ${ESDR_PCAP_DIR}/dec_wpan.c
    ${ESDR_PCAP_DIR}/dec_generic.c
    ${ESDR_PCAP_DIR}/refine.c
    ${ESDR_PCAP_DIR}/records.c
    ${ESDR_PCAP_DIR}/pcapng.c
    ${CMAKE_CURRENT_LIST_DIR}/../../ubertooth/src/upstream/bluetooth_le.c)
  if(NOT CONFIG_ESP_SDR_IEEE802154 OR CONFIG_ESP_SDR_AS_EXTENSION)
    list(APPEND ESDR_PCAP_DSP ${CMAKE_CURRENT_LIST_DIR}/../../../lib/ieee802154/ieee154_phy.c)
  endif()
  target_sources(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/esdr_pcap.c ${ESDR_PCAP_DSP})
  set_source_files_properties(${ESDR_PCAP_DSP} PROPERTIES COMPILE_OPTIONS "-O2")
  target_include_directories(usermod_esdr INTERFACE ${ESDR_PCAP_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../ubertooth/src/upstream
    ${CMAKE_CURRENT_LIST_DIR}/../../../include)
endif()
# The sampling profiler (esdr.prof_start()).
if(CONFIG_APP_PROF)
  target_sources(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/esdr_prof.c)
  # With SMP it times who holds irq_lock()'s global lock on CPU 0.
  if(CONFIG_SMP)
    zephyr_link_libraries(-Wl,--wrap=z_smp_global_lock -Wl,--wrap=z_smp_global_unlock)
  endif()
endif()
# The MCH2022 badge's FPGA (drivers/esdr_fpga): the fpga module.
if(CONFIG_ESDR_FPGA)
  target_sources(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}/esdr_fpga.c)
endif()
# app_crash.h of apps/common.
target_include_directories(usermod_esdr INTERFACE ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/../../common/include)
target_link_libraries(usermod INTERFACE usermod_esdr)
