/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S31 placement rules (esp_sdr_place.h): the categories as the code was placed before them.
 * An app's rules come first (<app>/place.h); pins marked "as before" keep a function or buffer
 * where it was although its category says otherwise, each a candidate for a measured change
 * (doc/code-placement.md).
 *
 * IRAM is the TCM, as is all internal RAM; no high region; PSRAM through the raw .ext_ram.bss
 * section (EXT_RAM_BSS_ATTR places nothing in it here).
 */

/* Paced code: TCM, below group 2 (the dump ring turns groups 2 and 3). */
#ifndef ESP_SDR_PLACE_PACED_SAMPLE
#define ESP_SDR_PLACE_PACED_SAMPLE ESP_SDR_IRAM
#endif
#ifndef ESP_SDR_PLACE_PACED_CHUNK
#define ESP_SDR_PLACE_PACED_CHUNK ESP_SDR_IRAM
#endif
#ifndef ESP_SDR_PLACE_PACED_EVENT
#define ESP_SDR_PLACE_PACED_EVENT ESP_SDR_FLASH
#endif

/* As before: the receiver's gain refresh, called once a unit or run, from flash. */
#ifndef ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh_after
#define ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh_after ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh
#define ESP_SDR_PIN_CODE_esp_sdr_rx_gain_refresh ESP_SDR_FLASH
#endif

/* As before: the paced receive poll, once a sample, from flash; the dump burst in TCM. */
#ifndef ESP_SDR_PIN_CODE_esp_sdr_rx_paced_poll
#define ESP_SDR_PIN_CODE_esp_sdr_rx_paced_poll ESP_SDR_FLASH
#endif
#ifndef ESP_SDR_PIN_CODE_dump_run
#define ESP_SDR_PIN_CODE_dump_run ESP_SDR_IRAM
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

/* Data: TCM by default, bulk in PSRAM. */
#ifndef ESP_SDR_PLACE_STATE
#define ESP_SDR_PLACE_STATE ESP_SDR_DEFAULT
#endif
#ifndef ESP_SDR_PLACE_SCRATCH
#define ESP_SDR_PLACE_SCRATCH ESP_SDR_DEFAULT
#endif
#ifndef ESP_SDR_PLACE_STACK
#define ESP_SDR_PLACE_STACK ESP_SDR_DEFAULT
#endif
#ifndef ESP_SDR_PLACE_BULK
#define ESP_SDR_PLACE_BULK ESP_SDR_PSRAM_FILE
#endif
#ifndef ESP_SDR_PLACE_TABLE
#define ESP_SDR_PLACE_TABLE ESP_SDR_DEFAULT
#endif
