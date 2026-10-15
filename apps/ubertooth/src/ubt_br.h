/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The RX_SYMBOLS block builder: packs the demodulator's symbols into the 64 byte BR_PACKETs the
 * host searches for access codes (upstream bt_stream_rx(), enqueue() and the DMA handlers of
 * firmware/bluetooth_rxtx/bluetooth_rxtx.c). Every block is 400 consecutive symbols, 50 bytes,
 * first symbol in bit 7 of data[0]; the host (cb_scan, cb_rx, ubertooth-dump -b) looks for
 * access codes inside one block and never relies on the one before.
 *
 * Squelched stretches. Upstream streams every block, also when nothing is received (the CC2400
 * keeps demodulating noise; CS_TRIGGER only flags the blocks with carrier). The demodulator
 * emits nothing while its squelch is closed, so the builder fills the time between symbols with
 * idle symbols (zero bits at the level the demodulator reports), keeping the block cadence of
 * 400 us and the symbol slots contiguous; the host sees blocks of zeros where the CC2400 gave
 * noise, which no access code search matches. With ubt_br_set_drop() the idle blocks beyond
 * the first few are not sent at all, which costs the USB link nothing and the host loses no
 * information (it only ever looks at one block at a time; the queue's KEEP_ALIVE keeps the
 * connection up). The glue calls ubt_br_advance() after each push of samples with
 * gfsk_demod_horizon() so idle time is turned into blocks even when no symbol arrives.
 *
 * Time stamps. Upstream stamps a block at the DMA terminal count, when the last of its 400
 * symbols has been received, and the host takes block start = clk100ns - 4000 (cb_rx: clk_offset
 * = (clk100ns + 10 * offset + 6250 - 4000) % 6250, cb_scan: clk100ns + 10 * offset): the stamp
 * is the instant the last symbol ENDS. The stamp callback therefore gets the sample index of
 * the end of the last symbol, the decision instant of that symbol (gfsk_sym.t, which is the
 * sample at which the symbol's centre was on the air) plus 2 samples.
 */

#ifndef UBT_BR_H_
#define UBT_BR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfsk_sym.h"
#include "ubt_proto.h"
#include "ubt_rssi.h"

/* Symbols per block: DMA_SIZE bytes, 8 symbols each. */
#define UBT_BR_BLOCK_SYMS (DMA_SIZE * 8U)
/* Samples (4 MS/s) per symbol slot. */
#define UBT_BR_SLOT GFSK_SAMPLES_PER_SYM

/** A finished block (the glue puts it in the USB queue; a full queue sets FIFO_OVERFLOW there). */
typedef void (*ubt_br_emit_fn)(void *ctx, const usb_pkt_rx *pkt);
/** Stream sample index (end of the block's last symbol) to clkn_high and clk100ns. */
typedef void (*ubt_br_stamp_fn)(void *ctx, uint32_t t_end, uint8_t *clkn_high, uint32_t *clk100ns);

struct ubt_br {
	ubt_br_emit_fn emit;
	ubt_br_stamp_fn stamp;
	void *ctx;
	struct ubt_rssi *rssi;

	uint16_t channel_mhz;
	uint16_t drop_after;
	/* Status bits for the next block: DMA_OVERFLOW, DISCARD. */
	uint8_t status;
	bool cs_latch;
	bool started;
	/* The block under construction. */
	uint16_t nsym;
	uint16_t nreal; /* of them from the demodulator */
	uint32_t t_next; /* decision instant of the next slot */
	uint8_t idle_pwr;
	uint8_t pw_max, pw_min;
	uint16_t pw_n;
	uint32_t pw_sum;
	uint16_t idle_run; /* blocks without a real symbol in a row */
	usb_pkt_rx pkt;

	uint32_t blocks, idle_blocks, dropped_blocks;
};

/**
 * @param stamp May be NULL: the 4 MS/s sample index then maps to 2.5 clk100ns per sample from 0.
 * @param rssi Squelch (CS_TRIGGER, RSSI_TRIGGER) and the per channel average of rssi_avg.
 */
void ubt_br_init(struct ubt_br *b, ubt_br_emit_fn emit, ubt_br_stamp_fn stamp, void *ctx,
		 struct ubt_rssi *rssi);

/**
 * The radio was retuned or hopped (the demodulator restarts its sample counter at 0): drop the
 * partial block and flag the first block that follows with DISCARD, as upstream after hop().
 */
void ubt_br_retune(struct ubt_br *b, uint16_t channel_mhz);

/** Idle blocks sent in a row before further ones are dropped; 0 sends all (upstream behaviour). */
void ubt_br_set_drop(struct ubt_br *b, uint16_t after_idle_blocks);

/** The input lost samples (the ring reported a gap): the next block gets DMA_OVERFLOW. */
void ubt_br_gap(struct ubt_br *b);

/** Symbols from the demodulator, in order. */
void ubt_br_push(struct ubt_br *b, const struct gfsk_sym *s, size_t n);

/**
 * Nothing will be emitted with a decision instant before @p horizon (gfsk_demod_horizon()):
 * fill the idle time up to it, at the power level @p idle_pwr (gfsk_demod_level()).
 */
void ubt_br_advance(struct ubt_br *b, uint32_t horizon, uint8_t idle_pwr);

#endif /* UBT_BR_H_ */
