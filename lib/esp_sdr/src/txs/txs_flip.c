/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, flip scheduler: see txs_flip.h.
 */

#include <string.h>

#include <esp_sdr/esp_sdr_place.h>

#include "txs_flip.h"

int txs_flip_init(struct txs_flip *f, const struct txs_flip_cfg *cfg, uint64_t first)
{
	if ((first & (TXS_GROUP_WORDS - 1U)) != 0U || cfg->cost <= cfg->zcost || cfg->chunk == 0U ||
	    cfg->write_lat > cfg->lock_lead ||
	    cfg->margin < cfg->lock_lead + cfg->chunk * cfg->zcost + cfg->overhead) {
		return -1;
	}
	memset(f, 0, sizeof(*f));
	f->cfg = *cfg;
	f->mode = TXS_RUN;
	f->reason = TXS_STOP_NONE;
	atomic_init(&f->req, 0);
	f->next = first;
	f->dac_group = txs_group_of(first - TXS_GROUP_WORDS);
	return 0;
}

/* Cycles left to the point of no return, negative when it has passed. */
ESP_SDR_CODE(PACED_CHUNK, cycles_to_pnr)
static int64_t cycles_to_pnr(const struct txs_flip *f, const struct txs_pos *p, uint64_t fill)
{
	uint64_t rest = txs_flip_hi(f) - fill;
	int64_t zc;

	/* a window's rest and a cost per word are small: one 32 bit multiply, no 64 bit one */
	if (rest <= 0xffffU && f->cfg.zcost <= 0xffffU) {
		zc = (int64_t)((uint32_t)rest * f->cfg.zcost);
	} else {
		zc = (int64_t)(rest * f->cfg.zcost);
	}
	return txs_pos_cycles_to(p, f->next) - zc - (int64_t)f->cfg.margin;
}

ESP_SDR_CODE(PACED_CHUNK, txs_flip_poll)
enum txs_op txs_flip_poll(struct txs_flip *f, const struct txs_pos *p, uint64_t fill)
{
	bool final;
	int64_t to_due;
	/* a load first: the exchange (an atomic operation) only when another core asked */
	if (atomic_load_explicit(&f->req, memory_order_relaxed) != 0U) {
		uint32_t req = atomic_exchange_explicit(&f->req, 0, memory_order_acquire);

		if (req != 0U) {
			txs_flip_stop(f, (enum txs_stop)req);
		}
	}
	final = f->mode == TXS_DRAIN && f->drain_left == 0U;
	if (f->mode == TXS_OFF) {
		return TXS_OP_OFF;
	}
	to_due = txs_pos_cycles_to(p, f->next);
	if (to_due <= (int64_t)f->cfg.lock_lead) {
		if (final) {
			f->mode = TXS_OFF;
			f->reason = TXS_STOP_DRAINED;
			return TXS_OP_OFF;
		}
		return TXS_OP_FLIP;
	}
	if (final || fill >= txs_flip_hi(f)) {
		return TXS_OP_IDLE;
	}
	if (f->mode == TXS_DRAIN || cycles_to_pnr(f, p, fill) <= 0) {
		return TXS_OP_ZERO;
	}
	return TXS_OP_FILL;
}

ESP_SDR_CODE(PACED_CHUNK, may_write_wide)
static bool may_write_wide(const struct txs_flip *f, const struct txs_pos *p, uint64_t fill,
			uint64_t lo, uint64_t n)
{
	uint64_t hi = txs_flip_hi(f), gap = 0, above, below = 0, from, cur_after;

	if (n <= lo || n <= f->next || lo >= hi) {
		return true;
	}
	if (f->mode != TXS_RUN || lo < f->next || n > hi) {
		return false;
	}
	if (lo < fill) {
		below = (n < fill ? n : fill) - lo;
	}
	from = lo > fill ? lo : fill;
	gap = lo > fill ? lo - fill : 0;
	above = n > from ? n - from : 0;
	cur_after = n > fill ? n : fill;
	return txs_pos_cycles_to(p, f->next) >
	       (int64_t)((below + above) * f->cfg.cost + (gap + (hi - cur_after)) * f->cfg.zcost +
			 f->cfg.margin);
}

ESP_SDR_CODE(PACED_CHUNK, txs_flip_may_write)
bool txs_flip_may_write(const struct txs_flip *f, const struct txs_pos *p, uint64_t fill,
			uint64_t lo, uint64_t n)
{
	uint64_t hi = txs_flip_hi(f);
	uint32_t l, nn, fl, below, from, gap, above, cur_after, work;

	if (n <= lo || n <= f->next || lo >= hi) {
		return true;
	}
	if (f->mode != TXS_RUN || lo < f->next || n > hi) {
		return false;
	}
	if (fill < f->next || fill > hi || f->cfg.cost > 0x7fffU || f->cfg.zcost > 0x7fffU) {
		return may_write_wide(f, p, fill, lo, n);
	}
	/* Everything lies in the window: offsets from its start are 32 bit, so are the products
	 * (at most 32768 words x 32767 cycles, twice).
	 */
	l = (uint32_t)(lo - f->next);
	nn = (uint32_t)(n - f->next);
	fl = (uint32_t)(fill - f->next);
	below = l < fl ? (nn < fl ? nn : fl) - l : 0U;
	from = l > fl ? l : fl;
	gap = l > fl ? l - fl : 0U;
	above = nn > from ? nn - from : 0U;
	cur_after = nn > fl ? nn : fl;
	work = (below + above) * f->cfg.cost + (gap + TXS_GROUP_WORDS - cur_after) * f->cfg.zcost;
	return txs_pos_cycles_to(p, f->next) > (int64_t)work + (int64_t)f->cfg.margin;
}

