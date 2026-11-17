/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osmosdr's placement rules (esp_sdr_place.h), ahead of the SoC's. Pins marked "as before" keep
 * code where it was, each a candidate for a measured change (doc/dev/code-placement.md).
 */

/* As before: the ring's sink and what it calls run on the ring's CPU from flash. */
#define ESP_SDR_PIN_CODE_osmosdr_block_close ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_block_open  ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_put_items   ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_sink_pack   ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_sink_nco    ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_sink        ESP_SDR_FLASH
