/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of tests/common/src/esdr_pfb_ref.c: the float channelizer against numpy's direct
 * evaluation of its definition (pfb_ref.h, tools/gen_ref.py) and against a direct double model
 * here, over gapped streams of every M with a shift and DC per segment; the DC removed exactly,
 * and without bias from items quantized as the C6 does; the prototype's response; tone gain and
 * phase across gaps; the detector on synthetic bursts; and a benchmark, printed only.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "esdr_pfb_ref.h"
#include "pfb_ref.h"
#include "test_rng.h"

/* Host side (tests/common/src/host_clock.c). */
uint64_t test_host_ns(void);
uint64_t test_host_cycles(void);

static const uint16_t channels[] = {16, 20, 32, 40, 64, 80};

/* The largest bank (M 80, T 16, 41 frames) fits. */
static uint8_t mem[120000] __aligned(16);
static uint8_t dmem[40000] __aligned(16);
static struct esdr_pfb pfb;
static struct esdr_det det;

static void setup(uint16_t m, uint16_t t, uint16_t frames, uint32_t chunk, float gain)
{
	struct esdr_pfb_cfg cfg = {
		.channels = m, .taps = t, .frames = frames, .chunk = chunk, .gain = gain,
	};

	zassert_true(esdr_pfb_mem_size(&cfg) <= sizeof(mem), "mem %u",
		     (unsigned int)esdr_pfb_mem_size(&cfg));
	zassert_ok(esdr_pfb_init(&pfb, &cfg, mem, sizeof(mem)), "init M %u T %u", m, t);
}

/* ---- prototype ---- */

static inline double tapd(const struct esdr_pfb *p, unsigned int n)
{
	return (double)esdr_pfb_tap(p, n);
}

/* |H(f)| of the bank's prototype (symmetric: real about its centre), f in cycles per pair. */
static double response(const struct esdr_pfb *p, double f)
{
	double s = 0.0, mid = 0.5 * (double)(p->l - 1U);

	for (unsigned int n = 0; n < p->l; n++) {
		s += tapd(p, n) * cos(ESP_SDR_TAU * f * ((double)n - mid));
	}
	return fabs(s);
}

ZTEST(esdr_pfb, test_design)
{
	double sum;

	for (size_t i = 0; i < ARRAY_SIZE(pfb_refs); i++) {
		const struct pfb_ref *r = &pfb_refs[i];
		double h2 = 0.0, stop = 0.0, edge;

		sum = 0.0;
		setup(r->m, PFB_REF_TAPS, 1, 0, 0.0f);
		for (unsigned int n = 0; n < pfb.l; n++) {
			double h = tapd(&pfb, n);

			sum += h;
			h2 += h * h;
			zassert_equal(h, tapd(&pfb, pfb.l - 1U - n), "symmetric");
		}
		zassert_within(sum, 1.0, 1e-6, "DC gain %f", sum);
		zassert_within(h2 / r->h2, 1.0, 1e-5, "sum h^2 %g, numpy %g", h2, r->h2);
		edge = 20.0 * log10(response(&pfb, 0.5 / r->m));
		for (double f = 1.0 / r->m; f < 0.5; f += 1.0 / (8.0 * pfb.l)) {
			stop = fmax(stop, response(&pfb, f));
		}
		stop = 20.0 * log10(stop);
		printk("M %2u T 8: %.3f dB at fs/(2M), stop band from fs/M %.1f dB "
		       "(numpy %.3f, %.1f)\n", r->m, edge, stop, (double)r->edge_db,
		       (double)r->stop_db);
		zassert_within(edge, -6.02, 0.05, "edge %.3f dB", edge);
		zassert_within(edge, (double)r->edge_db, 0.005, "edge %.3f dB", edge);
		zassert_true(stop <= -60.0, "stop band %.1f dB", stop);
		zassert_within(stop, (double)r->stop_db, 0.3, "stop band %.1f dB", stop);
	}
	/* The gain scales the taps: full scale (512) comes out as 32768 at 64. */
	setup(40, 8, 1, 0, 64.0f);
	sum = 0.0;
	for (unsigned int n = 0; n < pfb.l; n++) {
		sum += tapd(&pfb, n);
	}
	zassert_within(sum, 64.0, 1e-4, "gain %f", sum);
}

/* ---- against numpy ---- */

static int8_t ref_items[PFB_REF_ITEMS];

/* pfb_ref.h's items: an LCG's top bytes. */
static void make_ref_items(void)
{
	uint32_t x = PFB_REF_SEED;

	for (size_t i = 0; i < PFB_REF_ITEMS; i++) {
		x = x * 1664525U + 1013904223U;
		ref_items[i] = (int8_t)(x >> 24);
	}
}

#define REF_FRAMES_MAX 512U

struct ref_ctx {
	const struct pfb_ref *r;
	uint32_t n;
	uint64_t want_m[REF_FRAMES_MAX];
	uint8_t want_f[REF_FRAMES_MAX];
	double level, cs[ESDR_PFB_M_MAX], sn[ESDR_PFB_M_MAX];
	double worst_pw, worst_ws, worst_full;
	unsigned int nfull;
};

static struct ref_ctx rc;

