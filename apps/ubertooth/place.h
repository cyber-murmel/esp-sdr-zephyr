/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ubertooth's placement rules (esp_sdr_place.h), for its sources (ESP_SDR_PLACE_SCOPE ubertooth),
 * ahead of the SoC's: the demodulator and the LE decoder run per sample from IRAM; the rest of the
 * receive path runs from flash, its hottest part in the hot block (hot_text_s3.ld). Pins marked
 * "as before" keep code and data where it was, each a candidate for a measured change
 * (doc/dev/code-placement.md).
 */

#define ESP_SDR_PLACE_ubertooth_SAMPLE ESP_SDR_IRAM
#define ESP_SDR_PLACE_ubertooth_CHUNK  ESP_SDR_FLASH
#define ESP_SDR_PLACE_ubertooth_EVENT  ESP_SDR_FLASH

/* As before: the per-poll chain on CPU 0 that was put in IRAM by hand. */
#define ESP_SDR_PIN_CODE_ubertooth_gfsk_demod_push    ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_ubt_le_feed        ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_ubt_rx_sess_poll   ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_ubt_radio_rx_read8 ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_rd_begin           ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_rd_commit          ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_ubt_pie_copy       ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_ubt_specan_frame   ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_ubertooth_plat_cycles        ESP_SDR_IRAM

/* As before: per symbol, from flash (the hot block). */
#define ESP_SDR_PIN_CODE_ubertooth_ubt_floor_hist_add ESP_SDR_FLASH

/* As before: the modulator's per-block and per-symbol parts that run from flash. */
#define ESP_SDR_PIN_CODE_ubertooth_gen_range       ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_ubertooth_sym_sum         ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_ubertooth_sym_edge        ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_ubertooth_gfsk_mod_gen_cb ESP_SDR_FLASH

/* As before: the BLE modes' per-packet state in the DRAM image (le_run() clears it). */
#define ESP_SDR_PIN_DATA_ubertooth_run ESP_SDR_DEFAULT

/* As before: the tables and the flag the hot loops read, in DRAM rather than flash. */
#define ESP_SDR_PIN_DATA_ubertooth_gfsk_log2_lut ESP_SDR_DRAM
#define ESP_SDR_PIN_DATA_ubertooth_wseq          ESP_SDR_DRAM
#define ESP_SDR_PIN_DATA_ubertooth_rev8          ESP_SDR_DRAM
#define ESP_SDR_PIN_DATA_ubertooth_pack_k        ESP_SDR_DRAM
#define ESP_SDR_PIN_DATA_ubertooth_pack_vec      ESP_SDR_DRAM
