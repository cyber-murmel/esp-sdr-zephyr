/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The channel mode (doc/pcap.md, "Modes"): the C6's paced receive (ESDR_MODE_RX, one channel
 * without a gap at its rate, the LO on the channel, the DC taken out on the C6) cut into bursts
 * by a power detector: the power of blocks of NB_BLOCK samples against a floor learnt from the
 * quietest of the first blocks and following the quiet ones; a block above floor * ratio opens
 * a burst (with NB_PRE samples before it), NB_HANG quiet blocks close it. Run by the stream
 * thread (survey.c).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app.h"
#include "esdr_proto.h"

LOG_MODULE_REGISTER(narrow, LOG_LEVEL_INF);

/* Samples a detector block (8 us at 2 MS/s), the pre-roll, quiet blocks before closing. */
#define NB_BLOCK 16U
#define NB_PRE   64U
#define NB_HANG  6U
/*
 * Loud blocks a burst needs, or it goes back unseen: the receiver's noise and interference
 * make thousands of one-block blips a second (2026-10-27, channel 37), and nothing of
 * interest is that short (an empty Bluetooth LE PDU lasts 80 us, ten blocks).
 */
#define NB_MIN_LOUD 3U
/* Blocks the floor is first learnt from (its lower quartile), then its follow rate. */
#define NB_LEARN  32U
#define NB_FOLLOW 64.0f

static struct {
	uint64_t freq_hz;
	uint32_t rate_hz;
	float ratio, floor;
	/* The current block's power sum and samples; quiet blocks in a row while open. */
	float acc;
	uint32_t in_block, quiet, learnt;
	float learn[NB_LEARN];
	/* The last NB_PRE samples (the pre-roll), its write position and fill. */
	int16_t pre[2U * NB_PRE];
	uint32_t pre_at, pre_n;
	/* The next sample's index on the stream's axis; whether the axis is anchored. */
	uint64_t next;
	bool anchored, have_next;
	struct burst *b;
	bool skip, loud_set;
	uint8_t gain;
	/* Loud blocks of the burst being assembled. */
	uint32_t loud_blocks;
} nb;

int narrow_start(uint64_t freq_hz, uint32_t rate_hz)
{
	if (rate_hz == 0U || 1000000000U % rate_hz != 0U) {
		return -EINVAL;
	}
	memset(&nb, 0, sizeof(nb));
	nb.freq_hz = freq_hz;
	nb.rate_hz = rate_hz;
	nb.ratio = app_cfg.open_ratio > 1.0f ? app_cfg.open_ratio : 4.0f;
	nb.gain = app_cfg.rx_gain == ESDR_GAIN_AUTO ? ESDR_SEG_GAIN_AUTO : (uint8_t)app_cfg.rx_gain;
	LOG_INF("channel mode at %u kHz, %u S/s", (unsigned int)(freq_hz / 1000U), rate_hz);
	return 0;
}

static void close_burst(uint16_t flags)
{
	struct burst *b = nb.b;

	nb.b = NULL;
	if (b == NULL) {
		return;
	}
	b->flags |= flags;
	if (flags == 0U && nb.loud_blocks < NB_MIN_LOUD) {
		burst_put(b);
		return;
	}
	if (decoder_put(b) != 0) {
		app_stats.bursts_lost++;
		burst_put(b);
		return;
	}
	app_stats.bursts++;
}

void narrow_stop(void)
{
	close_burst(BURST_F_CUT);
}

/*
 * A new burst from the pre-roll on (it ends with the opening block; @p end is the index after
 * its last sample); none (counted) when no buffer is free.
 */
static void open_burst(uint64_t end)
{
	struct burst *b = burst_get();
	uint32_t n = nb.pre_n;

	if (b == NULL) {
		app_stats.bursts_lost++;
		nb.skip = true;
		return;
	}
	b->channels = 1;
	b->rate_hz = nb.rate_hz;
	b->freq_hz = nb.freq_hz;
	b->floor = nb.floor;
	b->gain = nb.gain;
	b->m0 = end - n;
	/* The pre-roll, oldest first. */
	for (uint32_t k = 0; k < n; k++) {
		uint32_t i = (nb.pre_at + NB_PRE - n + k) % NB_PRE;

		b->iq[2U * k] = nb.pre[2U * i];
		b->iq[2U * k + 1U] = nb.pre[2U * i + 1U];
	}
	b->n = n;
	nb.b = b;
	nb.quiet = 0;
	nb.loud_set = false;
	nb.loud_blocks = 0;
}

/* The end of a detector block (@p end: the index after its last sample): learn, open, follow,
 * close. */
static void block_done(uint64_t end)
{
	float pw = nb.acc / NB_BLOCK;
	bool loud;

	nb.acc = 0.0f;
	nb.in_block = 0;
	if (nb.learnt < NB_LEARN) {
		/* Kept sorted as it fills: the lower quartile is then one read. */
		uint32_t j = nb.learnt++;

		for (; j > 0U && nb.learn[j - 1U] > pw; j--) {
			nb.learn[j] = nb.learn[j - 1U];
		}
		nb.learn[j] = pw;
		if (nb.learnt == NB_LEARN) {
			nb.floor = MAX(nb.learn[NB_LEARN / 4U], 1.0f);
		}
		return;
	}
	loud = pw > nb.floor * nb.ratio;
	if (!loud) {
		nb.floor += (pw - nb.floor) / NB_FOLLOW;
	}
	if (nb.skip) {
		/* Cut short or without a buffer: nothing until a quiet block. */
		nb.skip = loud;
		return;
	}
	if (nb.b == NULL) {
		if (loud) {
			open_burst(end);
		}
		if (nb.b == NULL) {
			return;
		}
	}
	if (loud) {
		nb.quiet = 0;
		nb.loud_blocks++;
		if (!nb.loud_set) {
			nb.b->loud_from = nb.b->n >= NB_BLOCK ? nb.b->n - NB_BLOCK : 0U;
			nb.loud_set = true;
		}
		nb.b->loud_to = nb.b->n;
		nb.b->power = MAX(nb.b->power, pw);
	} else if (++nb.quiet >= NB_HANG) {
		close_burst(0U);
	}
}

