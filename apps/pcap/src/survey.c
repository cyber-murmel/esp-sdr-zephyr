/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The stream thread and the survey (doc/pcap.md, "Survey"). The radio's blocks come here: in
 * the survey the C6's gapless segment blocks go through the fixed-point channelizer and burst
 * detector (../../common/include/esdr_pfbq.h, PIE on the P4: this is the one thread that uses
 * it, pinned to its core), each channel's opening is assembled into a burst (burst.h) until it closes, a signal
 * that opens BURST_WIDE_MIN or more adjacent channels at once is kept as one wideband burst
 * without samples, and the C6's width records are joined into wideband runs; in the channel
 * mode the blocks go to narrow.c. Closed bursts go to the decoder.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app.h"
#include "esdr_pfbq.h"
#include "esdr_seg.h"
#include "radio.h"

LOG_MODULE_REGISTER(survey, LOG_LEVEL_INF);

#define STREAM_STACK 4096
#define STREAM_PRIO  5
/* Working memory of the filterbank and the detector: every supported M fits (M 80 needs some
 * 42 KB, M 20 some 11 KB).
 */
#define CHAN_MEM     (64U * 1024U)
/* Output scale: a full-scale carrier (512 raw units) in its channel gives 16384. */
#define CHAN_GAIN    32.0f
/* No block for this long: the stream stopped, its last quad and block go out. */
#define IDLE_MS      20
/* Blocks waiting for the thread (radio pool slots). */
#define BLOCK_Q      12
/* Frames one take may hand over (a detector event holds two blocks of 16 frames at most). */
#define TAKE_MAX     64U
/* Width records closer than this (pairs at 80 MS/s) join one run. */
#define WIDE_JOIN    1600U
/*
 * A hole in the C6's stream this long (pairs at 80 MS/s, half as many at 40: two 10 us
 * windows) ends every open burst there; a shorter one is zero filled (a burst's samples keep
 * their time). Without that, a channel the detector keeps open over the hole would fill its
 * burst with zeros.
 */
#define HOLE_PAIRS   1600U

K_MSGQ_DEFINE(block_q, sizeof(uint8_t *), BLOCK_Q, 4);
static K_THREAD_STACK_DEFINE(stream_stack, STREAM_STACK);
static struct k_thread stream_thread;
static uint8_t chan_mem[CHAN_MEM] __aligned(16);

enum cmd {
	CMD_NONE,
	CMD_SURVEY,
	CMD_NARROW,
	CMD_STOP,
};

/* A channel: the burst it assembles, the frame after its last sample, why it waits. */
struct chan_st {
	struct burst *b;
	uint64_t next;
	/* Cut (no buffer, or full): nothing more until it closes. In a wideband run. */
	bool skip, wide;
	bool loud_set;
	/* Its burst ended at a hole while the detector kept it open: the next samples start one. */
	bool resume;
};

static struct {
	/* The command for the thread, its argument, the thread's answer. */
	atomic_t cmd;
	uint64_t arg_hz;
	uint32_t arg_rate;
	int result;
	struct k_sem done;
	/* What runs: 0, CMD_SURVEY or CMD_NARROW. */
	int mode;

	struct esdr_pfbq pfb;
	struct esdr_detq det;
	struct esdr_seg_track track;
	uint16_t m;
	/* The C6's rate: 80 MS/s over decim (1, or 2 at 40 MS/s). */
	uint32_t decim;
	uint32_t rate_hz, spacing_hz;
	/* The channels kept: the host's mask (app_cfg.mask) on this bank's channel numbers. */
	uint32_t mask[3];
	uint64_t centre_hz;
	float ratio;
	bool anchored;
	uint8_t gain;
	uint32_t decided;
	struct chan_st ch[ESDR_PFB_M_MAX];
	/* The wideband run in progress: its span in frequency order, first and last frame, power. */
	struct {
		bool on;
		uint16_t lo, hi;
		uint64_t first, last;
		float power, floor;
	} wide;
	/* The C6's width records being joined. */
	struct wide_run wr;
	bool wr_on;
} sv;

