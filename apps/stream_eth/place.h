/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * stream_eth's placement rules (esp_sdr_place.h), ahead of the SoC's. The sender's path stays in
 * flash unless CONFIG_APP_VRT_RAMFUNC asks for internal RAM, which is nearly full.
 */

#if !defined(CONFIG_APP_VRT_RAMFUNC)
#define ESP_SDR_PIN_CODE_ctl_done ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_data_done ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_build_frame ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_bench_done_cb ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_send_data ESP_SDR_FLASH
#endif
