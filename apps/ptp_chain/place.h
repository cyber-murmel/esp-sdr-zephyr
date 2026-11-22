/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ptp_chain's placement rules (esp_sdr_place.h): everything the receive interrupt and the burst
 * timer run is in IRAM, off the flash cache. chain_math.c's chain_rx and the tx records are
 * placed by chain_ram.ld.
 */

#define ESP_SDR_PIN_CODE_chain_hook ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_chain_send ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_chain_harvest ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_chain_slot_done ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_chain_burst ESP_SDR_IRAM