/* ---- channels in frequency order ---- */

/* Channel k's offset from the centre in channel spacings: k up to M / 2 above, the rest below. */
static int chan_offset(unsigned int k)
{
	return k <= sv.m / 2U ? (int)k : (int)k - (int)sv.m;
}

/* Position in frequency order (0 is the lowest channel) and back. */
static unsigned int chan_pos(unsigned int k)
{
	return (unsigned int)(chan_offset(k) + (int)sv.m / 2 - 1);
}

static unsigned int pos_chan(unsigned int p)
{
	int s = (int)p - ((int)sv.m / 2 - 1);

	return s >= 0 ? (unsigned int)s : (unsigned int)(s + (int)sv.m);
}

static uint64_t chan_hz(unsigned int k)
{
	return (uint64_t)((int64_t)sv.centre_hz + (int64_t)chan_offset(k) * sv.spacing_hz);
}

/* ---- bursts ---- */

static void queue(struct chan_st *c)
{
	struct burst *b = c->b;

	c->b = NULL;
	if (b == NULL) {
		return;
	}
	/* Only quiet frames (a channel resumed after a hole that never got loud again): nothing. */
	if (!c->loud_set) {
		burst_put(b);
		return;
	}
	if (b->n == 0U || decoder_put(b) != 0) {
		app_stats.bursts_lost += b->n != 0U ? 1U : 0U;
		burst_put(b);
		return;
	}
	app_stats.bursts++;
}

static void open_burst(unsigned int k, const struct esdr_detq_ev *ev)
{
	struct chan_st *c = &sv.ch[k];
	struct burst *b = burst_get();

	if (b == NULL) {
		app_stats.bursts_lost++;
		c->skip = true;
		return;
	}
	b->chan = (uint16_t)k;
	b->channels = sv.m;
	b->rate_hz = sv.rate_hz;
	b->freq_hz = chan_hz(k);
	b->floor = ev->floor;
	b->gain = sv.gain;
	c->b = b;
	c->skip = false;
	c->loud_set = false;
	/* A neighbour open as well: this may be the skirt of a wider signal, or that its. */
	for (int d = -1; d <= 1; d += 2) {
		unsigned int p = chan_pos(k);

		if ((d < 0 && p == 0U) || (d > 0 && p + 1U >= sv.m)) {
			continue;
		}
		struct chan_st *n = &sv.ch[pos_chan((unsigned int)((int)p + d))];

		if (n->b != NULL) {
			n->b->flags |= BURST_F_NEIGHBOUR;
			b->flags |= BURST_F_NEIGHBOUR;
		}
	}
}

/* Words, four at a time: the P4's memcpy() is the ROM's, some 5.5 cycles a byte. */
static void copy_words(uint32_t *dst, const uint32_t *src, uint32_t n)
{
	for (; n >= 4U; n -= 4U, dst += 4, src += 4) {
		uint32_t a = src[0], b = src[1], c = src[2], d = src[3];

		dst[0] = a;
		dst[1] = b;
		dst[2] = c;
		dst[3] = d;
	}
	for (; n > 0U; n--) {
		*dst++ = *src++;
	}
}

static void zero_words(uint32_t *dst, uint32_t n)
{
	for (; n > 0U; n--) {
		*dst++ = 0U;
	}
}

