/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ring sink's int16 samples to the sample ring's int8: MIN(x >> UBT_PACK_SHIFT, 127) for
 * di/dq[0..n) on the vector unit (ubt_pack.c, after a check at boot). Returns the samples done:
 * n, or 0 if the check failed (the caller's C loop then).
 */

#ifndef UBT_PACK_H_
#define UBT_PACK_H_

#include <stddef.h>
#include <stdint.h>

#define UBT_PACK_SHIFT 6

size_t ubt_pack(int8_t *di, int8_t *dq, const int16_t *i, const int16_t *q, size_t n);

#endif /* UBT_PACK_H_ */