static void ref_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	struct ref_ctx *c = user;
	const struct pfb_ref *r = c->r;
	uint32_t i = c->n++;
	double pw = 0.0, ws = 0.0;

	zassert_true(i < r->frames, "M %u: more than %u frames", r->m, r->frames);
	zassert_equal(m, c->want_m[i], "M %u frame %u: m %llu, numpy %llu", r->m, i,
		      (unsigned long long)m, (unsigned long long)c->want_m[i]);
	zassert_equal(flags, c->want_f[i], "M %u frame %u: flags %u, numpy %u", r->m, i, flags,
		      c->want_f[i]);
	for (unsigned int k = 0; k < r->m; k++) {
		double yr = (double)iq[2 * k], yi = (double)iq[2 * k + 1];

		pw += yr * yr + yi * yi;
		ws += yr * c->cs[k] + yi * c->sn[k];
	}
	c->worst_pw = fmax(c->worst_pw, fabs(pw - (double)r->pw[i]) / c->level);
	c->worst_ws = fmax(c->worst_ws, fabs(ws - (double)r->ws[i]) / sqrt(r->m * c->level));
	for (unsigned int s = 0; s < r->nsel; s++) {
		if (r->sel[s] != i) {
			continue;
		}
		const float *y = &r->full[2U * r->m * s];

		for (unsigned int k = 0; k < 2U * r->m; k++) {
			c->worst_full = fmax(c->worst_full, fabs((double)(iq[k] - y[k])) /
							    sqrt(c->level / r->m));
		}
		c->nfull++;
	}
}

ZTEST(esdr_pfb, test_numpy_reference)
{
	make_ref_items();
	for (size_t i = 0; i < ARRAY_SIZE(pfb_refs); i++) {
		const struct pfb_ref *r = &pfb_refs[i];
		uint32_t n = 0;

		memset(&rc, 0, sizeof(rc));
		rc.r = r;
		zassert_true(r->frames <= REF_FRAMES_MAX);
		for (unsigned int j = 0; j < r->ndm; j++) {
			for (unsigned int q = 0; q < r->dm[2 * j + 1]; q++) {
				rc.want_m[n++] = r->m0 + r->dm[2 * j] + q;
			}
		}
		zassert_equal(n, r->frames);
		n = 0;
		for (unsigned int j = 0; j < r->nflags; j++) {
			for (unsigned int q = 0; q < r->flags[2 * j + 1]; q++) {
				rc.want_f[n++] = (uint8_t)r->flags[2 * j];
			}
		}
		zassert_equal(n, r->frames);
		for (unsigned int f = 0; f < r->frames; f++) {
			rc.level += (double)r->pw[f] / r->frames;
		}
		for (unsigned int k = 0; k < r->m; k++) {
			double a = 0.6180339887498949 * k;

			a = ESP_SDR_TAU * (a - floor(a));
			rc.cs[k] = cos(a);
			rc.sn[k] = sin(a);
		}
		setup(r->m, PFB_REF_TAPS, 1, 0, 0.0f);
		for (size_t s = 0; s < ARRAY_SIZE(pfb_ref_segs); s++) {
			const struct pfb_ref_seg *g = &pfb_ref_segs[s];

			int ret = esdr_pfb_push(&pfb, PFB_REF_I0 + g->off, &ref_items[2 * g->off],
						g->pairs, g->shift, g->dc_i, g->dc_q, ref_frame,
						&rc);

			zassert_true(ret >= 0, "push %d", ret);
		}
		printk("M %2u vs numpy: %u frames, worst |y|^2 sum %.1e, weighted sum %.1e, full "
		       "frames %.1e (of the level)\n", r->m, rc.n, rc.worst_pw, rc.worst_ws,
		       rc.worst_full);
		zassert_equal(rc.n, r->frames, "M %u: %u frames, numpy %u", r->m, rc.n, r->frames);
		zassert_equal(rc.nfull, r->nsel);
		zassert_true(rc.worst_pw < 1e-4 && rc.worst_ws < 1e-4 && rc.worst_full < 1e-4);
	}
}

/* ---- against a direct model ---- */

/* Pairs a case covers, after PAD of zeros. */
#define DM_PAD  1300U
#define DM_SPAN 14000U

static double dxr[DM_PAD + DM_SPAN], dxi[DM_PAD + DM_SPAN];
static bool dknown[DM_PAD + DM_SPAN];
static int8_t ditems[2 * DM_SPAN];
static float dfl[2 * DM_SPAN];

struct dm_ctx {
	uint64_t i0;
	double tol, worst;
	bool have_last;
	uint64_t last_m;
	uint32_t frames, ring;
	/* The last frames' outputs and where they were, to check the ring keeps them. */
	const float *back[8];
	float copy[8][2];
};

static struct dm_ctx dc;

/* The definition, in double: frame m's outputs, and whether its window was all received. */
static bool direct(const struct esdr_pfb *p, uint64_t m, double *yr, double *yi)
{
	static double wc[ESDR_PFB_M_MAX], wsn[ESDR_PFB_M_MAX];
	const unsigned int mm = p->m;
	const uint64_t nn = m * p->d;
	long o = (long)(nn - dc.i0) + (long)DM_PAD;
	bool complete = true;

	for (unsigned int k = 0; k < mm; k++) {
		yr[k] = 0.0;
		yi[k] = 0.0;
		wc[k] = cos(-ESP_SDR_TAU * k / mm);
		wsn[k] = sin(-ESP_SDR_TAU * k / mm);
	}
	for (unsigned int n = 0; n < p->l; n++, o--) {
		if (!dknown[o]) {
			complete = false;
			continue;
		}
		const double h = tapd(p, n);
		const double xr = h * dxr[o], xi = h * dxi[o];
		const unsigned int e = (unsigned int)((nn - n) % mm);
		unsigned int idx = 0;

		/* exp(-j tau k e / M), k e taken mod M step by step. */
		for (unsigned int k = 0; k < mm; k++) {
			yr[k] += xr * wc[idx] - xi * wsn[idx];
			yi[k] += xr * wsn[idx] + xi * wc[idx];
			idx += e;
			idx -= idx >= mm ? mm : 0U;
		}
	}
	return complete;
}

