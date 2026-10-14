/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The vector unit FFT for ubt_specan (ubt_specan_fft_fn).
 */

#ifndef UBT_PIE_H_
#define UBT_PIE_H_

#include <stdint.h>

/* Copy the twiddle table into @p w_ram (UBT_SPECAN_N int16, 16 byte aligned, internal RAM) and use it. */
void ubt_pie_init(int16_t *w_ram);

/* 256 complex int16 pairs in place, 16 byte aligned; see ubt_specan.h for the contract. */
void ubt_pie_fft256(int16_t *iq);

#endif /* UBT_PIE_H_ */
