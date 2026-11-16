/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The half-band filter (-1 0 9 16 9 0 -1) / 32 that keeps a burst at half its rate, a block at a
 * time. Plain C, no Zephyr: the host test builds it too (tests/host/test_burst_filter.py).
 */

#ifndef ESDR_HB_H_
#define ESDR_HB_H_

#include <stdbool.h>
#include <stdint.h>

/* The last inputs, newest first, and how many came. All 0 at a burst's start. */
struct esdr_hb {
	int32_t i[7], q[7];
	uint32_t n;
};

/*
 * @p n input pairs at @p p (little endian int16 with @p wide, else int8) through the filter.
 * Returns the pairs written to @p out (int16 I, Q), at most (n + 1) / 2.
 */
uint32_t esdr_hb_block(struct esdr_hb *hb, const uint8_t *p, uint32_t n, bool wide, int16_t *out);

#endif /* ESDR_HB_H_ */
