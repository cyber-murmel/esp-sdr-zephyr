/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RX_SYMBOLS block builder, see ubt_br.h.
 */

#include <string.h>

#include "ubt_br.h"

/* Samples from a symbol's decision instant to its end. */
#define SYM_HALF (GFSK_SAMPLES_PER_SYM / 2U)

static void default_stamp(void *ctx, uint32_t t_end, uint8_t *clkn_high, uint32_t *clk100ns)
{
	/* 4 MS/s samples are 2.5 units of 100 ns. */
	uint64_t t100 = (uint64_t)t_end * 5U / 2U;
	uint64_t ticks = t100 / UBT_CLK100NS_PER_CLKN;

	(void)ctx;
	*clkn_high = (uint8_t)((ticks >> 20) & 0xffU);
	*clk100ns = (uint32_t)(t100 % UBT_CLK100NS_WRAP);
}

static void block_reset(struct ubt_br *b)
{
	memset(b->pkt.data, 0, sizeof(b->pkt.data));
	b->nsym = 0;
	b->nreal = 0;
	b->pw_max = 0;
	b->pw_min = 255;
	b->pw_n = 0;
	b->pw_sum = 0;
}

void ubt_br_init(struct ubt_br *b, ubt_br_emit_fn emit, ubt_br_stamp_fn stamp, void *ctx,
		 struct ubt_rssi *rssi)
{
	memset(b, 0, sizeof(*b));
	b->emit = emit;
	b->stamp = stamp != NULL ? stamp : default_stamp;
	b->ctx = ctx;
	b->rssi = rssi;
	b->channel_mhz = 2441;
	block_reset(b);
}

void ubt_br_set_drop(struct ubt_br *b, uint16_t after_idle_blocks)
{
	b->drop_after = after_idle_blocks;
}

void ubt_br_gap(struct ubt_br *b)
{
	b->status |= DMA_OVERFLOW;
}

void ubt_br_retune(struct ubt_br *b, uint16_t channel_mhz)
{
	b->channel_mhz = channel_mhz;
	b->started = false;
	b->cs_latch = false;
	b->idle_run = 0;
	b->status |= DISCARD;
	block_reset(b);
	if (b->rssi != NULL) {
		ubt_rssi_squelch_update(b->rssi, channel_mhz);
	}
}

/* The block is full, its last symbol's decision instant is @p t_last. */
static void block_finish(struct ubt_br *b, uint32_t t_last)
{
	struct ubt_rssi *r = b->rssi;
	usb_pkt_rx *p = &b->pkt;
	bool idle = b->nreal == 0U;
	uint8_t status = b->status;
	int avg_pwr = b->pw_n != 0U ? (int)((b->pw_sum + b->pw_n / 2U) / b->pw_n) : 0;

	b->status = 0;
	b->blocks++;
	/* The channel's average sees every block, idle ones included: that is the noise floor. */
	if (r != NULL && b->pw_n != 0U) {
		struct ubt_rssi_blk blk;
		int32_t per = (ubt_rssi_get_cal() - 200) * 128 + UBT_RSSI_OFFSET * 256;

		blk.max = ubt_rssi_raw(b->pw_max);
		blk.min = ubt_rssi_raw(b->pw_min);
		blk.n = b->pw_n;
		blk.sum = (int32_t)b->pw_sum * 128 + (int32_t)b->pw_n * per;
		ubt_rssi_iir_update(r, b->channel_mhz, &blk);
	}
	if (idle) {
		b->idle_blocks++;
		if (b->idle_run < UINT16_MAX) {
			b->idle_run++;
		}
		if (b->drop_after != 0U && b->idle_run > b->drop_after) {
			b->dropped_blocks++;
			b->status |= status; /* the flags go on the next block sent */
			return;
		}
	} else {
		b->idle_run = 0;
	}
	p->pkt_type = BR_PACKET;
	if (r != NULL) {
		if (b->cs_latch || r->cs_no_squelch) {
			status |= CS_TRIGGER;
		}
		if (ubt_rssi_dbm_x2(b->pw_max) >= 2 * r->cs_cur) {
			status |= RSSI_TRIGGER;
		}
	}
	b->cs_latch = false;
	p->status = status;
	p->channel = (uint8_t)((b->channel_mhz - 2402U) & 0xffU);
	b->stamp(b->ctx, t_last + SYM_HALF, &p->clkn_high, &p->clk100ns);
	p->rssi_max = ubt_rssi_br_raw(b->pw_max);
	p->rssi_min = ubt_rssi_br_raw(b->pw_min);
	p->rssi_avg = r != NULL ? ubt_rssi_br_raw_from_dbm(ubt_rssi_iir_get(r, b->channel_mhz) -
							 UBT_RSSI_OFFSET)
				: ubt_rssi_br_raw((uint8_t)avg_pwr);
	p->rssi_count = (uint8_t)(b->pw_n > 255U ? 255U : b->pw_n);
	p->reserved[0] = 0;
	p->reserved[1] = 0;
	b->emit(b->ctx, p);
}

