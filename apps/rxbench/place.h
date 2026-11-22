/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * rxbench's placement rules (esp_sdr_place.h), ahead of the SoC's: the hook runs in the receive
 * interrupt at every frame and the consumer at every frame, so with CONFIG_APP_RXB_RAMFUNC they
 * stay out of the flash cache.
 */

#if defined(CONFIG_APP_RXB_RAMFUNC)
#define ESP_SDR_PIN_CODE_rxb_hook ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_rxb_batch ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_rxb_consume ESP_SDR_IRAM
#endif