static void dm_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	static double yr[ESDR_PFB_M_MAX], yi[ESDR_PFB_M_MAX];
	struct dm_ctx *c = user;
	long o = (long)(m * pfb.d - c->i0) + (long)DM_PAD;
	bool complete;
	uint32_t want;

	zassert_true(o >= (long)DM_PAD && o < (long)(DM_PAD + DM_SPAN) && dknown[o],
		     "frame %llu: its newest pair was not received", (unsigned long long)m);
	zassert_true(!c->have_last || m > c->last_m, "frames out of order");
	complete = direct(&pfb, m, yr, yi);
	want = (complete ? ESDR_PFB_COMPLETE : 0U) |
	       (!c->have_last || m != c->last_m + 1U ? ESDR_PFB_GAP : 0U);
	zassert_equal(flags, want, "frame %llu: flags %u, want %u", (unsigned long long)m, flags,
		      want);
	for (unsigned int k = 0; k < pfb.m; k++) {
		c->worst = fmax(c->worst, fabs((double)iq[2 * k] - yr[k]) / c->tol);
		c->worst = fmax(c->worst, fabs((double)iq[2 * k + 1] - yi[k]) / c->tol);
	}
	/* The frame ring - 1 frames back still holds what it was given. */
	unsigned int back = MIN(c->ring - 1U, 7U);

	if (c->frames >= back && back > 0U) {
		unsigned int s = (c->frames - back) % 8U;

		zassert_true(c->back[s][0] == c->copy[s][0] && c->back[s][1] == c->copy[s][1],
			     "ring of %u frames lost frame %u back", c->ring, back);
	}
	c->back[c->frames % 8U] = iq;
	c->copy[c->frames % 8U][0] = iq[0];
	c->copy[c->frames % 8U][1] = iq[1];
	c->frames++;
	c->have_last = true;
	c->last_m = m;
}

/* Frames whose newest pair was received, between offsets 0 and @p end. */
static uint32_t frames_expected(const struct esdr_pfb *p, uint32_t end)
{
	uint32_t n = 0;

	for (uint32_t o = 0; o < end; o++) {
		n += dknown[DM_PAD + o] && (dc.i0 + o) % p->d == 0U;
	}
	return n;
}

/*
 * One case: segments of random lengths separated by gaps of 0, under L (zeros), L - 1, L and more
 * (new runs), int8 with random shifts and DC or float.
 */
static void direct_case(uint16_t m, uint16_t t, uint16_t frames, uint32_t chunk, float gain,
			bool f32, uint64_t seed)
{
	uint32_t off = 0, segs = 0;
	double rms = 0.0;
	unsigned int count = 0;

	test_rng_seed(seed);
	setup(m, t, frames, chunk, gain);
	memset(&dc, 0, sizeof(dc));
	memset(dknown, 0, sizeof(dknown));
	memset(dxr, 0, sizeof(dxr));
	memset(dxi, 0, sizeof(dxi));
	dc.i0 = (1ULL << 40) + test_rng_u32();
	dc.ring = frames != 0U ? frames : 1U;
	for (uint32_t i = 0; i < 2U * DM_SPAN; i++) {
		ditems[i] = (int8_t)(test_rng_u32() >> 24);
		dfl[i] = (float)(test_rng_gauss() * 300.0);
	}
	while (true) {
		uint32_t pairs = 1U + test_rng_u32() % 1500U, shift = test_rng_u32() % 3U;
		uint32_t kind = test_rng_u32() % 6U, gap;
		/* The segment's DC, int8 as esdr_seg_hdr carries it. */
		int32_t dci = (int32_t)(test_rng_u32() >> 24) - 128;
		int32_t dcq = (int32_t)(test_rng_u32() >> 24) - 128;
		/* An item stands for the middle of its step (the C6's shift floors). */
		const double sc = (double)(1U << shift), half = 0.5 * (sc - 1.0);
		int ret;

		gap = kind == 0U ? 0U : kind == 1U ? 1U + test_rng_u32() % (pfb.l - 1U) :
			kind == 2U ? pfb.l - 1U : kind == 3U ? pfb.l : kind == 4U ? pfb.l + 1U :
			pfb.l + test_rng_u32() % 3000U;
		if (segs > 0U) {
			off += gap;
		}
		if (off + pairs > DM_SPAN) {
			break;
		}
		for (uint32_t j = 0; j < pairs; j++) {
			uint32_t o = off + j;

			dknown[DM_PAD + o] = true;
			if (f32) {
				dxr[DM_PAD + o] = dfl[2 * o];
				dxi[DM_PAD + o] = dfl[2 * o + 1];
			} else {
				dxr[DM_PAD + o] = ditems[2 * o] * sc + half - dci;
				dxi[DM_PAD + o] = ditems[2 * o + 1] * sc + half - dcq;
			}
			rms += dxr[DM_PAD + o] * dxr[DM_PAD + o];
			rms += dxi[DM_PAD + o] * dxi[DM_PAD + o];
			count++;
		}
		dc.tol = 1e-4 * (gain != 0.0f ? (double)gain : 1.0) * sqrt(rms / count);
		if (f32) {
			ret = esdr_pfb_push_f32(&pfb, dc.i0 + off, &dfl[2 * off], pairs, dm_frame,
						&dc);
		} else {
			ret = esdr_pfb_push(&pfb, dc.i0 + off, &ditems[2 * off], pairs, shift, dci,
					    dcq, dm_frame, &dc);
		}
		zassert_true(ret >= 0, "push %d", ret);
		off += pairs;
		segs++;
	}
	zassert_equal(dc.frames, frames_expected(&pfb, off), "frames %u, want %u", dc.frames,
		      frames_expected(&pfb, off));
	printk("M %2u T %2u chunk %4u frames %2u %s: %u segments, %u frames, worst %.2f of 1e-4 "
	       "of the input rms\n", m, t, chunk, frames, f32 ? "f32 " : "int8", segs, dc.frames,
	       dc.worst);
	zassert_true(dc.worst <= 1.0, "worst %.2f", dc.worst);
}

