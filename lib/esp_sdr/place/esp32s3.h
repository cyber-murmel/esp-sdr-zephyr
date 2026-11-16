/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S3 placement rules (esp_sdr_place.h): the categories as the code was placed before them.
 * An app's rules come first (<app>/place.h); pins marked "as before" keep a function or buffer
 * where it was although its category says otherwise, each a candidate for a measured change
 * (doc/code-placement.md).
 */

/* Paced code: IRAM, the receive ring and its sinks run there with interrupts masked. */
#ifndef ESP_SDR_PLACE_PACED_SAMPLE
#define ESP_SDR_PLACE_PACED_SAMPLE ESP_SDR_IRAM
#endif
#ifndef ESP_SDR_PLACE_PACED_CHUNK
#define ESP_SDR_PLACE_PACED_CHUNK ESP_SDR_IRAM
#endif
#ifndef ESP_SDR_PLACE_PACED_EVENT
#define ESP_SDR_PLACE_PACED_EVENT ESP_SDR_FLASH
#endif

/* As before: the ring calls these once a unit, from flash. */
#ifndef ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh_after
#define ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh_after ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh
#define ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh ESP_SDR_FLASH
#endif
/* As before: once a transmit session, from IRAM. */
#ifndef ESP_SDR_PIN_CODE_esp_sdr_dac_halt
#define ESP_SDR_PIN_CODE_esp_sdr_dac_halt ESP_SDR_IRAM
#endif

/* As before: the transmit DAC's player and filler run once a block from flash. */
#ifndef ESP_SDR_PIN_CODE_tx_dac_timed_fill
#define ESP_SDR_PIN_CODE_tx_dac_timed_fill ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_tx_dac_fill_block
#define ESP_SDR_PIN_CODE_tx_dac_fill_block ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_tx_dac_wait_until_locked
#define ESP_SDR_PIN_CODE_tx_dac_wait_until_locked ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_tx_dac_loop_start
#define ESP_SDR_PIN_CODE_tx_dac_loop_start ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_tx_dac_filler
#define ESP_SDR_PIN_CODE_tx_dac_filler ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_tx_dac_player
#define ESP_SDR_PIN_CODE_tx_dac_player ESP_SDR_FLASH
#endif
/*
 * As before: the transmit DAC's stage and stacks above the banks only with
 * CONFIG_ESP_SDR_TX_DAC_HIGH_RAM, else in the DRAM image.
 */
#if !defined(CONFIG_ESP_SDR_TX_DAC_HIGH_RAM)
#ifndef ESP_SDR_PIN_DATA_tx_dac_stage
#define ESP_SDR_PIN_DATA_tx_dac_stage ESP_SDR_DEFAULT
#endif
#ifndef ESP_SDR_PIN_DATA_tx_dac_player_stack
#define ESP_SDR_PIN_DATA_tx_dac_player_stack ESP_SDR_NOINIT
#endif
#ifndef ESP_SDR_PIN_DATA_tx_dac_filler_stacks
#define ESP_SDR_PIN_DATA_tx_dac_filler_stacks ESP_SDR_NOINIT
#endif
#endif

/* As before: chancap's filter kernels, written once at setup, read per block above the banks. */
#ifndef ESP_SDR_PIN_DATA_chancap_ddck
#define ESP_SDR_PIN_DATA_chancap_ddck ESP_SDR_HIGH
#endif

/* Buffered code: flash, unless an app's rules say otherwise. */
#ifndef ESP_SDR_PLACE_SAMPLE
#define ESP_SDR_PLACE_SAMPLE ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PLACE_CHUNK
#define ESP_SDR_PLACE_CHUNK ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PLACE_EVENT
#define ESP_SDR_PLACE_EVENT ESP_SDR_FLASH
#endif

/* Data: working state and stacks above the banks, bulk in PSRAM, tables where const goes. */
#ifndef ESP_SDR_PLACE_STATE
#define ESP_SDR_PLACE_STATE ESP_SDR_HIGH
#endif
#ifndef ESP_SDR_PLACE_SCRATCH
#define ESP_SDR_PLACE_SCRATCH ESP_SDR_HIGH
#endif
#ifndef ESP_SDR_PLACE_STACK
#define ESP_SDR_PLACE_STACK ESP_SDR_HIGH
#endif
#ifndef ESP_SDR_PLACE_BULK
#define ESP_SDR_PLACE_BULK ESP_SDR_PSRAM
#endif
#ifndef ESP_SDR_PLACE_TABLE
#define ESP_SDR_PLACE_TABLE ESP_SDR_DEFAULT
#endif
