# SPDX-License-Identifier: GPL-3.0-or-later

add_subdirectory_ifdef(CONFIG_ESP_SDR esp_sdr)
add_subdirectory_ifdef(CONFIG_ESP_SDR_IEEE802154 ieee802154)
add_subdirectory_ifdef(CONFIG_ZIGBEE_ZNP zigbee)
add_subdirectory_ifdef(CONFIG_ESP_SDR_BOOT_SELECT boot_select)

# The S31 transmit layout needs no library: a RAM_SECTIONS snippet ahead of the SoC's .noinit.
if(CONFIG_ESP_SDR_S31_TXMEM)
  zephyr_linker_sources(RAM_SECTIONS ${CMAKE_CURRENT_LIST_DIR}/esp_sdr/esp_sdr_txmem_s31.ld)
endif()

# Group 1 free without the library (with it esp_sdr_bank_s31.ld asserts the same).
if(CONFIG_ESP_SDR_S31_GROUP1 AND NOT CONFIG_ESP_SDR AND NOT CONFIG_ESP_SDR_S31_TXMEM)
  zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_LIST_DIR}/esp_sdr/esp_sdr_group1_s31.ld)
endif()