/* One slot: a symbol (real or idle) at decision instant t. */
static inline void place(struct ubt_br *b, uint32_t t, unsigned int bit, uint8_t pwr, bool real)
{
	if (bit != 0U) {
		b->pkt.data[b->nsym >> 3] |= (uint8_t)(0x80U >> (b->nsym & 7U));
	}
	b->pw_max = pwr > b->pw_max ? pwr : b->pw_max;
	b->pw_min = pwr < b->pw_min ? pwr : b->pw_min;
	b->pw_sum += pwr;
	b->pw_n++;
	b->nreal += real ? 1U : 0U;
	if (++b->nsym == UBT_BR_BLOCK_SYMS) {
		block_finish(b, t);
		block_reset(b);
	}
}

/* @p n idle slots from the next slot on. */
static void fill_idle(struct ubt_br *b, uint32_t n)
{
	while (n != 0U) {
		if (b->nsym == 0U && n >= UBT_BR_BLOCK_SYMS) {
			/* Whole blocks of idle: no per symbol work. */
			uint32_t last = b->t_next + UBT_BR_SLOT * (UBT_BR_BLOCK_SYMS - 1U);

			b->nsym = UBT_BR_BLOCK_SYMS - 1U;
			b->pw_n = UBT_BR_BLOCK_SYMS - 1U;
			b->pw_sum = (UBT_BR_BLOCK_SYMS - 1U) * b->idle_pwr;
			b->pw_max = b->pw_min = b->idle_pwr;
			place(b, last, 0, b->idle_pwr, false);
			b->t_next += UBT_BR_SLOT * UBT_BR_BLOCK_SYMS;
			n -= UBT_BR_BLOCK_SYMS;
			continue;
		}
		place(b, b->t_next, 0, b->idle_pwr, false);
		b->t_next += UBT_BR_SLOT;
		n--;
	}
}

void ubt_br_advance(struct ubt_br *b, uint32_t horizon, uint8_t idle_pwr)
{
	int32_t dt;

	b->idle_pwr = idle_pwr;
	if (!b->started) {
		/* Squelch closed from the start: the idle blocks begin here. */
		b->started = true;
		b->t_next = horizon;
		return;
	}
	dt = (int32_t)(horizon - b->t_next);
	if (dt >= (int32_t)UBT_BR_SLOT) {
		fill_idle(b, (uint32_t)dt / UBT_BR_SLOT);
	}
}

void ubt_br_push(struct ubt_br *b, const struct gfsk_sym *s, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		int32_t dt;

		if (!b->started) {
			b->started = true;
			b->cs_latch = true;
			b->t_next = s[k].t;
		}
		dt = (int32_t)(s[k].t - b->t_next);
		if (dt > (int32_t)SYM_HALF) {
			/* Idle slots before it: a squelched stretch that ended. */
			fill_idle(b, ((uint32_t)dt + SYM_HALF) / UBT_BR_SLOT);
			b->cs_latch = true;
		}
		place(b, s[k].t, s[k].bit, s[k].pwr, true);
		b->t_next = s[k].t + UBT_BR_SLOT;
	}
}
