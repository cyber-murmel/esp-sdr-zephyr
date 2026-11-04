/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The half-band filter of a burst (esdr_hb.h). The delay line lives in locals, not memory: this
 * runs in the link's cooperative parser, which holds up everything else while a burst goes by.
 */

#pragma GCC optimize("O3")

#include "esdr_hb.h"

uint32_t esdr_hb_block(struct esdr_hb *hb, const uint8_t *p, uint32_t n, bool wide, int16_t *out)
{
	int32_t i0 = hb->i[0], i1 = hb->i[1], i2 = hb->i[2], i3 = hb->i[3];
	int32_t i4 = hb->i[4], i5 = hb->i[5], i6 = hb->i[6];
	int32_t q0 = hb->q[0], q1 = hb->q[1], q2 = hb->q[2], q3 = hb->q[3];
	int32_t q4 = hb->q[4], q5 = hb->q[5], q6 = hb->q[6];
	uint32_t count = hb->n;
	uint32_t made = 0;

	for (uint32_t k = 0; k < n; k++) {
		i6 = i5;
		i5 = i4;
		i4 = i3;
		i3 = i2;
		i2 = i1;
		i1 = i0;
		q6 = q5;
		q5 = q4;
		q4 = q3;
		q3 = q2;
		q2 = q1;
		q1 = q0;
		if (wide) {
			i0 = (int16_t)(p[0] | (uint16_t)p[1] << 8);
			q0 = (int16_t)(p[2] | (uint16_t)p[3] << 8);
			p += 4;
		} else {
			i0 = (int8_t)p[0];
			q0 = (int8_t)p[1];
			p += 2;
		}
		if ((++count & 1U) != 0U || count < 7U) {
			continue;
		}
		*out++ = (int16_t)((16 * i3 + 9 * (i2 + i4) - (i0 + i6)) / 32);
		*out++ = (int16_t)((16 * q3 + 9 * (q2 + q4) - (q0 + q6)) / 32);
		made++;
	}
	hb->i[0] = i0;
	hb->i[1] = i1;
	hb->i[2] = i2;
	hb->i[3] = i3;
	hb->i[4] = i4;
	hb->i[5] = i5;
	hb->i[6] = i6;
	hb->q[0] = q0;
	hb->q[1] = q1;
	hb->q[2] = q2;
	hb->q[3] = q3;
	hb->q[4] = q4;
	hb->q[5] = q5;
	hb->q[6] = q6;
	hb->n = count;
	return made;
}