ZTEST(esdr_pfb, test_direct_model)
{
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		direct_case(channels[i], 8, 1, 0, 0.0f, false, 11U + i);
	}
	direct_case(16, 4, 3, 64, 1.0f, false, 21U);
	direct_case(32, 12, 33, 0, 64.0f, true, 22U);
	direct_case(40, 16, 8, 160, 0.37f, false, 23U);
	direct_case(64, 8, 33, 256, 1.0f, true, 24U);
	direct_case(80, 4, 2, 327, 2.0f, false, 25U);
	direct_case(80, 16, 41, 0, 1.0f, true, 26U);
}

/* ---- DC ---- */

/* Constant items from offset off: I item i, Q item q, at shift 0 the segment's DC. */
struct dc_seg {
	uint32_t off, pairs;
	int8_t i, q;
};

struct dc_ctx {
	unsigned int frames, complete;
	/* Largest output of any channel; channel 0's largest error from want on complete frames. */
	double worst, err0, want_r, want_i;
};

static struct dc_ctx zc, nc;

static void dc_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	struct dc_ctx *c = user;

	ARG_UNUSED(m);
	for (unsigned int k = 0; k < 2U * pfb.m; k++) {
		c->worst = fmax(c->worst, fabs((double)iq[k]));
	}
	if ((flags & ESDR_PFB_COMPLETE) != 0U) {
		double er = (double)iq[0] - c->want_r, ei = (double)iq[1] - c->want_i;

		c->err0 = fmax(c->err0, hypot(er, ei));
		c->complete++;
	}
	c->frames++;
}

/*
 * A constant input at its segment's DC, at shift 0 (no step whose middle to take; I and Q apart,
 * the DC changing at every segment) over a gap that zero-fills and one that restarts: every
 * output of every frame is zero, complete or not. Told no DC, the last segment gives gain dc in
 * channel 0 once complete. Shifts 1 and 2 take the C6's quantizer: test_dc_floor.
 */
ZTEST(esdr_pfb, test_dc)
{
	static const struct dc_seg segs[] = {
		{0, 600, 37, -21}, {700, 500, -80, 64}, {1200, 600, 63, -64},
		{2800, 900, 5, -6}, {3700, 900, -128, 127},
	};
	const struct dc_seg *g = &segs[ARRAY_SIZE(segs) - 1U];
	static int8_t items[2 * 900];
	const uint64_t i0 = (1ULL << 36) + 4321U;
	const double gain = 64.0;

	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		setup(channels[i], 8, 1, 0, (float)gain);
		memset(&zc, 0, sizeof(zc));
		for (size_t s = 0; s < ARRAY_SIZE(segs); s++) {
			for (uint32_t j = 0; j < segs[s].pairs; j++) {
				items[2 * j] = segs[s].i;
				items[2 * j + 1] = segs[s].q;
			}
			zassert_true(esdr_pfb_push(&pfb, i0 + segs[s].off, items, segs[s].pairs, 0,
						   segs[s].i, segs[s].q, dc_frame, &zc) >= 0);
		}
		zassert_true(zc.complete > 0U && zc.complete < zc.frames);
		zassert_true(zc.worst <= 1e-6 * gain * 512.0, "M %u: output %g at the DC",
			     channels[i], zc.worst);

		/* The control: items still hold the last segment's. */
		esdr_pfb_reset(&pfb);
		memset(&nc, 0, sizeof(nc));
		nc.want_r = gain * g->i;
		nc.want_i = gain * g->q;
		zassert_true(esdr_pfb_push(&pfb, i0, items, g->pairs, 0, 0, 0, dc_frame, &nc) > 0);
		zassert_true(nc.complete > 0U);
		zassert_true(nc.err0 <= 1e-4 * hypot(nc.want_r, nc.want_i),
			     "M %u: channel 0 off by %g", channels[i], nc.err0);
		printk("M %2u: items at the DC: %u frames (%u complete), largest output %g; "
		       "told no DC, channel 0 within %.1e of gain dc\n", channels[i], zc.frames,
		       zc.complete, zc.worst, nc.err0 / hypot(nc.want_r, nc.want_i));
	}
}

/*
 * The C6's items (esp_sdr_gapless.c): a 10-bit raw value shifted right, a floor, saturated to
 * int8 (sat_lut; pack_top8 keeps the top 8 bits at shift 2, the same).
 */
static int8_t c6_item(int32_t raw, unsigned int shift)
{
	return (int8_t)CLAMP(raw >> shift, -128, 127);
}

/*
 * A dither of 16 pairs, the receiver's noise in small: each half holds -4 .. -1 and 1 .. 4 (zero
 * mean, every residue mod 4 alike), the second the first reversed (no first moment). Through the
 * C6's quantizer and taken as the middle of the step, any 16 in a row less the DC sum to zero.
 */
static const int8_t dith_i[16] = {3, -1, 4, -3, 1, -4, 2, -2, -2, 2, -4, 1, -3, 4, -1, 3};
static const int8_t dith_q[16] = {-2, 4, -4, 1, -3, 3, -1, 2, 2, -1, 3, -3, 1, -4, 4, -2};