/* Samples of a frame run from frame @p m0 into the channel's burst, a gap before it zero filled. */
static bool append(struct chan_st *c, const int16_t *iq, uint32_t n, uint64_t m0)
{
	struct burst *b = c->b;

	if (b->n == 0U) {
		b->m0 = m0;
		c->next = m0;
	}
	if (m0 > c->next) {
		uint64_t gap = m0 - c->next;

		if (b->n + gap > BURST_MAX_SAMPLES) {
			return false;
		}
		zero_words((uint32_t *)b->iq + b->n, (uint32_t)gap);
		b->n += (uint32_t)gap;
		b->flags |= BURST_F_TRANSIENT;
	} else if (m0 < c->next) {
		/* Frames already taken (cannot happen with the detector's own order): skip them. */
		uint64_t back = c->next - m0;

		if (back >= n) {
			return true;
		}
		iq += 2U * back;
		n -= (uint32_t)back;
	}
	n = MIN(n, BURST_MAX_SAMPLES - b->n);
	copy_words((uint32_t *)b->iq + b->n, (const uint32_t *)iq, n);
	b->n += n;
	c->next = b->m0 + b->n;
	return b->n < BURST_MAX_SAMPLES;
}

/* A channel to send at a block's end (esdr_detq_cb): its frames into its burst. */
static void det_cb(const struct esdr_detq *d, const struct esdr_detq_ev *ev, void *user)
{
	static int16_t take[2U * TAKE_MAX] __aligned(4);
	struct chan_st *c = &sv.ch[ev->chan];
	uint32_t pos = 0, fl, before;
	uint64_t m0;
	unsigned int n;

	ARG_UNUSED(user);
	if (ev->chan >= sv.m || (sv.mask[ev->chan / 32U] & BIT(ev->chan % 32U)) == 0U) {
		return;
	}
	if (c->wide) {
		return;
	}
	if ((ev->flags & ESDR_DET_OPEN) != 0U) {
		queue(c);
		open_burst(ev->chan, ev);
	} else if (c->resume && c->b == NULL && !c->skip) {
		open_burst(ev->chan, ev);
	}
	c->resume = false;
	if (c->skip || c->b == NULL) {
		if ((ev->flags & ESDR_DET_CLOSE) != 0U) {
			c->skip = false;
		}
		return;
	}
	before = c->b->n;
	while (pos < 4U * ev->n) {
		n = esdr_detq_take(d, ev, &pos, take, TAKE_MAX, &m0, &fl);
		if (n == 0U) {
			break;
		}
		if ((fl & ESDR_PFB_COMPLETE) == 0U) {
			c->b->flags |= BURST_F_TRANSIENT;
		}
		if (!append(c, take, n, m0)) {
			/* Full: what it has goes, the rest of the opening is skipped. */
			c->b->flags |= BURST_F_TRUNCATED;
			queue(c);
			c->skip = (ev->flags & ESDR_DET_CLOSE) == 0U;
			return;
		}
	}
	if (ev->power > ev->floor * sv.ratio) {
		if (!c->loud_set) {
			c->b->loud_from = before;
			c->loud_set = true;
		}
		c->b->loud_to = c->b->n;
	}
	c->b->power = MAX(c->b->power, ev->power);
	c->b->floor = ev->floor;
	if ((ev->flags & ESDR_DET_CLOSE) != 0U) {
		queue(c);
	}
}

/* ---- wideband: several adjacent channels open together ---- */

static void wide_emit(void)
{
	struct burst *b;

	sv.wide.on = false;
	for (unsigned int p = 0; p < sv.m; p++) {
		struct chan_st *c = &sv.ch[pos_chan(p)];

		if (c->wide) {
			c->wide = false;
			/* Still open: nothing of it until it closes and opens again. */
			c->skip = true;
		}
	}
	b = burst_get();
	if (b == NULL) {
		app_stats.bursts_lost++;
		return;
	}
	b->chan = (uint16_t)pos_chan(sv.wide.lo);
	b->channels = sv.m;
	b->rate_hz = sv.rate_hz;
	b->wide_lo = (uint16_t)pos_chan(sv.wide.lo);
	b->wide_hi = (uint16_t)pos_chan(sv.wide.hi);
	b->freq_hz = (chan_hz(b->wide_lo) + chan_hz(b->wide_hi)) / 2U;
	b->m0 = sv.wide.first;
	b->loud_to = (uint32_t)MIN(sv.wide.last - sv.wide.first, UINT32_MAX);
	b->power = sv.wide.power;
	b->floor = sv.wide.floor;
	b->gain = sv.gain;
	b->flags = BURST_F_WIDEBAND;
	if (decoder_put(b) != 0) {
		app_stats.bursts_lost++;
		burst_put(b);
		return;
	}
	app_stats.bursts_wide++;
}

