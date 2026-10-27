/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The IEEE 802.15.4 decoder of apps/pcap (decode.h): a closed burst at 4 MS/s, one at 2 MS/s
 * (the channel mode's) read at 4 MS/s, or the band between two neighbouring channels of the bank
 * recombined from both (doc/pcap.md), streamed through a receiver of the software O-QPSK PHY
 * (lib/ieee802154/ieee154_phy.c). Every frame
 * comes out once, with its FCS verdict (bad ones only when asked), its first preamble sample (of
 * a pair: an index into lo), a carrier offset estimate, its mean power and the 802.15.4 channel
 * of the frequency it was decoded on. Plain C, no Zephyr: tests/unit/pcap_wpan runs it on
 * native_sim.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/ieee154_phy.h>

#include "dec_util.h"
#include "decode.h"

/* The only rate the PHY runs at: 2 samples a chip. */
#define WPAN_RATE_HZ 4000000U
/*
 * The channel mode's rate, the C6's boxcar over one chip: every other sample of the PHY's rate
 * interpolated on the way in (cubic midpoints). The PHY decodes that, from any offset of the
 * boxcar to the chips, down to the SNR it needs at 4 MS/s (some 6 dB).
 */
#define WPAN_HALF_HZ 2000000U

/* Frames of the two-neighbour combination (or of an interpolated burst) fed at a time. */
#define CHUNK_FRAMES 1024U

/*
 * The carrier offset is estimated over the frame's first CFO_SPAN samples inside the stream
 * (the SHR is 640 of them, every frame at least 1408), given at least CFO_MIN.
 */
#define CFO_SPAN 2048U
#define CFO_MIN  256U

/* The 2.4 GHz grid: 2405 MHz + 5 MHz (c - 11), c = 11 .. 26, a frequency within +-100 kHz. */
#define CHAN_BASE_HZ 2405000000LL
#define CHAN_STEP_HZ 5000000LL
#define CHAN_TOL_HZ  100000LL
#define CHAN_FIRST   11
#define CHAN_COUNT   16

/* What dec_wpan_scratch() asks for. */
struct scratch {
	struct ieee154_phy_rx rx;
	/* A chunk of the combination, being fed to the receiver. */
	int16_t chunk[2U * CHUNK_FRAMES];
	/* A frame's samples for its estimates (of a pair: recombined). */
	int16_t span[2U * CFO_SPAN];
	/* The packet going out, and a bad one held back for a good copy of it. */
	struct dec_wpan_pkt pkt, held;
};

/*
 * The stream a receiver is fed: one burst (hi NULL), or the combination of lo and hi over the
 * frames both hold, from absolute frame base_m, n frames. Sample 0 of the stream is sample
 * start_in_lo of lo (0 for one burst), where the packets' start counts from. half: lo is at
 * WPAN_HALF_HZ, the stream twice its samples at WPAN_RATE_HZ.
 */
struct stream {
	const struct burst *lo, *hi;
	uint64_t base_m;
	uint32_t n;
	int32_t start_in_lo;
	bool half;
};

struct ctx {
	struct stream st;
	struct scratch *s;
	dec_wpan_emit emit;
	void *user;
	int count;
	uint8_t channel;
	bool combined;
	/* s->held waits for the other phase tracker's verdict on the frame ending at held_end. */
	bool held;
	uint32_t held_end;
};

static inline int16_t sat16(int32_t v)
{
	return (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
}

/*
 * The band centred between channels k (lo) and k + 1 (hi) at the same 4 MS/s, n frames from
 * absolute frame index m (doc/pcap.md): z[m] = lo[m] exp(-j pi m / 2) + hi[m] exp(+j pi m / 2),
 * the factors by m mod 4 being 1, -j, -1, j for lo and 1, j, -1, -j for hi, saturated to int16.
 * Both bursts must hold the frames. Not static: the unit test checks the convention on tones.
 */
void dec_wpan_combine(const struct burst *lo, const struct burst *hi, uint64_t m, uint32_t n,
		      int16_t *out)
{
	const int16_t *a = lo->iq + 2U * (uint32_t)(m - lo->m0);
	const int16_t *b = hi->iq + 2U * (uint32_t)(m - hi->m0);
	unsigned int ph = (unsigned int)(m & 3U);

	for (uint32_t i = 0; i < n; i++, a += 2, b += 2, out += 2) {
		int32_t ai = a[0], aq = a[1], bi = b[0], bq = b[1], zi, zq;

		switch (ph) {
		case 0:
			zi = ai + bi;
			zq = aq + bq;
			break;
		case 1: /* -j lo = (aq, -ai), j hi = (-bq, bi) */
			zi = aq - bq;
			zq = bi - ai;
			break;
		case 2:
			zi = -ai - bi;
			zq = -aq - bq;
			break;
		default: /* j lo = (-aq, ai), -j hi = (bq, -bi) */
			zi = bq - aq;
			zq = ai - bi;
			break;
		}
		out[0] = sat16(zi);
		out[1] = sat16(zq);
		ph = (ph + 1U) & 3U;
	}
}

/*
 * Samples [from, from + count) at WPAN_RATE_HZ of burst @p b at WPAN_HALF_HZ: the even ones its
 * own, the odd ones the cubic midpoints (9 (x[m] + x[m + 1]) - x[m - 1] - x[m + 2]) / 16, the
 * burst zero beyond its ends.
 */
static void interpolate(const struct burst *b, uint32_t from, uint32_t count, int16_t *out)
{
	const int16_t *x = b->iq;
	const uint32_t n = b->n;

	for (uint32_t i = from; i < from + count; i++, out += 2) {
		uint32_t m = i / 2U;

		if ((i & 1U) == 0U) {
			out[0] = x[2U * m];
			out[1] = x[2U * m + 1U];
			continue;
		}
		for (uint32_t c = 0; c < 2U; c++) {
			int32_t x0 = x[2U * m + c];
			int32_t x1 = m + 1U < n ? x[2U * (m + 1U) + c] : 0;
			int32_t xm = m > 0U ? x[2U * (m - 1U) + c] : 0;
			int32_t x2 = m + 2U < n ? x[2U * (m + 2U) + c] : 0;

			out[c] = sat16((9 * (x0 + x1) - xm - x2) / 16);
		}
	}
}

/*
 * Samples [from, from + count) of the stream: in the burst itself, or interpolated or
 * recombined into buf.
 */
static const int16_t *stream_get(const struct stream *st, uint32_t from, uint32_t count,
				 int16_t *buf)
{
	if (st->half) {
		interpolate(st->lo, from, count, buf);
		return buf;
	}
	if (st->hi == NULL) {
		return st->lo->iq + 2U * from;
	}
	dec_wpan_combine(st->lo, st->hi, st->base_m + from, count, buf);
	return buf;
}

/* Sum of I^2 + Q^2 over count interleaved samples. */
static uint64_t power_sum(const int16_t *iq, uint32_t count)
{
	uint64_t acc = 0;

	for (uint32_t i = 0; i < count; i++) {
		int32_t xi = iq[2U * i], xq = iq[2U * i + 1U];

		acc += (uint32_t)(xi * xi) + (uint32_t)(xq * xq);
	}
	return acc;
}

/* Mean I^2 + Q^2 over the stream's samples [from, to), CFO_SPAN at a time. */
static float stream_power(const struct stream *st, struct scratch *s, uint32_t from, uint32_t to)
{
	uint64_t acc = 0;

	if (to <= from) {
		return 0.0f;
	}
	for (uint32_t p = from; p < to; p += CFO_SPAN) {
		uint32_t cnt = to - p < CFO_SPAN ? to - p : CFO_SPAN;

		acc += power_sum(stream_get(st, p, cnt, s->span), cnt);
	}
	return (float)acc / (float)(to - from);
}

/*
 * Carrier offset, Hz, of O-QPSK samples at 2 a chip (ieee154_dsp_cfo() in float, as the P4's FPU
 * computes it): x[n] conj(x[n - 2]) turns by +-90 degrees a chip (MSK) plus tau f T, T = 0.5 us;
 * its square loses the sign and its mean points at 180 degrees + 2 tau f T, whatever the chips.
 * The mean is removed first (carrier feedthrough sits at the channel's centre). Unambiguous
 * within +-500 kHz.
 */
static int32_t cfo_estimate(const int16_t *iq, uint32_t n)
{
	int64_t si = 0, sq = 0;
	float sr = 0.0f, sj = 0.0f, mi, mq;

	if (n < 3U) {
		return 0;
	}
	for (uint32_t k = 0; k < n; k++) {
		si += iq[2U * k];
		sq += iq[2U * k + 1U];
	}
	mi = (float)si / (float)n;
	mq = (float)sq / (float)n;
	for (uint32_t k = 2; k < n; k++) {
		float xi = (float)iq[2U * k] - mi, xq = (float)iq[2U * k + 1U] - mq;
		float yi = (float)iq[2U * k - 4U] - mi, yq = (float)iq[2U * k - 3U] - mq;
		/* z = x conj(y), scaled so that the sums of its square stay in float's range. */
		float zr = (xi * yi + xq * yq) * (1.0f / 32768.0f);
		float zj = (xq * yi - xi * yq) * (1.0f / 32768.0f);

		sr += zr * zr - zj * zj;
		sj += 2.0f * zr * zj;
	}
	if (sr == 0.0f && sj == 0.0f) {
		return 0;
	}
	/* arg(-S) = 2 tau f T */
	return (int32_t)lrintf(atan2f(-sj, -sr) / (ESP_SDR_TAU_F * 1e-6f));
}

/* Chip j (0, 1, ... from the first preamble chip) of a frame as +-1: SHR, PHR, then the PSDU. */
static int chip_pm(const uint8_t *psdu, uint8_t len, uint32_t j)
{
	uint32_t b = j / 64U;
	uint8_t v = b < 4U ? 0x00U : (b == 4U ? 0xA7U : (b == 5U ? len : psdu[b - 6U]));
	uint32_t c = ieee154_phy_chips((j % 64U) < 32U ? (v & 0xFU) : (v >> 4));

	return ((c >> (31U - j % 32U)) & 1U) != 0U ? 1 : -1;
}

/*
 * The same at 2 MS/s, the C6's boxcar over each chip, aided by the frame's chips: the phase
 * from sample to sample over the frame (sample @p first on its first chip), less what the chips
 * turn (O-QPSK as MSK: chip j turns the phase by (-1)^(j + 1) c[j] c[j - 1] 90 degrees), over
 * the samples. Each step stays within +-180 degrees whatever the boxcar's offset to the chips,
 * for an offset within +-250 kHz.
 */
static int32_t cfo_estimate_half(const struct burst *b, uint32_t first, const uint8_t *psdu,
				 uint8_t len)
{
	const uint32_t nchips = (6U + len) * 64U;
	uint32_t e = first + nchips < b->n ? first + nchips : b->n;
	int64_t meas = 0, chips = 0;

	if (e < first + 2U + CFO_MIN / 2U) {
		return 0;
	}
	for (uint32_t k = first + 2U; k < e; k++) {
		const int16_t *x = &b->iq[2U * k];

		meas += dec_step(x[-2], x[-1], x[0], x[1]);
		/* A binary angle: 90 degrees are 16384. */
		chips += ((k - first) & 1U ? 16384 : -16384) * chip_pm(psdu, len, k - first) *
			 chip_pm(psdu, len, k - first - 1U);
	}
	return (int32_t)lrintf(dec_angle_hz((float)(meas - chips) / (float)(e - first - 2U),
					    (float)WPAN_HALF_HZ));
}

/* The packet of a frame the receiver just completed, its estimates from the stream. */
static void describe(struct ctx *c, struct dec_wpan_pkt *p, const uint8_t *psdu, uint8_t len,
		     int lqi, bool fcs_ok, uint32_t end)
{
	const struct stream *st = &c->st;
	/* Position 0 of the receiver is sample 0 of the stream. */
	int32_t start = (int32_t)ieee154_phy_rx_frame_start_i(&c->s->rx);
	uint32_t from = start < 0 ? 0U : (uint32_t)start;
	uint32_t to = end < st->n ? end : st->n;

	memset(p, 0, sizeof(*p));
	p->len = len;
	memcpy(p->psdu, psdu, len);
	p->fcs_ok = fcs_ok;
	p->lqi = (uint8_t)(lqi < 0 ? 0 : (lqi > 255 ? 255 : lqi));
	p->start = (st->half ? start / 2 : start) + st->start_in_lo;
	p->channel = c->channel;
	p->combined = c->combined;
	if (to > from) {
		uint32_t cnt = to - from < CFO_SPAN ? to - from : CFO_SPAN;

		if (st->half) {
			p->offset_hz = cfo_estimate_half(st->lo, from / 2U, psdu, len);
		} else if (cnt >= CFO_MIN) {
			p->offset_hz = cfo_estimate(stream_get(st, from, cnt, c->s->span), cnt);
		}
		p->power = stream_power(st, c->s, from, to);
	}
}

static void put(struct ctx *c, const struct dec_wpan_pkt *p)
{
	c->count++;
	c->emit(p, c->user);
}

/*
 * A frame from the receiver. One whose FCS failed is held back: the other phase tracker may
 * still complete the same frame (an end within IEEE154_PHY_DUP_SAMPLES) with a good FCS, and that
 * copy replaces it. Anything later, or the end of the stream, lets it go.
 */
static void frame(struct ctx *c, const uint8_t *psdu, uint8_t len, int lqi, bool fcs_ok)
{
	struct scratch *s = c->s;
	uint32_t end = ieee154_phy_rx_position_i(&s->rx);

	if (c->held) {
		if ((int32_t)(end - c->held_end) <= IEEE154_PHY_DUP_SAMPLES) {
			if (fcs_ok) {
				c->held = false;
				describe(c, &s->pkt, psdu, len, lqi, true, end);
				put(c, &s->pkt);
			}
			return;
		}
		c->held = false;
		put(c, &s->held);
	}
	if (fcs_ok) {
		describe(c, &s->pkt, psdu, len, lqi, true, end);
		put(c, &s->pkt);
	} else {
		describe(c, &s->held, psdu, len, lqi, false, end);
		c->held = true;
		c->held_end = end;
	}
}

static void on_good(const uint8_t *psdu, uint8_t len, int lqi, void *user)
{
	frame(user, psdu, len, lqi, true);
}

static void on_any(const uint8_t *psdu, uint8_t len, int lqi, bool fcs_ok, void *user)
{
	frame(user, psdu, len, lqi, fcs_ok);
}

/*
 * A fresh receiver for the stream, delivering good frames or all of them. The PHY's chip tables
 * are built on the first run (the decoders run on one thread; ieee154_phy_init() writes the same
 * values whenever it runs).
 */
static void receiver_start(struct ctx *c, bool keep_bad)
{
	static bool tables;

	if (!tables) {
		ieee154_phy_init();
		tables = true;
	}
	ieee154_phy_rx_init(&c->s->rx);
	if (keep_bad) {
		ieee154_phy_set_all_cb_i(&c->s->rx, on_any, c);
	} else {
		ieee154_phy_set_rx_cb_i(&c->s->rx, on_good, c);
	}
}

/* The stream is over: a frame still held back goes out. */
static int receiver_end(struct ctx *c)
{
	if (c->held) {
		c->held = false;
		put(c, &c->s->held);
	}
	return c->count;
}

static int check_scratch(void *scratch, size_t scratch_len)
{
	if (scratch == NULL || scratch_len < sizeof(struct scratch)) {
		return -ENOMEM;
	}
	if (((uintptr_t)scratch & (sizeof(void *) - 1U)) != 0U) {
		return -EINVAL;
	}
	return 0;
}

size_t dec_wpan_scratch(uint32_t n)
{
	(void)n;
	return sizeof(struct scratch);
}

int dec_wpan_run(const struct burst *b, const struct dec_wpan_cfg *cfg, void *scratch,
		 size_t scratch_len, dec_wpan_emit emit, void *user)
{
	struct ctx c;
	int ret;

	if (b == NULL || cfg == NULL || emit == NULL) {
		return -EINVAL;
	}
	if (b->rate_hz != WPAN_RATE_HZ && b->rate_hz != WPAN_HALF_HZ) {
		return -ENOTSUP;
	}
	ret = check_scratch(scratch, scratch_len);
	if (ret != 0) {
		return ret;
	}
	if (b->n == 0U || b->iq == NULL) {
		return 0;
	}
	memset(&c, 0, sizeof(c));
	c.st.lo = b;
	c.st.base_m = b->m0;
	c.st.n = b->n;
	c.st.half = b->rate_hz == WPAN_HALF_HZ;
	c.s = scratch;
	c.emit = emit;
	c.user = user;
	c.channel = (uint8_t)dec_wpan_channel(b->freq_hz);
	receiver_start(&c, cfg->keep_bad);
	if (!c.st.half) {
		ieee154_phy_rx_block_i(&c.s->rx, b->iq, b->n);
		return receiver_end(&c);
	}
	c.st.n = 2U * b->n;
	for (uint32_t off = 0; off < c.st.n; off += CHUNK_FRAMES) {
		uint32_t cnt = c.st.n - off < CHUNK_FRAMES ? c.st.n - off : CHUNK_FRAMES;

		interpolate(b, off, cnt, c.s->chunk);
		ieee154_phy_rx_block_i(&c.s->rx, c.s->chunk, cnt);
	}
	return receiver_end(&c);
}

int dec_wpan_run_pair(const struct burst *lo, const struct burst *hi,
		      const struct dec_wpan_cfg *cfg, void *scratch, size_t scratch_len,
		      dec_wpan_emit emit, void *user)
{
	struct ctx c;
	uint64_t from, to;
	int ret;

	if (lo == NULL || hi == NULL || cfg == NULL || emit == NULL) {
		return -EINVAL;
	}
	/* Neighbours on the bank's grid; the last channel and channel 0 are neighbours too. */
	if (lo->channels != hi->channels ||
	    !(lo->chan + 1U == hi->chan || (lo->chan + 1U == lo->channels && hi->chan == 0U))) {
		return -EINVAL;
	}
	if (lo->rate_hz != WPAN_RATE_HZ || hi->rate_hz != WPAN_RATE_HZ) {
		return -ENOTSUP;
	}
	ret = check_scratch(scratch, scratch_len);
	if (ret != 0) {
		return ret;
	}
	if (lo->n == 0U || hi->n == 0U || lo->iq == NULL || hi->iq == NULL) {
		return 0;
	}
	from = lo->m0 > hi->m0 ? lo->m0 : hi->m0;
	to = lo->m0 + lo->n < hi->m0 + hi->n ? lo->m0 + lo->n : hi->m0 + hi->n;
	if (to <= from) {
		return 0;
	}
	memset(&c, 0, sizeof(c));
	c.st.lo = lo;
	c.st.hi = hi;
	c.st.base_m = from;
	c.st.n = (uint32_t)(to - from);
	c.st.start_in_lo = (int32_t)(from - lo->m0);
	c.s = scratch;
	c.emit = emit;
	c.user = user;
	c.channel = (uint8_t)dec_wpan_channel((lo->freq_hz + hi->freq_hz) / 2U);
	c.combined = true;
	receiver_start(&c, cfg->keep_bad);
	for (uint32_t off = 0; off < c.st.n; off += CHUNK_FRAMES) {
		uint32_t cnt = c.st.n - off < CHUNK_FRAMES ? c.st.n - off : CHUNK_FRAMES;

		dec_wpan_combine(lo, hi, from + off, cnt, c.s->chunk);
		ieee154_phy_rx_block_i(&c.s->rx, c.s->chunk, cnt);
	}
	return receiver_end(&c);
}

int dec_wpan_channel(uint64_t freq_hz)
{
	int64_t d = (int64_t)freq_hz - CHAN_BASE_HZ, k, off;

	if (d < -CHAN_TOL_HZ) {
		return 0;
	}
	k = (d + CHAN_STEP_HZ / 2) / CHAN_STEP_HZ;
	if (k >= CHAN_COUNT) {
		return 0;
	}
	off = d - k * CHAN_STEP_HZ;
	if (off < -CHAN_TOL_HZ || off > CHAN_TOL_HZ) {
		return 0;
	}
	return CHAN_FIRST + (int)k;
}