ESP_SDR_CODE(PACED_CHUNK, txs_flip_fit)
uint64_t txs_flip_fit(const struct txs_flip *f, const struct txs_pos *p, uint64_t fill,
		      uint64_t lo, uint64_t end)
{
	uint64_t hi = txs_flip_hi(f), a = lo > f->next ? lo : f->next, best, top;

	if (end <= a || a >= hi) {
		return end;
	}
	best = a;
	top = end < hi ? end : hi;
	/* usually the whole range fits: one test instead of a search */
	if (txs_flip_may_write(f, p, fill, a, top)) {
		return top;
	}
	while (best < top) {
		uint64_t mid = best + (top - best + 1U) / 2U;

		if (txs_flip_may_write(f, p, fill, a, mid)) {
			best = mid;
		} else {
			top = mid - 1U;
		}
	}
	return best;
}

ESP_SDR_CODE(PACED_CHUNK, txs_flip_aim)
uint32_t txs_flip_aim(const struct txs_flip *f, const struct txs_pos *p)
{
	return txs_pos_counter_at(p, f->next) - f->cfg.write_lat;
}

ESP_SDR_CODE(PACED_CHUNK, txs_flip_done)
uint32_t txs_flip_done(struct txs_flip *f, uint64_t play, uint64_t fill, uint32_t data)
{
	int64_t d = (int64_t)(play - f->next);
	uint32_t groups = 1;
	uint64_t hi = txs_flip_hi(f);

	f->flips++;
	if (d > 0) {
		if ((uint64_t)d > f->cfg.tol) {
			f->late_flips++;
		} else {
			f->slack_flips++;
		}
		if ((uint64_t)d > f->late_worst) {
			f->late_worst = (uint64_t)d;
		}
	} else if (d < 0) {
		f->early_flips++;
		if ((uint64_t)-d > f->early_worst) {
			f->early_worst = (uint64_t)-d;
		}
	}
	if (d >= (int64_t)TXS_GROUP_WORDS) {
		/* Whole groups went by: the old window was never played from its own group and the
		 * window playing now was never refilled.
		 */
		groups = (uint32_t)(((uint64_t)d >> TXS_GROUP_SHIFT) + 1U);
		f->missed_flips += groups - 1U;
		f->stale_words += (uint64_t)groups << TXS_GROUP_SHIFT;
	} else if (d >= 0) {
		/* the late words, then whatever of the window was not written */
		uint64_t from = fill > f->next + (uint64_t)d ? fill : f->next + (uint64_t)d;

		f->stale_words += (uint64_t)d + (from < hi ? hi - from : 0U);
	} else {
		f->stale_words += (uint64_t)-d + (fill < hi ? hi - fill : 0U);
	}
	f->dac_group = txs_group_of(play > f->next ? play : f->next);
	f->next += (uint64_t)groups << TXS_GROUP_SHIFT;

	if (data == 0U) {
		f->zero_passes++;
	} else {
		f->zero_passes = 0;
	}
	if (f->mode == TXS_DRAIN) {
		if (groups > 1U) {
			f->mode = TXS_OFF;
			f->reason = TXS_STOP_MISSED;
		} else if (f->drain_left != 0U) {
			f->drain_left--;
		}
	} else if (f->mode == TXS_RUN && f->cfg.hang_passes != 0U &&
		   f->zero_passes >= f->cfg.hang_passes) {
		f->mode = TXS_OFF;
		f->reason = TXS_STOP_HANG;
	}
	return groups;
}

ESP_SDR_CODE(PACED_CHUNK, txs_flip_stop)
void txs_flip_stop(struct txs_flip *f, enum txs_stop how)
{
	if (f->mode == TXS_OFF) {
		return;
	}
	if (how == TXS_STOP_NOW) {
		f->reason = TXS_STOP_NOW;
		f->mode = TXS_OFF;
	} else if (how == TXS_STOP_DRAIN && f->mode == TXS_RUN) {
		/* the countdown first: nothing may see DRAIN with a stale count */
		f->drain_left = 2;
		f->reason = TXS_STOP_DRAIN;
		f->mode = TXS_DRAIN;
	}
}

void txs_flip_request(struct txs_flip *f, enum txs_stop how)
{
	uint32_t old = atomic_load_explicit(&f->req, memory_order_relaxed);

	/* the stronger request stays: NONE < DRAIN < NOW */
	while (old < (uint32_t)how &&
	       !atomic_compare_exchange_weak_explicit(&f->req, &old, (uint32_t)how,
						      memory_order_release, memory_order_relaxed)) {
	}
}
