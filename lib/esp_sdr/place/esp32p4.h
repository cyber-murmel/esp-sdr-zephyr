/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-P4 placement rules (esp_sdr_place.h): the categories as the code was placed before them.
 * An app's rules come first (<app>/place.h); pins marked "as before" keep a function or buffer
 * where it was although its category says otherwise, each a candidate for a measured change
 * (doc/dev/code-placement.md).
 *
 * lib/esp_sdr does not build here; the apps' code and data follow these. IRAM is L2MEM; no high
 * region; PSRAM through the raw .ext_ram.bss section (EXT_RAM_BSS_ATTR places nothing in it here).
 */

/* Paced code: L2MEM. */
#ifndef ESP_SDR_PLACE_PACED_SAMPLE
#define ESP_SDR_PLACE_PACED_SAMPLE ESP_SDR_IRAM
#endif
#ifndef ESP_SDR_PLACE_PACED_CHUNK
#define ESP_SDR_PLACE_PACED_CHUNK ESP_SDR_IRAM
#endif
#ifndef ESP_SDR_PLACE_PACED_EVENT
#define ESP_SDR_PLACE_PACED_EVENT ESP_SDR_FLASH
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

/* Data: the DRAM image by default, bulk in PSRAM. */
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
