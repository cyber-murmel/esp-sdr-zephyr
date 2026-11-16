/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Placement of the receive path's code and data by usage category (esp_sdr_place.h; the app's
 * rules in place.h). UBT_HOT(cat, fn): a function placed by its category and never inlined into a
 * caller placed elsewhere; on the host, plain code.
 */

#ifndef UBT_ATTR_H_
#define UBT_ATTR_H_

#include <esp_sdr/esp_sdr_place.h>

#if defined(__ZEPHYR__)
#define UBT_HOT(cat, fn) ESP_SDR_CODE(cat, fn) __attribute__((noinline))
#else
#define UBT_HOT(cat, fn)
#endif

#endif /* UBT_ATTR_H_ */
