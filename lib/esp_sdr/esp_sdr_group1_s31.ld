/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CONFIG_ESP_SDR_S31_GROUP1 without CONFIG_ESP_SDR (with it esp_sdr_bank_s31.ld does this): the
 * image ends below TCM group 1, the libc arena takes what is left of group 0.
 */

_heap_sentry = 0x2F020000;
_libc_heap_size = _heap_sentry - _end;

ASSERT(_end <= 0x2F020000, "esp-sdr: image end is not below TCM group 1")
