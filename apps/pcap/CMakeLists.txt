# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_sdr_pcap)

# USB: the CDC-ACM shell and DFU (app_usb.h); this app's vendor interface registers next to them.
include(${CMAKE_CURRENT_LIST_DIR}/../common/common.cmake)
# The radio over SDIO, the gapless stream's channelizer and the CPU clock, as the bridge has them.
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_sdio_host.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../common/esdr_pfbq.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../common/app_clock.cmake)

target_include_directories(app PRIVATE src ../osmosdr/src ../../include ../ubertooth/src/upstream)
target_sources(app PRIVATE
  src/main.c
  src/radio.c
  src/survey.c
  src/narrow.c
  src/burst.c
  src/decoder.c
  src/output.c
  src/usb_pcap.c)

# The decoders and the stream's format: plain C, unit tested on native_sim (tests/unit/pcap_*,
# pcapng), at -O2 for their loops. The 802.15.4 PHY is lib/ieee802154's (no radio needed), the
# Bluetooth LE whitening and CRC the vendored Ubertooth tables (GPL-2.0-or-later).
set(PCAP_DSP
  src/pcapng.c
  src/records.c
  src/dec_ble.c
  src/dec_wpan.c
  src/dec_generic.c
  src/refine.c
  ${CMAKE_CURRENT_LIST_DIR}/../../lib/ieee802154/ieee154_phy.c
  ${CMAKE_CURRENT_LIST_DIR}/../ubertooth/src/upstream/bluetooth_le.c)
target_sources(app PRIVATE ${PCAP_DSP})
set_source_files_properties(${PCAP_DSP} PROPERTIES COMPILE_OPTIONS "-O2")
# The stream thread's work per sample (the channel mode's detector, the survey's assembler).
set_source_files_properties(src/narrow.c src/survey.c PROPERTIES COMPILE_OPTIONS "-O2")
