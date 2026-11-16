/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the fixed-point bank (esdr_pfbq.c) and the tests' float reference bank (esdr_pfb_ref.c)
 * share beyond esdr_pfb.h: the FFT's plan and input order, and the detector's floor rules.
 */

#ifndef ESDR_PFB_IMPL_H_
#define ESDR_PFB_IMPL_H_

#include <stddef.h>
#include <stdint.h>

#include "esdr_pfb.h"

/*
 * Detector: floor follower over quiet blocks; a quiet block this many times below the floor
 * replaces it (10 dB: noise of a right floor does not get there, even in a block of B / 2
 * frames); blocks loud in a row before a channel relearns.
 */
#define DET_FOLLOW  64.0f
#define DET_SNAP    10.0f
#define DET_RELEARN 2048U

/* Radices in stage order, the largest first: stage 1 runs without twiddles. */
static inline unsigned int esdr_pfb_plan(unsigned int m, uint8_t radix[4])
{
	static const uint8_t tab[][5] = {
		{16, 4, 4, 0, 0}, {20, 5, 4, 0, 0}, {32, 4, 4, 2, 0},
		{40, 5, 4, 2, 0}, {64, 4, 4, 4, 0}, {80, 5, 4, 4, 0},
	};

	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
		if (tab[i][0] == m) {
			unsigned int s = 0;

			for (; s < 4U && tab[i][s + 1U] != 0U; s++) {
				radix[s] = tab[i][s + 1U];
			}
			return s;
		}
	}
	return 0;
}

/* Twiddles of stage @p s (complex): column j = 1 .. span - 1, then q = 1 .. radix - 1. */
static inline size_t esdr_pfb_tw_count(const uint8_t *radix, unsigned int s)
{
	size_t span = 1;

	for (unsigned int i = 0; i < s; i++) {
		span *= radix[i];
	}
	return (span - 1U) * (radix[s] - 1U);
}

/* Where natural input i sits for the stages: the last stage's digit is the most significant. */
static inline unsigned int esdr_pfb_digit_rev(const uint8_t *radix, unsigned int stages,
					      unsigned int i)
{
	unsigned int span[4], pos = 0;

	span[0] = 1;
	for (unsigned int s = 1; s < stages; s++) {
		span[s] = span[s - 1U] * radix[s - 1U];
	}
	for (unsigned int s = stages; s-- > 0U;) {
		pos += (i % radix[s]) * span[s];
		i /= radix[s];
	}
	return pos;
}

/* The lower quartile of a channel's learning blocks (sorts them). */
static inline float esdr_det_lower_quartile(float *v)
{
	for (unsigned int i = 1; i < ESDR_DET_LEARN; i++) {
		float x = v[i];
		unsigned int j = i;

		for (; j > 0U && v[j - 1U] > x; j--) {
			v[j] = v[j - 1U];
		}
		v[j] = x;
	}
	return v[ESDR_DET_LEARN / 4U];
}

#endif /* ESDR_PFB_IMPL_H_ */
