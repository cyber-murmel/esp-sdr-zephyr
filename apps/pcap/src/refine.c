/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Refinement of a narrow burst (refine.h, doc/pcap.md "Refinement"): a mixer from a 32-bit phase
 * accumulator and a cascade of half-band decimators in float, block by block: each stage keeps
 * its last inputs as history before a linear block, so that the scratch is the stages' blocks
 * whatever the burst's length and the output may overwrite the input. Each stage is an 11-tap
 * equiripple half-band (Lawson's minimax on its pass band): flat within 0.014 dB up to an eighth
 * of its input rate (a quarter of its output rate), 55 dB down from three eighths, where the
 * images of that band lie. Every output sample is centred on input sample decim k (the stages
 * zero phase, the stream zero beyond its ends), so that sample indices scale exactly. Plain C, no
 * Zephyr: tests/unit/pcap_generic runs it on native_sim.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "refine.h"

/* Taps of a stage, the centre's index (the delay), the history a stage keeps. */
#define HB_TAPS    11U
#define HB_HALF    ((HB_TAPS - 1U) / 2U)
#define HB_HIST    (HB_TAPS - 1U)
/* Pairs the first stage takes at once. */
#define BLK        64U
/* The taps at 1, 3 and 5 samples from the centre (0.5); the others are zero. */
#define HB_C1      0.301612244f
#define HB_C3      -0.063869865f
#define HB_C5      0.013054642f
/* The noise power a stage passes of white noise: the sum of its squared taps. */
#define HB_NOISE   0.440439f
/* Stages at most: REFINE_DECIM_MAX = 2^STAGES_MAX. */
#define STAGES_MAX 6U
/* A 32-bit binary angle in radians. */
#define NCO_SCALE  (ESP_SDR_TAU_F / 4294967296.0f)

struct hb {
	/* HB_HIST pairs of history (I, Q), then up to BLK new ones. */
	float x[2U * (HB_HIST + BLK)];
	/* New pairs held, the input index of the first of them, the next output's centre (an
	 * input index), outputs still due. */
	uint32_t have, base, next, due;
};

struct rstate {
	struct hb hb[STAGES_MAX];
	unsigned int stages;
	int16_t *out;
	uint32_t k, n;
};

static void *align8(void *p)
{
	return (void *)(((uintptr_t)p + 7U) & ~(uintptr_t)7U);
}

static inline int16_t sat16(float v)
{
	if (v >= 32767.0f) {
		return INT16_MAX;
	}
	if (v <= -32768.0f) {
		return INT16_MIN;
	}
	return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static void put_out(struct rstate *st, float re, float im)
{
	if (st->k < st->n) {
		st->out[2U * st->k] = sat16(re);
		st->out[2U * st->k + 1U] = sat16(im);
		st->k++;
	}
}

/*
 * The outputs stage @p s's inputs complete, into the next stage (or the output), then its last
 * HB_HIST inputs kept as history. Output k is centred on input 2 k and needs the inputs up to
 * 2 k + HB_HALF.
 */
static void hb_run(struct rstate *st, unsigned int s)
{
	struct hb *h = &st->hb[s];
	struct hb *nx = s + 1U < st->stages ? &st->hb[s + 1U] : NULL;
	float *dst = nx != NULL ? &nx->x[2U * (HB_HIST + nx->have)] : NULL;
	uint32_t next = h->next, due = h->due, made = 0U;
	/* Input next's pair in x is next + HB_HIST - base; the window starts HB_HALF before. */
	const uint32_t end = h->base + h->have, off = HB_HIST - HB_HALF - h->base;

	while (due > 0U && next + HB_HALF < end) {
		const float *w = &h->x[2U * (next + off)];
		float re = 0.5f * w[10] + HB_C1 * (w[8] + w[12]) + HB_C3 * (w[4] + w[16]) +
			   HB_C5 * (w[0] + w[20]);
		float im = 0.5f * w[11] + HB_C1 * (w[9] + w[13]) + HB_C3 * (w[5] + w[17]) +
			   HB_C5 * (w[1] + w[21]);

		if (dst != NULL) {
			dst[2U * made] = re;
			dst[2U * made + 1U] = im;
			made++;
		} else {
			put_out(st, re, im);
		}
		next += 2U;
		due--;
	}
	h->next = next;
	h->due = due;
	if (nx != NULL) {
		nx->have += made;
	}
	memmove(h->x, &h->x[2U * h->have], 2U * HB_HIST * sizeof(h->x[0]));
	h->base = end;
	h->have = 0U;
}

/* Every stage from @p from on runs over what it holds (the first takes at most BLK at once:
 * each later one is given at most half of that and one). */
static void hb_rounds(struct rstate *st, unsigned int from)
{
	for (unsigned int s = from; s < st->stages; s++) {
		hb_run(st, s);
	}
}

unsigned int refine_decim(uint32_t rate_hz, uint32_t bandwidth_hz)
{
	unsigned int decim = 1U;

	if (bandwidth_hz == 0U) {
		return 1U;
	}
	while (decim < REFINE_DECIM_MAX &&
	       (uint64_t)rate_hz >= 4ULL * bandwidth_hz * (2ULL * decim)) {
		decim *= 2U;
	}
	return decim;
}

size_t refine_scratch(uint32_t n)
{
	(void)n;
	return sizeof(struct rstate) + 8U;
}

int refine_run(const struct burst *b, int32_t offset_hz, unsigned int decim, int16_t *out,
	       void *scratch, size_t scratch_len, struct burst *view)
{
	struct rstate *st;
	struct burst in;
	uint32_t want, step;
	float gain = 1.0f, fr, v, rr, ri;

	if (b == NULL || out == NULL || view == NULL || scratch == NULL) {
		return -EINVAL;
	}
	if (decim == 0U || decim > REFINE_DECIM_MAX || (decim & (decim - 1U)) != 0U ||
	    b->rate_hz == 0U || (b->n > 0U && b->iq == NULL)) {
		return -EINVAL;
	}
	if (scratch_len < refine_scratch(b->n)) {
		return -ENOMEM;
	}
	/* The caller's view may be b itself. */
	in = *b;
	st = align8(scratch);
	memset(st, 0, sizeof(*st));
	st->out = out;
	st->n = in.n / decim;
	want = in.n;
	while ((1U << st->stages) < decim) {
		want /= 2U;
		st->hb[st->stages].due = want;
		/* White noise through the first stage; the later ones see its flat middle, half
		 * the input's density, and pass HB_NOISE of what lies in their band. */
		gain *= st->stages == 0U ? HB_NOISE : 0.5f;
		st->stages++;
	}

	/* Mixed down by offset_hz: the phase of sample i is -tau i offset / rate, from 0. */
	fr = (float)offset_hz / (float)in.rate_hz;
	fr -= roundf(fr);
	v = -fr * 4294967296.0f;
	step = (uint32_t)(int64_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
	rr = cosf((float)(int32_t)step * NCO_SCALE);
	ri = sinf((float)(int32_t)step * NCO_SCALE);
	for (uint32_t i0 = 0; i0 < in.n; i0 += BLK) {
		uint32_t m = in.n - i0 < BLK ? in.n - i0 : BLK;
		float a = (float)(int32_t)(i0 * step) * NCO_SCALE;
		float orr = cosf(a), oi = sinf(a);
		float *x = st->stages > 0U ? &st->hb[0].x[2U * HB_HIST] : NULL;

		/* The phasor set at every block, rotated between. */
		for (uint32_t j = 0; j < m; j++) {
			float xr = in.iq[2U * (i0 + j)], xi = in.iq[2U * (i0 + j) + 1U];
			float yr = xr * orr - xi * oi, yi = xr * oi + xi * orr, t;

			if (x != NULL) {
				x[2U * j] = yr;
				x[2U * j + 1U] = yi;
			} else {
				put_out(st, yr, yi);
			}
			t = orr * rr - oi * ri;
			oi = orr * ri + oi * rr;
			orr = t;
		}
		if (x != NULL) {
			st->hb[0].have = m;
			hb_rounds(st, 0U);
		}
	}
	/* Each stage's tail: zeros in until it has made its outputs. */
	for (unsigned int s = 0; s < st->stages; s++) {
		struct hb *h = &st->hb[s];

		while (h->due > 0U) {
			memset(&h->x[2U * (HB_HIST + h->have)], 0,
			       2U * (BLK - h->have) * sizeof(h->x[0]));
			h->have = BLK;
			hb_rounds(st, s);
		}
	}

	*view = in;
	view->rate_hz = in.rate_hz / decim;
	view->freq_hz = (uint64_t)((int64_t)in.freq_hz + offset_hz);
	view->m0 = in.m0 / decim;
	view->n = st->n;
	view->loud_from = in.loud_from / decim;
	view->loud_to = in.loud_to / decim;
	if (view->n > 0U) {
		if (view->loud_to >= view->n) {
			view->loud_to = view->n - 1U;
		}
		if (view->loud_from > view->loud_to) {
			view->loud_from = view->loud_to;
		}
	}
	view->iq = out;
	/* A narrow signal passes whole, the noise by the stages' noise bandwidth. */
	view->floor = in.floor * gain;
	view->power = (in.power > in.floor ? in.power - in.floor : 0.0f) + in.floor * gain;
	return 0;
}