/* After a decided block: the longest run of open channels in frequency order. */
static void wide_scan(uint64_t start)
{
	const uint8_t *open = esdr_detq_open(&sv.det);
	const float *pw = esdr_detq_power(&sv.det), *fl = esdr_detq_floor(&sv.det);
	unsigned int best_lo = 0, best_n = 0, run_lo = 0, run_n = 0;

	for (unsigned int p = 0; p < sv.m; p++) {
		if (open[pos_chan(p)] != 0U) {
			if (run_n++ == 0U) {
				run_lo = p;
			}
			if (run_n > best_n) {
				best_n = run_n;
				best_lo = run_lo;
			}
		} else {
			run_n = 0;
		}
	}
	if (best_n < BURST_WIDE_MIN) {
		if (sv.wide.on) {
			wide_emit();
		}
		return;
	}
	if (!sv.wide.on) {
		sv.wide.on = true;
		sv.wide.lo = (uint16_t)best_lo;
		sv.wide.hi = (uint16_t)(best_lo + best_n - 1U);
		sv.wide.first = start;
		sv.wide.power = 0.0f;
		sv.wide.floor = 0.0f;
	} else {
		sv.wide.lo = (uint16_t)MIN(sv.wide.lo, best_lo);
		sv.wide.hi = (uint16_t)MAX(sv.wide.hi, best_lo + best_n - 1U);
	}
	sv.wide.last = start + sv.det.b;
	for (unsigned int p = best_lo; p < best_lo + best_n; p++) {
		unsigned int k = pos_chan(p);
		struct chan_st *c = &sv.ch[k];

		sv.wide.power = MAX(sv.wide.power, pw[k]);
		sv.wide.floor = MAX(sv.wide.floor, fl[k]);
		if (!c->wide) {
			c->wide = true;
			/* Its samples are of a wider signal: not a burst of its own. */
			if (c->b != NULL) {
				burst_put(c->b);
				c->b = NULL;
			}
		}
	}
}

static void quad_cb(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	uint64_t start;
	uint32_t decided;

	ARG_UNUSED(user);
	esdr_detq_quad(&sv.det, m0, iq, valid, complete, det_cb, NULL);
	decided = esdr_detq_decided(&sv.det, &start);
	if (decided != sv.decided) {
		sv.decided = decided;
		wide_scan(start);
	}
}

/* ---- the C6's width records ---- */

static void wr_flush(void)
{
	if (!sv.wr_on) {
		return;
	}
	sv.wr_on = false;
	if (decoder_put_wide(&sv.wr) != 0) {
		app_stats.bursts_lost++;
		return;
	}
	app_stats.bursts_wide++;
}

static void wide_record(const struct esdr_seg *seg)
{
	struct esdr_seg_wide r;
	float mag;

	memcpy(&r, seg->iq, sizeof(r));
	app_stats.c6_wide++;
	if (sv.wr_on && seg->index > sv.wr.end + WIDE_JOIN / sv.decim) {
		wr_flush();
	}
	if (!sv.wr_on) {
		sv.wr = (struct wide_run){.centre_hz = sv.centre_hz, .index = seg->index,
					  .gain = seg->gain, .power_dbfs_x10 = INT16_MIN};
		sv.wr_on = true;
	}
	sv.wr.end = seg->index + sys_le16_to_cpu(r.span);
	mag = sqrtf((float)r.r1_i * r.r1_i + (float)r.r1_q * r.r1_q);
	if (mag > 0.0f) {
		sv.wr.r1_i += r.r1_i / mag;
		sv.wr.r1_q += r.r1_q / mag;
	}
	sv.wr.power_dbfs_x10 = MAX(sv.wr.power_dbfs_x10, seg->power_dbfs_x10);
	sv.wr.records++;
}