/* One sample (the 16-bit path, and 8-bit samples outside whole detector blocks). */
static void sample(int16_t i, int16_t q, uint64_t at)
{
	nb.pre[2U * nb.pre_at] = i;
	nb.pre[2U * nb.pre_at + 1U] = q;
	nb.pre_at = (nb.pre_at + 1U) % NB_PRE;
	nb.pre_n = MIN(nb.pre_n + 1U, NB_PRE);
	if (nb.b != NULL) {
		struct burst *b = nb.b;

		b->iq[2U * b->n] = i;
		b->iq[2U * b->n + 1U] = q;
		if (++b->n >= BURST_MAX_SAMPLES) {
			close_burst(BURST_F_TRUNCATED);
			nb.skip = true;
		}
	}
	nb.acc += (float)i * i + (float)q * q;
	if (++nb.in_block == NB_BLOCK) {
		block_done(at + 1U);
	}
}

/*
 * A whole detector block of 8-bit items @p p (I, Q; full scale 128, the decoders' 16384) that
 * starts one: into the pre-roll and the open burst, its power summed in integers (16 samples of
 * up to 2 128^2 each), then the block's end. @p at is its first sample's index.
 */
static void block8(const uint8_t *p, uint64_t at)
{
	int16_t v[2U * NB_BLOCK];
	int32_t acc = 0;
	struct burst *b = nb.b;

	for (uint32_t k = 0; k < 2U * NB_BLOCK; k += 2U) {
		int32_t i = (int8_t)p[k], q = (int8_t)p[k + 1U];

		acc += i * i + q * q;
		v[k] = (int16_t)(i * 128);
		v[k + 1U] = (int16_t)(q * 128);
	}
	for (uint32_t k = 0; k < NB_BLOCK; k++) {
		uint32_t j = (nb.pre_at + k) % NB_PRE;

		nb.pre[2U * j] = v[2U * k];
		nb.pre[2U * j + 1U] = v[2U * k + 1U];
	}
	nb.pre_at = (nb.pre_at + NB_BLOCK) % NB_PRE;
	nb.pre_n = MIN(nb.pre_n + NB_BLOCK, NB_PRE);
	if (b != NULL) {
		uint32_t m = MIN(NB_BLOCK, BURST_MAX_SAMPLES - b->n);

		memcpy(&b->iq[2U * b->n], v, 4U * m);
		b->n += m;
		if (b->n >= BURST_MAX_SAMPLES) {
			close_burst(BURST_F_TRUNCATED);
			nb.skip = true;
		}
	}
	nb.acc = (float)acc * 16384.0f;
	block_done(at + NB_BLOCK);
}

void narrow_block(const uint8_t *blk)
{
	struct esdr_block_hdr h;
	const uint8_t *p = blk + sizeof(h);
	uint32_t n;

	memcpy(&h, blk, sizeof(h));
	h.flags = sys_le16_to_cpu(h.flags);
	if (sys_le32_to_cpu(h.magic) != ESDR_BLOCK_MAGIC ||
	    (h.flags & (ESDR_BLK_SEG | ESDR_BLK_CHAN | ESDR_BLK_TRIGGER | ESDR_BLK_BURST |
			ESDR_BLK_TEST | ESDR_BLK_4BIT)) != 0U) {
		app_stats.other_blocks++;
		return;
	}
	h.index = sys_le64_to_cpu(h.index);
	n = sys_le16_to_cpu(h.samples);
	if (!nb.anchored) {
		timebase_anchor(axis_ns(h.index, nb.rate_hz));
		nb.anchored = true;
	}
	/* A gap: what was open ends, the pre-roll and the detector's block start again. */
	if ((h.flags & ESDR_BLK_GAP) != 0U || (nb.have_next && h.index != nb.next)) {
		app_stats.channel_gaps++;
		close_burst(BURST_F_DROPPED);
		nb.pre_n = 0;
		nb.in_block = 0;
		nb.acc = 0.0f;
		nb.skip = false;
	}
	if ((h.flags & ESDR_BLK_16BIT) != 0U) {
		n = MIN(n, (ESDR_BLOCK_BYTES - sizeof(h)) / 4U);
		for (uint32_t k = 0; k < n; k++) {
			int16_t i = (int16_t)sys_get_le16(p + 4U * k);
			int16_t q = (int16_t)sys_get_le16(p + 4U * k + 2U);

			sample(i, q, h.index + k);
		}
	} else {
		/* 8-bit items: full scale 128, the decoders' 16384. A detector block begun in the
		 * radio's block before ends sample by sample, whole ones go at once. */
		uint32_t k = 0;

		n = MIN(n, (ESDR_BLOCK_BYTES - sizeof(h)) / 2U);
		for (; k < n && nb.in_block != 0U; k++) {
			sample((int16_t)((int8_t)p[2U * k] * 128), (int16_t)((int8_t)p[2U * k + 1U] * 128),
			       h.index + k);
		}
		for (; n - k >= NB_BLOCK; k += NB_BLOCK) {
			block8(p + 2U * k, h.index + k);
		}
		for (; k < n; k++) {
			sample((int16_t)((int8_t)p[2U * k] * 128), (int16_t)((int8_t)p[2U * k + 1U] * 128),
			       h.index + k);
		}
	}
	app_stats.channel_samples += n;
	nb.next = h.index + n;
	nb.have_next = true;
}
