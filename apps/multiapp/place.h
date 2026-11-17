/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * multiapp's placement rules (esp_sdr_place.h), ahead of the SoC's: the host's (ESP_SDR_PLACE_SCOPE
 * multiapp) and those of the apps it builds as extensions, whose scope is the extension's name.
 * Pins marked "as before" keep data where it was, each a candidate for a measured change
 * (doc/dev/code-placement.md).
 */

#include "../capture/place.h"
#include "../link/place.h"
#include "../micropython/place.h"
#include "../osmosdr/place.h"

/* Not zeroed at boot, llext zeroes what it needs: zeroing them would slow every boot. */
#define ESP_SDR_PIN_DATA_multiapp_instr_heap ESP_SDR_PSRAM_NOINIT
#define ESP_SDR_PIN_DATA_multiapp_data_heap  ESP_SDR_PSRAM_NOINIT
/* One heap for both where code runs at the address it is written at (ESP32-S31). */
#define ESP_SDR_PIN_DATA_multiapp_ext_heap   ESP_SDR_PSRAM_NOINIT

/* As before: the standalone modules' heap in the DRAM image. */
#define ESP_SDR_PIN_DATA_multiapp_module_heap_mem ESP_SDR_NOINIT

/* As before: the app's stack in PSRAM like its data, internal RAM kept for the dump banks. */
#define ESP_SDR_PIN_DATA_multiapp_app_stack ESP_SDR_PSRAM