/* ---- segments ---- */

/*
 * A hole before the next segment: what the bank and the detector still hold goes out, then
 * every open burst ends here; channels the detector keeps open start a new burst with their
 * next samples.
 */
static void hole(void)
{
	(void)esdr_pfbq_flush(&sv.pfb, quad_cb, NULL);
	esdr_detq_flush(&sv.det, det_cb, NULL);
	if (sv.wide.on) {
		wide_emit();
	}
	for (unsigned int k = 0; k < sv.m; k++) {
		struct chan_st *c = &sv.ch[k];

		if (c->b != NULL) {
			c->b->flags |= BURST_F_HOLE;
			queue(c);
			c->resume = true;
		}
	}
}

static int seg_cb(const struct esdr_seg *seg, void *user)
{
	uint64_t gap;

	ARG_UNUSED(user);
	if (!sv.anchored) {
		/* The C6's axis: 80 MS/s, 12.5 ns a pair (25 at 40 MS/s). */
		timebase_anchor(seg->index * 25U * sv.decim / 2U);
		sv.anchored = true;
	}
	if ((seg->flags & ESDR_SEG_WIDE) != 0U) {
		if (seg->pairs == ESDR_SEG_WIDE_PAIRS) {
			wide_record(seg);
		}
		return 0;
	}
	app_stats.pairs += seg->pairs;
	sv.gain = seg->gain;
	if ((seg->flags & ESDR_SEG_DROPPED) != 0U) {
		for (unsigned int k = 0; k < sv.m; k++) {
			if (sv.ch[k].b != NULL) {
				sv.ch[k].b->flags |= BURST_F_DROPPED;
			}
		}
	}
	gap = esdr_seg_track_gap(&sv.track, seg);
	if (gap == ESDR_SEG_GAP_BACK) {
		/* A segment that goes back (a restart of the radio) starts the filterbank anew. */
		hole();
		esdr_pfbq_reset(&sv.pfb);
	} else if (gap != ESDR_SEG_GAP_START && gap >= HOLE_PAIRS / sv.decim) {
		hole();
	}
	(void)esdr_pfbq_push(&sv.pfb, seg->index, seg->iq, seg->pairs, seg->shift, seg->dc_i,
			     seg->dc_q, quad_cb, NULL);
	return 0;
}

/* ---- the thread ---- */

/* The survey's bank at @p in_rate_hz: M, the C6's decimation, the configurations, their memory. */
struct bank {
	uint16_t m;
	uint32_t decim;
	struct esdr_pfbq_cfg pc;
	struct esdr_detq_cfg dc;
	size_t pn, dn;
};

/*
 * @p in_rate_hz: the C6's rate, 80 or 40 MS/s. At 40 the bank runs half the channels the host
 * asked for: their spacing and rate stay what the decoders expect. 0, -EINVAL for a bank the
 * filterbank does not have (16 channels at 40 MS/s), -ENOMEM.
 */
static int bank_get(uint32_t in_rate_hz, struct bank *k)
{
	k->decim = in_rate_hz == ESDR_GAPLESS_RATE_HZ / 2U ? 2U : 1U;
	k->m = app_cfg.channels / k->decim;
	k->pc = (struct esdr_pfbq_cfg){.channels = k->m, .quads = ESDR_DETQ_QUADS(16U),
				       .gain = CHAN_GAIN};
	k->dc = (struct esdr_detq_cfg){.channels = k->m, .open_ratio = app_cfg.open_ratio};
	if ((in_rate_hz != ESDR_GAPLESS_RATE_HZ && k->decim == 1U) ||
	    k->m * k->decim != app_cfg.channels) {
		return -EINVAL;
	}
	k->pn = esdr_pfbq_mem_size(&k->pc);
	k->dn = esdr_detq_mem_size(&k->dc);
	if (k->pn == 0U || k->dn == 0U) {
		return -EINVAL;
	}
	return ROUND_UP(k->pn, 16U) + k->dn > sizeof(chan_mem) ? -ENOMEM : 0;
}

