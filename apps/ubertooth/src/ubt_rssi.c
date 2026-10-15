/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from Project Ubertooth, firmware/bluetooth_rxtx/ubertooth_cs.c and ubertooth_rssi.c
 * (Copyright 2015 Hannes Ellinger). See ubt_rssi.h for what changed.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ubt_rssi.h"

#include "ubt_proto.h"

/* IIR weight of a new block: 3/256 as upstream's RSSI_IIR_ALPHA. */
#define RSSI_IIR_ALPHA 3
#define IIR_NONE       INT16_MIN

static int k_half_db = UBT_RSSI_K_DEFAULT;

void ubt_rssi_set_cal(int k)
{
	k_half_db = k;
}

int ubt_rssi_get_cal(void)
{
	return k_half_db;
}

int ubt_rssi_dbm_x2(uint8_t pwr)
{
	return (int)pwr - 200 + k_half_db;
}

static int8_t sat8(int v)
{
	return (int8_t)(v > INT8_MAX ? INT8_MAX : (v < INT8_MIN ? INT8_MIN : v));
}

int8_t ubt_rssi_raw(uint8_t pwr)
{
	/* Rounded to the nearest dB (floor of x + 1/2). */
	return sat8(((ubt_rssi_dbm_x2(pwr) + 1) >> 1) + UBT_RSSI_OFFSET);
}

int8_t ubt_rssi_br_to_dbm(int8_t rssi)
{
	/* models the cc2400 datasheet fig 22 for 1M as piece-wise linear */
	if (rssi < -48) {
		return -120;
	} else if (rssi <= -45) {
		return (int8_t)(6 * (rssi + 28));
	} else if (rssi <= 30) {
		return (int8_t)((99 * ((int)rssi - 62)) / 110);
	} else if (rssi <= 35) {
		return (int8_t)((60 * ((int)rssi - 35)) / 11);
	}
	return 0;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

int8_t ubt_rssi_br_raw_from_dbm(int dbm)
{
	/* The inverse of each segment of the host's map, then the best of the neighbours. */
	const int c[3] = {
		clampi(-28 + (dbm - (dbm < 0 ? 3 : -3)) / 6, -48, -45),
		clampi(62 + (dbm * 110 + (dbm < 0 ? -49 : 49)) / 99, -44, 30),
		clampi(35 + (dbm * 11 + (dbm < 0 ? -30 : 30)) / 60, 31, 35),
	};
	int best = c[1], best_err = 1000;

	if (dbm <= -120) {
		return -128;
	}
	if (dbm >= 0) {
		return 36;
	}
	for (int s = 0; s < 3; s++) {
		for (int r = c[s] - 1; r <= c[s] + 1; r++) {
			int err = ubt_rssi_br_to_dbm((int8_t)r) - dbm;

			err = err < 0 ? -err : err;
			if (err < best_err) {
				best_err = err;
				best = r;
			}
		}
	}
	return (int8_t)best;
}

int8_t ubt_rssi_br_raw(uint8_t pwr)
{
	return ubt_rssi_br_raw_from_dbm((ubt_rssi_dbm_x2(pwr) + 1) >> 1);
}

void ubt_rssi_blk_reset(struct ubt_rssi_blk *b)
{
	b->n = 0;
	b->sum = 0;
	b->max = INT8_MIN;
	b->min = INT8_MAX;
}

void ubt_rssi_blk_add(struct ubt_rssi_blk *b, int8_t v)
{
	b->max = v > b->max ? v : b->max;
	b->min = v < b->min ? v : b->min;
	b->sum += (int32_t)v * 256; /* scaled int math (x256) */
	if (b->n < UINT16_MAX) {
		b->n++;
	}
}

uint8_t ubt_rssi_blk_count(const struct ubt_rssi_blk *b)
{
	return (uint8_t)(b->n > 255U ? 255U : b->n);
}

int32_t ubt_rssi_blk_avg_x256(const struct ubt_rssi_blk *b)
{
	int32_t half = (int32_t)(b->n / 2U);

	if (b->n == 0U) {
		return 0;
	}
	return (b->sum + (b->sum < 0 ? -half : half)) / (int32_t)b->n;
}

void ubt_rssi_init(struct ubt_rssi *r)
{
	r->cs_no_squelch = false;
	r->cs_req = UBT_SQUELCH_MIN;
	r->cs_cur = UBT_SQUELCH_MIN;
	for (unsigned int i = 0; i < UBT_RSSI_CHANNELS; i++) {
		r->iir[i] = IIR_NONE;
	}
}

/* Index of a channel in the average table: slot 0 for anything outside the BR band. */
static unsigned int chan_slot(uint16_t mhz)
{
	if (mhz < 2402U || mhz > 2480U) {
		return 0;
	}
	return (unsigned int)(mhz - 2402U);
}

void ubt_rssi_set_squelch(struct ubt_rssi *r, int8_t req)
{
	r->cs_req = req;
}

int8_t ubt_rssi_get_squelch(const struct ubt_rssi *r)
{
	return r->cs_req;
}

/*
 * cs_threshold_set(): the CC2400's carrier sense threshold is in 4 dB steps, written as
 * ((level + 56) & 0xfc); the level in force is what that register value stands for.
 */
void ubt_rssi_squelch_update(struct ubt_rssi *r, uint16_t channel_mhz)
{
	int level;

	if (r->cs_req > 0) {
		/* Relative: this many dB over the channel's average. */
		level = ubt_rssi_iir_get(r, channel_mhz) - UBT_RSSI_OFFSET + r->cs_req;
	} else {
		level = r->cs_req;
	}
	level = level < UBT_SQUELCH_MIN ? UBT_SQUELCH_MIN : level;
	level = level > UBT_SQUELCH_MAX ? UBT_SQUELCH_MAX : level;
	r->cs_no_squelch = level <= UBT_SQUELCH_MIN;
	r->cs_cur = (int8_t)((int8_t)((level + 56) & 0xfc) - 56);
}

uint8_t ubt_rssi_squelch_pwr(const struct ubt_rssi *r)
{
	int p;

	if (r->cs_no_squelch) {
		return 0;
	}
	/* pwr = dBm_x2 + 200 - k, at least 1 (0 means no squelch to the demodulator). */
	p = 2 * r->cs_cur + 200 - k_half_db;
	return (uint8_t)(p < 1 ? 1 : (p > 255 ? 255 : p));
}

int ubt_rssi_squelch_raw(const struct ubt_rssi *r)
{
	return r->cs_cur + UBT_RSSI_OFFSET;
}

void ubt_rssi_iir_update(struct ubt_rssi *r, uint16_t channel_mhz, const struct ubt_rssi_blk *b)
{
	unsigned int i = chan_slot(channel_mhz);
	int32_t avg, acc;

	if (b->n == 0U) {
		return; /* upstream averaged 0 in: an error, not a measurement */
	}
	avg = ubt_rssi_blk_avg_x256(b);
	if (r->iir[i] == IIR_NONE) {
		r->iir[i] = (int16_t)avg;
		return;
	}
	/* IIR using scaled int math (x256) */
	acc = (int32_t)r->iir[i] * (256 - RSSI_IIR_ALPHA) + avg * RSSI_IIR_ALPHA;
	r->iir[i] = (int16_t)((acc + 128) >> 8);
}

int8_t ubt_rssi_iir_get(const struct ubt_rssi *r, uint16_t channel_mhz)
{
	int16_t v = r->iir[chan_slot(channel_mhz)];

	if (v == IIR_NONE) {
		return UBT_RSSI_NOISE_RAW;
	}
	return (int8_t)((v + 128) >> 8);
}