/* Offset and pairs (multiples of 16), shift, DC of I and Q; shift 0 keeps 4 off the rails. */
static const struct dq_seg {
	uint32_t off, pairs, shift;
	int8_t dc_i, dc_q;
} dq_segs[] = {
	{0, 1600, 0, 37, -21}, {1600, 1200, 2, -128, 127}, {2800, 1600, 1, 63, -64},
	{4496, 1600, 2, 5, -6}, {8096, 1600, 1, 127, -128}, {9696, 1600, 0, -100, 99},
};

struct dq_ctx {
	uint64_t i0;
	unsigned int complete, inside[3];
	/* |y_0| on complete frames: the largest, and by shift on those inside one segment. */
	double worst, worst_in[3];
};

static struct dq_ctx qc;

/* Shift of the segment that holds pairs o - L + 1 .. o, -1 if no one segment does. */
static int dq_shift(uint32_t o, uint32_t l)
{
	for (size_t s = 0; s < ARRAY_SIZE(dq_segs); s++) {
		const struct dq_seg *g = &dq_segs[s];

		if (o >= g->off && o < g->off + g->pairs) {
			return o + 1U >= g->off + l ? (int)g->shift : -1;
		}
	}
	return -1;
}

static void dq_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	struct dq_ctx *c = user;
	const double y = hypot((double)iq[0], (double)iq[1]);
	int s;

	if ((flags & ESDR_PFB_COMPLETE) == 0U) {
		return;
	}
	s = dq_shift((uint32_t)(m * pfb.d - c->i0), pfb.l);
	c->complete++;
	c->worst = fmax(c->worst, y);
	if (s >= 0) {
		c->inside[s]++;
		c->worst_in[s] = fmax(c->worst_in[s], y);
	}
}

/*
 * Raw pairs at the segment's DC plus the dither, quantized as the C6 does (shifts 0 to 2, I and
 * Q apart, the DC changing at every segment; contiguous segments, a gap that zero-fills and one
 * that restarts): channel 0, where the DC lands, is zero within 1e-2 of a raw unit on every
 * complete frame inside one segment, within 0.1 where the window spans two. Taken as
 * (item << shift) - dc, the floor would leave -(2^s - 1) / 2 on I and Q: |y_0| 0.71 and 2.1.
 */
ZTEST(esdr_pfb, test_dc_floor)
{
	static int8_t items[2 * 1600];
	const double gain = 64.0;

	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		setup(channels[i], 8, 1, 0, (float)gain);
		memset(&qc, 0, sizeof(qc));
		qc.i0 = (1ULL << 36) + 4321U;
		for (size_t s = 0; s < ARRAY_SIZE(dq_segs); s++) {
			const struct dq_seg *g = &dq_segs[s];

			for (uint32_t j = 0; j < g->pairs; j++) {
				items[2 * j] = c6_item(g->dc_i + dith_i[j % 16U], g->shift);
				items[2 * j + 1] = c6_item(g->dc_q + dith_q[j % 16U], g->shift);
			}
			zassert_true(esdr_pfb_push(&pfb, qc.i0 + g->off, items, g->pairs, g->shift,
						   g->dc_i, g->dc_q, dq_frame, &qc) >= 0);
		}
		printk("M %2u: C6 items at the DC, %u complete frames: channel 0 at most %.1e, "
		       "%.1e, %.1e of a raw unit inside segments of shift 0, 1, 2, %.1e on any\n",
		       channels[i], qc.complete, qc.worst_in[0] / gain, qc.worst_in[1] / gain,
		       qc.worst_in[2] / gain, qc.worst / gain);
		for (unsigned int s = 0; s < 3U; s++) {
			zassert_true(qc.inside[s] > 0U, "M %u: none inside shift %u", channels[i], s);
			zassert_true(qc.worst_in[s] <= 1e-2 * gain, "M %u shift %u: channel 0 %g",
				     channels[i], s, qc.worst_in[s] / gain);
		}
		zassert_true(qc.worst <= 0.1 * gain, "M %u: channel 0 %g", channels[i],
			     qc.worst / gain);
	}
}

ZTEST(esdr_pfb, test_edges)
{
	static int8_t z[64];
	unsigned int n = 0;

	setup(16, 8, 1, 0, 0.0f);
	zassert_equal(esdr_pfb_push(&pfb, 1000, z, 0, 0, 0, 0, NULL, NULL), 0, "no pairs");
	zassert_equal(esdr_pfb_push(&pfb, 1000, z, 32, 16, 0, 0, NULL, NULL), -EINVAL, "shift");
	/* 1000 .. 1031: frames at 1000, 1008, 1016, 1024. */
	zassert_equal(esdr_pfb_push(&pfb, 1000, z, 32, 0, 0, 0, NULL, NULL), 4);
	zassert_equal(esdr_pfb_push(&pfb, 1031, z, 8, 0, 0, 0, NULL, NULL), -ERANGE, "overlap");
	/* After a reset, any index starts a run: 7 .. 38 holds 8 .. 32. */
	esdr_pfb_reset(&pfb);
	zassert_equal(esdr_pfb_push(&pfb, 7, z, 32, 0, 0, 0, NULL, NULL), 4);
	/* From index 0: frame 0 has a window of zeros before the stream. */
	esdr_pfb_reset(&pfb);
	zassert_equal(esdr_pfb_push(&pfb, 0, z, 1, 0, 0, 0, NULL, NULL), 1);
	n = 0;
	for (unsigned int i = 0; i <= 160U; i++) {
		struct esdr_pfb_cfg cfg = {.channels = (uint16_t)i};

		n += esdr_pfb_mem_size(&cfg) != 0U;
	}
	zassert_equal(n, ARRAY_SIZE(channels), "supported M");
}

