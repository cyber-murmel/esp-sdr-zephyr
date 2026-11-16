/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * With CONFIG_ESP_SDR_HOT_TEXT (lib/esp_sdr/place.cmake), in SECTIONS after the flash text: the
 * link stops when the hot block's snippet did not reach the image. In SECTIONS an ASSERT takes no
 * semicolon (inside an output section it needs one, hot_text.ld.in).
 */

ASSERT(DEFINED(__esp_sdr_hot_text_start),
       "CONFIG_ESP_SDR_HOT_TEXT: no hot block, the linker script took no TEXT_SECTIONS snippet")
