/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pcap app's placement rules (esp_sdr_place.h), for its sources (ESP_SDR_PLACE_SCOPE pcap),
 * ahead of the SoC's. Pins marked "as before" keep data where it was, each a candidate for a
 * measured change (doc/dev/code-placement.md).
 */

/* As before: the decoders' scratch in PSRAM, 136 KB and two of 176 KB. */
#define ESP_SDR_PIN_DATA_pcap_scratch     ESP_SDR_PSRAM_FILE
#define ESP_SDR_PIN_DATA_pcap_gen_scratch ESP_SDR_PSRAM_FILE