ZTEST(esdr_pfb, test_init_rejects)
{
	struct esdr_pfb_cfg cfg = {.channels = 64};
	size_t need = esdr_pfb_mem_size(&cfg);

	zassert_true(need > 0U);
	zassert_equal(esdr_pfb_init(&pfb, &cfg, mem, need - 32U), -ENOMEM, "short");
	zassert_equal(esdr_pfb_init(&pfb, &cfg, mem + 2, sizeof(mem) - 2U), -EINVAL, "aligned");
	zassert_ok(esdr_pfb_init(&pfb, &cfg, mem + 4, need), "4-byte aligned");
	cfg.channels = 24;
	zassert_equal(esdr_pfb_init(&pfb, &cfg, mem, sizeof(mem)), -EINVAL, "M 24");
	cfg.channels = 128;
	zassert_equal(esdr_pfb_mem_size(&cfg), 0U, "M 128");
	cfg.channels = 64;
	cfg.taps = 6;
	zassert_equal(esdr_pfb_mem_size(&cfg), 0U, "T 6");
	cfg.taps = 20;
	zassert_equal(esdr_pfb_mem_size(&cfg), 0U, "T 20");
	cfg.taps = 8;
	cfg.chunk = 4U * 64U - 1U;
	zassert_equal(esdr_pfb_mem_size(&cfg), 0U, "chunk");
	cfg.chunk = 0;
	cfg.gain = -1.0f;
	zassert_equal(esdr_pfb_mem_size(&cfg), 0U, "gain");

	struct esdr_det_cfg dcfg = {.channels = 64, .block = 1};

	zassert_equal(esdr_det_mem_size(&dcfg), 0U, "block 1");
	dcfg.block = 0;
	dcfg.channels = 42;
	zassert_equal(esdr_det_mem_size(&dcfg), 0U, "M 42");
	dcfg.channels = 64;
	zassert_equal(esdr_det_init(&det, &dcfg, dmem, 64), -ENOMEM, "short");
}

/* ---- tones ---- */

static float tone_buf[2 * 3000];

struct tone_ctx {
	unsigned int k, ref_k, frames;
	/* Frame m's expected channel k output: amp exp(j (phase0 + tau num m / den)). */
	double amp, ph0;
	uint32_t num, den;
	double worst, leak;
};

static struct tone_ctx tc;

static void tone_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	struct tone_ctx *c = user;

	if ((flags & ESDR_PFB_COMPLETE) == 0U) {
		return;
	}
	double a = c->ph0 + ESP_SDR_TAU * (double)((c->num * (m % c->den)) % c->den) / c->den;
	double er = (double)iq[2 * c->k] - c->amp * cos(a);
	double ei = (double)iq[2 * c->k + 1] - c->amp * sin(a);

	c->worst = fmax(c->worst, sqrt(er * er + ei * ei));
	for (unsigned int o = 0; o < pfb.m; o++) {
		if (o != c->k) {
			c->leak = fmax(c->leak, hypot((double)iq[2 * o], (double)iq[2 * o + 1]));
		}
	}
	c->frames++;
}

/*
 * A full scale tone at (k + off / 10) fs / M, phase @p ph at index 0, over segments with a short
 * and a long gap: channel k gives gain A H(off / (10 M)) exp(j (ph + tau (off / 10) m / 2)) on
 * every complete frame.
 */
static void tone_case(uint16_t m, unsigned int k, unsigned int off, double ph)
{
	static const uint32_t seg_off[] = {0, 1050, 2100, 7000, 9000};
	const double amp = 512.0, gain = 3.0;
	const uint64_t i0 = (1ULL << 35) + 777U;
	/* f = (10 k + off) / (10 M), exactly: its phase at index i from (10 k + off) i mod 10 M. */
	const uint64_t num = 10U * k + off, den = 10U * m;
	double hr = 0.0, hi = 0.0;

	setup(m, 8, 1, 0, (float)gain);
	for (unsigned int n = 0; n < pfb.l; n++) {
		double a = -ESP_SDR_TAU * (double)(off * n % den) / den;

		hr += tapd(&pfb, n) * cos(a);
		hi += tapd(&pfb, n) * sin(a);
	}
	memset(&tc, 0, sizeof(tc));
	tc.k = k;
	tc.amp = amp * hypot(hr, hi);
	tc.ph0 = ph + atan2(hi, hr);
	/* (off / (10 M)) m D = off m / 20. */
	tc.num = off;
	tc.den = 20U;
	for (size_t s = 0; s < ARRAY_SIZE(seg_off); s++) {
		uint64_t first = i0 + seg_off[s];

		for (unsigned int j = 0; j < 1000U; j++) {
			double a = ph + ESP_SDR_TAU * (double)(num * ((first + j) % den) % den) /
					   den;

			tone_buf[2 * j] = (float)(amp * cos(a));
			tone_buf[2 * j + 1] = (float)(amp * sin(a));
		}
		zassert_true(esdr_pfb_push_f32(&pfb, first, tone_buf, 1000, tone_frame, &tc) > 0);
	}
	zassert_true(tc.frames > 0U);
	if (off == 0U) {
		zassert_within(tc.amp, gain * amp, 1e-3, "centre gain");
		/* Every other channel is at least fs / M away: the stop band. */
		zassert_true(tc.leak <= gain * amp * 1e-3, "M %u k %u: leak %.1f dB", m, k,
			     20.0 * log10(tc.leak / (gain * amp)));
	}
	zassert_true(tc.worst <= 1e-4 * gain * amp, "M %u k %u off %u: error %g", m, k, off,
		     tc.worst);
}

