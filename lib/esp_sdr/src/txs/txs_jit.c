/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, jitter cursor and loss handling: see txs_jit.h.
 */

#include <string.h>

#include <esp_sdr/esp_sdr_place.h>

#include "txs_jit.h"

int txs_jit_init(struct txs_jit *j, const struct txs_flip *f, uint32_t ahead, uint32_t depth)
{
	if (ahead < depth) {
		return -1;
	}
	memset(j, 0, sizeof(*j));
	j->ahead = ahead;
	txs_jit_flip(j, f);
	return 0;
}

ESP_SDR_CODE(PACED_CHUNK, txs_jit_flip)
void txs_jit_flip(struct txs_jit *j, const struct txs_flip *f)
{
	if (!j->closed && j->started && j->cursor < j->hi) {
		j->underruns++;
		j->unfilled += j->hi - j->cursor;
	}
	j->lo = f->next;
	j->hi = txs_flip_hi(f);
	if (j->cursor < j->lo) {
		j->cursor = j->lo;
	}
	j->closed = false;
	j->win_data = 0;
	j->nholes = 0;
}

ESP_SDR_CODE(PACED_CHUNK, hole_drop)
static void hole_drop(struct txs_jit *j, uint32_t i)
{
	memmove(&j->holes[i], &j->holes[i + 1U], (j->nholes - i - 1U) * sizeof(j->holes[0]));
	j->nholes--;
}

ESP_SDR_CODE(PACED_CHUNK, hole_add)
static void hole_add(struct txs_jit *j, uint64_t lo, uint64_t hi)
{
	if (j->nholes == TXS_HOLES) {
		hole_drop(j, 0);
	}
	j->holes[j->nholes].lo = lo;
	j->holes[j->nholes].hi = hi;
	j->nholes++;
}

/* Take [lo, hi) out of the holes. @return the words that were in them. */
ESP_SDR_CODE(PACED_CHUNK, hole_carve)
static uint64_t hole_carve(struct txs_jit *j, uint64_t lo, uint64_t hi)
{
	uint64_t got = 0;

	for (uint32_t i = 0; i < j->nholes;) {
		struct txs_span h = j->holes[i];

		if (h.hi <= lo || h.lo >= hi) {
			i++;
			continue;
		}
		got += (h.hi < hi ? h.hi : hi) - (h.lo > lo ? h.lo : lo);
		hole_drop(j, i);
		if (h.lo < lo) {
			hole_add(j, h.lo, lo);
		}
		if (h.hi > hi) {
			hole_add(j, hi, h.hi);
		}
		/* the pieces lie outside [lo, hi): start over, the order moved */
		i = 0;
	}
	return got;
}

/* The hull of the holes within [lo, hi) and their words. */
ESP_SDR_CODE(PACED_CHUNK, hole_hull)
static uint64_t hole_hull(const struct txs_jit *j, uint64_t lo, uint64_t hi, struct txs_span *hull)
{
	uint64_t got = 0;

	hull->lo = hi;
	hull->hi = lo;
	for (uint32_t i = 0; i < j->nholes; i++) {
		uint64_t a = j->holes[i].lo > lo ? j->holes[i].lo : lo;
		uint64_t b = j->holes[i].hi < hi ? j->holes[i].hi : hi;

		if (a >= b) {
			continue;
		}
		got += b - a;
		hull->lo = a < hull->lo ? a : hull->lo;
		hull->hi = b > hull->hi ? b : hull->hi;
	}
	return got;
}

ESP_SDR_CODE(PACED_CHUNK, txs_jit_packet)
void txs_jit_packet(struct txs_jit *j, uint64_t idx, uint32_t n, struct txs_place *out)
{
	uint64_t end = idx + n, a, b, cur = j->cursor;

	memset(out, 0, sizeof(*out));
	if (n == 0U || end < idx) {
		out->verdict = TXS_PK_BAD;
		return;
	}
	if (end <= j->lo) {
		j->late += n;
		out->verdict = TXS_PK_LATE;
		out->used = n;
		return;
	}
	if (idx >= j->hi) {
		if (idx - j->hi >= j->ahead) {
			j->far += n;
			out->verdict = TXS_PK_FAR;
			out->used = n;
		} else {
			out->verdict = TXS_PK_WAIT;
		}
		return;
	}
	a = idx > j->lo ? idx : j->lo;
	b = end < j->hi ? end : j->hi;
	out->used = (uint32_t)(b - idx);
	j->late += a - idx;
	if (j->closed) {
		j->late += b - a;
		out->verdict = TXS_PK_LATE;
		return;
	}
	out->verdict = TXS_PK_PLACE;
	if (a >= cur) {
		if (a > cur) {
			out->zero.lo = cur;
			out->zero.hi = a;
			j->lost += a - cur;
			hole_add(j, cur, a);
		}
		out->put.lo = a;
		out->put.hi = b;
		j->cursor = b;
		j->placed += b - a;
		j->win_data += (uint32_t)(b - a);
		j->started = true;
		return;
	}
	/* Below the cursor: a hole to fill, or a duplicate. */
	{
		struct txs_span hull;
		uint64_t below = (b < cur ? b : cur), ovl = hole_hull(j, a, below, &hull);
		uint64_t fresh = b > cur ? b - cur : 0;

		if (ovl == 0U && fresh == 0U) {
			j->dup += b - a;
			out->verdict = TXS_PK_DUP;
			return;
		}
		out->put.lo = ovl != 0U ? hull.lo : cur;
		out->put.hi = fresh != 0U ? b : hull.hi;
		hole_carve(j, a, below);
		j->recovered += ovl;
		j->dup += (below - a) - ovl;
		j->placed += ovl + fresh;
		j->win_data += (uint32_t)(ovl + fresh);
		j->started = true;
		if (b > cur) {
			j->cursor = b;
		}
	}
}

ESP_SDR_CODE(PACED_CHUNK, txs_jit_close)
void txs_jit_close(struct txs_jit *j, bool drain, struct txs_span *zero)
{
	zero->lo = zero->hi = j->cursor;
	if (j->closed) {
		return;
	}
	if (j->cursor < j->hi) {
		zero->hi = j->hi;
		j->zeroed += j->hi - j->cursor;
		if (!drain && j->started) {
			j->underruns++;
		}
	}
	j->cursor = j->hi;
	j->closed = true;
}