int survey_check(uint32_t in_rate_hz)
{
	struct bank k;

	return bank_get(in_rate_hz, &k);
}

static int survey_setup(uint64_t centre_hz, uint32_t in_rate_hz)
{
	struct bank k;
	int ret = bank_get(in_rate_hz, &k);
	const uint16_t m = k.m;
	const uint32_t decim = k.decim;
	const struct esdr_pfbq_cfg pc = k.pc;
	const struct esdr_detq_cfg dc = k.dc;
	const size_t pn = k.pn, dn = k.dn;

	if (ret != 0) {
		LOG_ERR("channelizer of %u channels at %u MS/s: %d", app_cfg.channels,
			in_rate_hz / 1000000U, ret);
		return ret;
	}
	if (esdr_pfbq_init(&sv.pfb, &pc, chan_mem, pn) != 0 ||
	    esdr_detq_init(&sv.det, &dc, chan_mem + ROUND_UP(pn, 16U), dn) != 0) {
		return -EINVAL;
	}
	esdr_seg_track_reset(&sv.track);
	memset(sv.ch, 0, sizeof(sv.ch));
	memset(&sv.wide, 0, sizeof(sv.wide));
	sv.wr_on = false;
	sv.m = m;
	sv.decim = decim;
	sv.spacing_hz = in_rate_hz / sv.m;
	/*
	 * Host bit k is the channel k spacings up (k up to channels / 2) or channels - k down. At
	 * 40 MS/s the bin at +-20 MHz holds both edges (the C6 drops samples without a filter):
	 * left out.
	 */
	memset(sv.mask, 0, sizeof(sv.mask));
	for (unsigned int c = 0; c < sv.m; c++) {
		int off = chan_offset(c);
		unsigned int hk = off >= 0 ? (unsigned int)off : (unsigned int)(off + app_cfg.channels);

		if ((app_cfg.mask[hk / 32U] & BIT(hk % 32U)) != 0U &&
		    !(decim > 1U && c == sv.m / 2U)) {
			sv.mask[c / 32U] |= BIT(c % 32U);
		}
	}
	sv.rate_hz = 2U * sv.spacing_hz;
	sv.centre_hz = centre_hz;
	sv.ratio = app_cfg.open_ratio > 1.0f ? app_cfg.open_ratio : 4.0f;
	sv.anchored = false;
	sv.decided = 0;
	sv.gain = ESDR_SEG_GAIN_AUTO;
	LOG_INF("survey at %u kHz, %u MS/s: %u channels of %u kHz (%u + %u bytes)",
		(unsigned int)(centre_hz / 1000U), in_rate_hz / 1000000U, sv.m,
		sv.spacing_hz / 1000U, (unsigned int)pn, (unsigned int)dn);
	return 0;
}

/* The stream stopped or the survey ends: what waits goes out; @p cut: open bursts end here. */
static void survey_flush(bool cut)
{
	(void)esdr_pfbq_flush(&sv.pfb, quad_cb, NULL);
	esdr_detq_flush(&sv.det, det_cb, NULL);
	wr_flush();
	if (!cut) {
		return;
	}
	if (sv.wide.on) {
		wide_emit();
	}
	for (unsigned int k = 0; k < sv.m; k++) {
		if (sv.ch[k].b != NULL) {
			sv.ch[k].b->flags |= BURST_F_CUT;
			queue(&sv.ch[k]);
		}
		sv.ch[k].skip = false;
		sv.ch[k].wide = false;
	}
}