ZTEST(esdr_pfb, test_tones)
{
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		const uint16_t m = channels[i];
		const unsigned int ks[] = {
			0, 1, m / 2U - 1U, m / 2U, m / 2U + 1U, m - 1U, 3U * m / 4U,
		};

		for (size_t j = 0; j < ARRAY_SIZE(ks); j++) {
			tone_case(m, ks[j], 0, 0.7 + j);
		}
		/* Off centre: the phase still follows the absolute index across both gaps. */
		tone_case(m, 3, 3, 1.1);
		tone_case(m, m - 2U, 7, -0.4);
		printk("M %2u: centre tones at gain A, others below -60 dB; offset tones coherent "
		       "across gaps\n", m);
	}
}

/* ---- detector ---- */

#define DET_M      64U
#define DET_B      16U
#define DET_HANG   4U
#define DET_SIGMA  8.0
#define DET_FRAMES 2000U

/* Bursts: channel, first and last pair (offsets), tone amplitude. */
static const struct burst {
	unsigned int chan;
	uint32_t from, to;
} bursts[] = {
	{5, 12000, 16000}, {50, 20000, 26000}, {5, 30000, 31000}, {20, 42000, 42800},
};

/* Runs of the stream (offsets): one long, then two after gaps, like kept windows. */
static const uint32_t det_runs[][2] = {{0, 36000}, {41000, 46000}, {49000, 51400}};

static float det_in[2 * 2400];

struct det_rec {
	uint64_t m0;
	/* Recorded outputs of the burst channels per frame, and the frames seen. */
	float y[DET_FRAMES][3][2];
	bool seen[DET_FRAMES];
	/* Per channel: opens, closes, the next frame it may send, open and close blocks. */
	unsigned int opens[DET_M], closes[DET_M];
	uint64_t next[DET_M];
	uint64_t open_m[DET_M][4], close_m[DET_M][4];
	unsigned int taken, splits;
};

static struct det_rec dr;

static int rec_index(unsigned int chan)
{
	return chan == 5U ? 0 : chan == 50U ? 1 : chan == 20U ? 2 : -1;
}

static void det_event(const struct esdr_det *d, const struct esdr_det_ev *ev, void *user)
{
	struct det_rec *r = user;
	float iq[2 * 2 * DET_B];
	uint32_t pos = 0, flags;
	uint64_t m0;
	unsigned int n, c = ev->chan;
	int ri = rec_index(c);

	if ((ev->flags & ESDR_DET_OPEN) != 0U) {
		zassert_true(r->opens[c] < 4U);
		r->open_m[c][r->opens[c]++] = d->slot[ev->first].m;
	}
	while ((n = esdr_det_take(d, ev, &pos, iq, 2U * DET_B, &m0, &flags)) > 0U) {
		zassert_true(m0 >= r->next[c], "channel %u sent frame %llu twice", c,
			     (unsigned long long)m0);
		for (unsigned int i = 0; i < n; i++) {
			uint64_t f = m0 + i - r->m0;

			zassert_true(f < DET_FRAMES && r->seen[f]);
			if (ri >= 0) {
				zassert_true(iq[2 * i] == r->y[f][ri][0] &&
					     iq[2 * i + 1] == r->y[f][ri][1],
					     "sample of frame %llu", (unsigned long long)(m0 + i));
			}
		}
		r->next[c] = m0 + n;
		r->taken += n;
		r->splits++;
	}
	zassert_equal(pos, ev->n);
	if ((ev->flags & ESDR_DET_CLOSE) != 0U) {
		zassert_true(r->closes[c] < 4U);
		r->close_m[c][r->closes[c]++] = d->slot[ev->first].m + ev->n;
	}
}

static void det_frame_cb(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	struct det_rec *r = user;
	uint64_t f;

	if (r->m0 == 0U) {
		r->m0 = m;
	}
	f = m - r->m0;
	zassert_true(f < DET_FRAMES);
	r->seen[f] = true;
	r->y[f][0][0] = iq[2 * 5];
	r->y[f][0][1] = iq[2 * 5 + 1];
	r->y[f][1][0] = iq[2 * 50];
	r->y[f][1][1] = iq[2 * 50 + 1];
	r->y[f][2][0] = iq[2 * 20];
	r->y[f][2][1] = iq[2 * 20 + 1];
	esdr_det_frame(&det, m, iq, flags, det_event, r);
}

