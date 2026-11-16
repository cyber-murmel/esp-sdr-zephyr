/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The link's placement rules (esp_sdr_place.h), for its sources (ESP_SDR_PLACE_SCOPE link), ahead
 * of the SoC's: the modems' per-sample and per-symbol loops run from IRAM, frame setup from flash.
 * Pins marked "as before" keep code and data where it was, each a candidate for a measured change
 * (doc/code-placement.md).
 */

#define ESP_SDR_PLACE_link_SAMPLE ESP_SDR_IRAM
#define ESP_SDR_PLACE_link_CHUNK  ESP_SDR_IRAM
#define ESP_SDR_PLACE_link_EVENT  ESP_SDR_FLASH

/* As before: the QAM frame builders, in IRAM. */
#define ESP_SDR_PIN_CODE_link_qam_tx_build      ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_link_qam_tx_build_pair ESP_SDR_IRAM

/* As before, flash by design: the 16- and 64-carrier OFDM paths and the BCH decoder. */
#define ESP_SDR_PIN_CODE_link_emit16         ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_link_tx_slot16      ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_link_sc_scan        ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_link_data_symbol_16 ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_link_data_symbol_64 ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_link_bch_decode     ESP_SDR_FLASH

/* As before: the decoders' state and the OFDM buffers in the DRAM image. */
#define ESP_SDR_PIN_DATA_link_qam_rx_ctx  ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_link_qam_rx2_ctx ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_link_bench_ram   ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_link_ofdm_tb     ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_link_ofdm_ack_tb ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_link_ofdm_wk2    ESP_SDR_DEFAULT