static void handle_cmd(void)
{
	int c = (int)atomic_set(&sv.cmd, CMD_NONE);

	if (c == CMD_NONE) {
		return;
	}
	if (sv.mode == CMD_SURVEY) {
		survey_flush(true);
	} else if (sv.mode == CMD_NARROW) {
		narrow_stop();
	}
	sv.mode = 0;
	sv.result = 0;
	if (c == CMD_SURVEY) {
		sv.result = survey_setup(sv.arg_hz, sv.arg_rate);
		sv.mode = sv.result == 0 ? CMD_SURVEY : 0;
	} else if (c == CMD_NARROW) {
		sv.result = narrow_start(sv.arg_hz, sv.arg_rate);
		sv.mode = sv.result == 0 ? CMD_NARROW : 0;
	}
	k_sem_give(&sv.done);
}

static void stream_fn(void *a, void *b, void *c)
{
	uint8_t *blk;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		int got = k_msgq_get(&block_q, &blk, K_MSEC(IDLE_MS));

		handle_cmd();
		if (got != 0) {
			if (sv.mode == CMD_SURVEY) {
				survey_flush(false);
			}
			continue;
		}
		if (sv.mode == CMD_SURVEY) {
			uint64_t t0 = esdr_pfbq_cycles();

			if (esdr_seg_parse(blk, ESDR_BLOCK_BYTES, seg_cb, NULL) < 0) {
				app_stats.other_blocks++;
			} else {
				app_stats.seg_blocks++;
			}
			app_stats.chan_cycles += esdr_pfbq_cycles() - t0;
		} else if (sv.mode == CMD_NARROW) {
			narrow_block(blk);
		}
		radio_block_free(blk);
	}
}

/* The radio's blocks (radio.h's sink): to the thread, dropped when it lags. */
static void on_block(uint8_t *blk)
{
	if (k_msgq_put(&block_q, &blk, K_NO_WAIT) != 0) {
		app_stats.seg_dropped++;
		radio_block_free(blk);
	}
}

static int run_cmd(enum cmd c, uint64_t hz, uint32_t rate)
{
	k_sem_reset(&sv.done);
	sv.arg_hz = hz;
	sv.arg_rate = rate;
	atomic_set(&sv.cmd, c);
	/* The thread takes a command at least every IDLE_MS. */
	if (k_sem_take(&sv.done, K_MSEC(2000)) != 0) {
		return -ETIMEDOUT;
	}
	return sv.result;
}

int survey_start(uint64_t centre_hz, uint32_t in_rate_hz)
{
	return run_cmd(CMD_SURVEY, centre_hz, in_rate_hz);
}

int stream_narrow(uint64_t freq_hz, uint32_t rate_hz)
{
	return run_cmd(CMD_NARROW, freq_hz, rate_hz);
}

void survey_stop(void)
{
	(void)run_cmd(CMD_STOP, 0, 0);
}

void survey_block(uint8_t *blk)
{
	on_block(blk);
}

/* PIE is switched on per core and its registers are not saved on a context switch: with two
 * cores the stream thread never moves, and threads nobody pins stay on CPU 0.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP) || IS_ENABLED(CONFIG_SCHED_CPU_MASK_PIN_ONLY),
	     "the PIE stream thread must not move between cores");

static int stream_init(void)
{
	k_sem_init(&sv.done, 0, 1);
	k_thread_create(&stream_thread, stream_stack, K_THREAD_STACK_SIZEOF(stream_stack),
			stream_fn, NULL, NULL, NULL, STREAM_PRIO, 0, K_FOREVER);
	k_thread_name_set(&stream_thread, "stream");
#if defined(CONFIG_SMP)
	if (k_thread_cpu_pin(&stream_thread, CONFIG_APP_PCAP_STREAM_CPU) != 0) {
		LOG_ERR("stream thread not pinned");
	}
#endif
	k_thread_start(&stream_thread);
	return 0;
}
SYS_INIT(stream_init, APPLICATION, 10);