ZTEST(esdr_pfb, test_detector)
{
	struct esdr_det_cfg dcfg = {.channels = DET_M, .block = DET_B, .hang = DET_HANG};
	const uint64_t i0 = 3200000000ULL;
	const double amp = 16.0;
	double h2 = 0.0, floor_lo = 1e30, floor_hi = 0.0;

	zassert_true(esdr_det_mem_size(&dcfg) <= sizeof(dmem));
	zassert_ok(esdr_det_init(&det, &dcfg, dmem, sizeof(dmem)));
	setup(DET_M, 8, ESDR_DET_FRAMES(DET_B), 0, 0.0f);
	memset(&dr, 0, sizeof(dr));
	test_rng_seed(99);
	for (size_t q = 0; q < ARRAY_SIZE(det_runs); q++) {
		for (uint32_t off = det_runs[q][0]; off < det_runs[q][1]; off += 2400U) {
			uint32_t n = MIN(2400U, det_runs[q][1] - off);

			for (uint32_t j = 0; j < n; j++) {
				uint64_t i = i0 + off + j;
				double re = test_rng_gauss() * DET_SIGMA;
				double im = test_rng_gauss() * DET_SIGMA;

				for (size_t b = 0; b < ARRAY_SIZE(bursts); b++) {
					if (off + j < bursts[b].from || off + j >= bursts[b].to) {
						continue;
					}
					/* (chan + 0.1) fs / M: well inside the channel. */
					uint64_t num = 10U * bursts[b].chan + 1U, den = 10U * DET_M;
					double a = ESP_SDR_TAU *
						   (double)(num * (i % den) % den) / den;

					re += amp * cos(a);
					im += amp * sin(a);
				}
				det_in[2 * j] = (float)re;
				det_in[2 * j + 1] = (float)im;
			}
			zassert_true(esdr_pfb_push_f32(&pfb, i0 + off, det_in, n, det_frame_cb,
						       &dr) >= 0);
		}
	}
	esdr_det_flush(&det, det_event, &dr);

	/* The burst channels opened and closed once per burst, nothing else did. */
	for (unsigned int c = 0; c < DET_M; c++) {
		unsigned int want = c == 5U ? 2U : (c == 50U || c == 20U) ? 1U : 0U;

		zassert_equal(dr.opens[c], want, "channel %u opened %u times", c, dr.opens[c]);
		zassert_equal(dr.closes[c], want, "channel %u closed %u times", c, dr.closes[c]);
	}
	/*
	 * Timing: a channel opens in the block where its burst begins or the next, the frames start
	 * with the block before; it closes hang blocks after the last block the burst (and the
	 * filter's tail, L / D frames) reaches.
	 */
	unsigned int seen5 = 0;

	for (size_t b = 0; b < ARRAY_SIZE(bursts); b++) {
		const unsigned int c = bursts[b].chan, i = c == 5U ? seen5++ : 0U;
		const uint64_t from = (i0 + bursts[b].from) / (DET_M / 2U);
		const uint64_t to = (i0 + bursts[b].to) / (DET_M / 2U) + 2U * 8U;
		const uint64_t blk = from - from % DET_B;
		int64_t late = (int64_t)dr.open_m[c][i] + DET_B - (int64_t)blk;
		uint64_t close = dr.close_m[c][i];

		printk("burst in channel %2u: frames %llu .. %llu; sent %lld frames before its "
		       "block, closed %lld frames after its tail\n", c,
		       (unsigned long long)(from - dr.m0), (unsigned long long)(to - dr.m0),
		       (long long)(blk - dr.open_m[c][i]), (long long)(close - to));
		zassert_true(late == 0 || late == (int64_t)DET_B,
			     "channel %u opened %lld frames late", c, late);
		zassert_true(close >= to + (DET_HANG - 1U) * DET_B &&
			     close <= to + (DET_HANG + 2U) * DET_B,
			     "channel %u closed at %llu, tail %llu", c,
			     (unsigned long long)close, (unsigned long long)to);
	}
	/* Quiet channels' floors: the channel's noise power, 2 sigma^2 sum h^2. */
	for (unsigned int n = 0; n < pfb.l; n++) {
		h2 += tapd(&pfb, n) * tapd(&pfb, n);
	}
	for (unsigned int c = 0; c < DET_M; c++) {
		if (c == 4U || c == 5U || c == 6U || c == 19U || c == 20U || c == 21U ||
		    c == 49U || c == 50U || c == 51U) {
			continue;
		}
		double ratio = (double)det.floor[c] / (2.0 * DET_SIGMA * DET_SIGMA * h2);

		floor_lo = fmin(floor_lo, ratio);
		floor_hi = fmax(floor_hi, ratio);
	}
	printk("detector: %u samples taken in %u runs; quiet floors %.2f .. %.2f of the noise\n",
	       dr.taken, dr.splits, floor_lo, floor_hi);
	zassert_true(floor_lo > 0.7 && floor_hi < 1.3, "floors %.2f .. %.2f", floor_lo, floor_hi);
}

/* ---- benchmark ---- */

static volatile float bench_sink;

static void bench_ev(const struct esdr_det *d, const struct esdr_det_ev *ev, void *user)
{
	ARG_UNUSED(d);
	ARG_UNUSED(user);
	bench_sink += ev->power;
}

static void bench_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	if (user != NULL) {
		esdr_det_frame(&det, m, iq, flags, bench_ev, NULL);
	} else {
		bench_sink += iq[2];
	}
	ARG_UNUSED(m);
}

ZTEST(esdr_pfb, test_bench)
{
	static int8_t seg[2 * 2000];
	const unsigned int segs = 1000;

	for (size_t i = 0; i < sizeof(seg); i++) {
		seg[i] = (int8_t)(test_rng_u32() >> 24);
	}
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		struct esdr_det_cfg dcfg = {.channels = channels[i]};
		double ns[2], cyc[2];

		zassert_ok(esdr_det_init(&det, &dcfg, dmem, sizeof(dmem)));
		for (int w = 0; w < 2; w++) {
			uint64_t idx = 1000000, t0, c0;

			setup(channels[i], 8, ESDR_DET_FRAMES(16), 0, 0.0f);
			t0 = test_host_ns();
			c0 = test_host_cycles();
			/* Gapless-like: segments of 2000 pairs, every fifth after a gap. */
			for (unsigned int s = 0; s < segs; s++) {
				esdr_pfb_push(&pfb, idx, seg, 2000, 1, 5, -3, bench_frame,
					      w ? &det : NULL);
				idx += 2000U + (s % 5U == 4U ? 5000U : 0U);
			}
			ns[w] = (double)(test_host_ns() - t0) / (2000.0 * segs);
			cyc[w] = (double)(test_host_cycles() - c0) / (2000.0 * segs);
		}
		printk("bench M %2u T 8: %.1f ns/pair (%.1f host cycles), with the detector %.1f "
		       "ns/pair (%.1f cycles)\n", channels[i], ns[0], cyc[0], ns[1], cyc[1]);
	}
}

ZTEST_SUITE(esdr_pfb, NULL, NULL, NULL, NULL, NULL);
