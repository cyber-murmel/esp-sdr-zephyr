/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The capture app's placement rules (esp_sdr_place.h), for its sources (ESP_SDR_PLACE_SCOPE
 * capture, here and as multiapp's extension), ahead of the SoC's. Pins marked "as before" keep
 * data where it was, each a candidate for a measured change (doc/dev/code-placement.md).
 */

/* As before: the FFT's work arrays in the DRAM image. */
#define ESP_SDR_PIN_DATA_capture_re  ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_capture_im  ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_capture_psd ESP_SDR_DEFAULT
