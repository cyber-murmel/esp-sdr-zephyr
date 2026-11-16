/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osmosdr_serial's placement rules (esp_sdr_place.h), ahead of the SoC's. Pins marked "as before"
 * keep code and data where it was, each a candidate for a measured change (doc/code-placement.md).
 */

/* As before: the paced loops themselves run from flash. */
#define ESP_SDR_PIN_CODE_osmosdr_serial_sdio_out ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_out_until ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_in_until ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_run_gapless ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_out_commit ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_rx_emit ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_diag_iq_sink ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_run_rx_fast ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_run_rx ESP_SDR_FLASH

#if !defined(CONFIG_ESP_SDR_GAPLESS)
/*
 * As before: without the gapless receiver, what the paced runs call runs from flash and the
 * buffers sit in the DRAM image (with it, in SRAM and block 3).
 */
#define ESP_SDR_PIN_CODE_osmosdr_serial_req_feed ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_link_byte ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_in_service ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_finish ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_reserve ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_commit ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_service ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_add ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_answer_stats ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_osmosdr_serial_gl_stop ESP_SDR_FLASH
#define ESP_SDR_PIN_DATA_osmosdr_serial_out ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_osmosdr_serial_bounce ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_osmosdr_serial_fr ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_osmosdr_serial_fast_tx ESP_SDR_DEFAULT
#endif
