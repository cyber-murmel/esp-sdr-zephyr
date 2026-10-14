/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Placement of the hot DSP loops: IRAM on the target (flash cache misses would
 * cost the 60 cycles per sample the receiver has), plain code on the host.
 */

#ifndef UBT_ATTR_H_
#define UBT_ATTR_H_

#if defined(__ZEPHYR__)
#include <esp_attr.h>
#define UBT_HOT IRAM_ATTR __attribute__((noinline))
#else
#define UBT_HOT
#endif

#endif /* UBT_ATTR_H_ */
