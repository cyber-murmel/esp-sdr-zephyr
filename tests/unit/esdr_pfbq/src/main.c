/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/common/src/esdr_pfbq.c: the fixed-point bank against the float reference
 * (tests/common/src/esdr_pfb_ref.c) over gapped streams of every M with a shift and DC per
 * segment (the same frames, the same completeness, values within a few output units); tone gain;
 * flushes; the detector on quads against the float detector on synthetic bursts; where the build
 * has the PIE kernels, those against the C kernels bit for bit; and a benchmark, printed only.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "esdr_pfb_ref.h"
#include "esdr_pfbq.h"
#include "test_rng.h"

static const uint16_t channels[] = {16, 32, 40, 64, 80};

/*
 * The largest banks fit (M 80, T 12, 9 quads; the float bank with one frame, or M 64 with 33);
 * the PIE-against-C test runs its second fixed bank in the float bank's room.
 */
static uint8_t fmem[40000] __aligned(16);
static uint8_t qmem[40000] __aligned(16);
static uint8_t dmem[12000] __aligned(16);
static uint8_t dqmem[20000] __aligned(16);
static struct esdr_pfb pfb;
static struct esdr_pfbq pq, pq2;
static struct esdr_det det;
static struct esdr_detq detq;

static uint64_t cycles(void)
{
#if defined(CONFIG_ARCH_POSIX)
	uint64_t test_host_cycles(void);

	return test_host_cycles();
#else
	uint32_t v;

	__asm__ volatile("csrr %0, mcycle" : "=r"(v));
	return v;
#endif
}

/* esdr_pfbq.c's profile: its clock, and the cycles per part. */
extern uint64_t esdr_pfbq_prof[5];

uint64_t esdr_pfb_now(void)
{
	return cycles();
}

static void setup_f(uint16_t m, uint16_t t, uint16_t frames, float gain)
{
	struct esdr_pfb_cfg cfg = {.channels = m, .taps = t, .frames = frames, .gain = gain};

	zassert_true(esdr_pfb_mem_size(&cfg) <= sizeof(fmem), "fmem %u",
		     (unsigned int)esdr_pfb_mem_size(&cfg));
	zassert_ok(esdr_pfb_init(&pfb, &cfg, fmem, sizeof(fmem)), "init M %u T %u", m, t);
}

static void setup_q(struct esdr_pfbq *p, uint8_t *mem, size_t len, uint16_t m, uint16_t t,
		    uint16_t quads, uint32_t chunk, float gain)
{
	struct esdr_pfbq_cfg cfg = {
		.channels = m, .taps = t, .quads = quads, .chunk = chunk, .gain = gain,
	};

	zassert_true(esdr_pfbq_mem_size(&cfg) <= len, "qmem %u",
		     (unsigned int)esdr_pfbq_mem_size(&cfg));
	zassert_ok(esdr_pfbq_init(p, &cfg, mem, len), "init M %u T %u", m, t);
}

/* ---- against the float bank ---- */

/*
 * Fixed quads by m0 / 4 (a push's and the quad pending before it); float frames the fixed bank
 * has not sent yet. The target has less room: shorter pushes and streams.
 */
#if defined(CONFIG_ARCH_POSIX)
#define QR       64U
#define PUSH_MAX 1500U
#define SPAN_MAX 70000U
#define KINDS    12U
#else
#define QR       24U
#define PUSH_MAX 600U
#define SPAN_MAX 24000U
#define KINDS    20U
#endif
#define PENDS 16U

struct vs_ctx {
	uint16_t m;
	struct {
		uint64_t m0;
		uint8_t valid, complete, matched;
		bool used;
		int16_t iq[4U * 2U * ESDR_PFB_M_MAX];
	} qr[QR];
	struct {
		uint64_t m;
		uint32_t flags;
		float iq[2U * ESDR_PFB_M_MAX];
	} pend[PENDS];
	unsigned int npend;
	unsigned int ffloat, ffixed, matched, complete;
	double worst, sq;
	uint64_t nsq;
};

static struct vs_ctx vs;

static void vs_retry(struct vs_ctx *c);

static void vs_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	struct vs_ctx *c = user;
	unsigned int s = (unsigned int)((m0 / 4U) % QR);

	zassert_equal(m0 % 4U, 0U);
	/* A run's last quad (flushed in this push) meets its float frames before its slot goes. */
	if (c->qr[s].used && c->qr[s].matched != c->qr[s].valid) {
		vs_retry(c);
	}
	zassert_true(!c->qr[s].used || c->qr[s].matched == c->qr[s].valid,
		     "quad %llu overwritten with frames unmatched", (unsigned long long)c->qr[s].m0);
	zassert_equal(complete & ~valid, 0U);
	c->qr[s].m0 = m0;
	c->qr[s].valid = valid;
	c->qr[s].complete = complete;
	c->qr[s].matched = 0;
	c->qr[s].used = true;
	memcpy(c->qr[s].iq, iq, 4U * 2U * c->m * sizeof(int16_t));
	c->ffixed += (valid & 1U) + ((valid >> 1) & 1U) + ((valid >> 2) & 1U) + ((valid >> 3) & 1U);
}

/* Float frame @p m against the fixed one, if that came: true when compared. */
static bool vs_match(struct vs_ctx *c, uint64_t m, const float *iq, uint32_t flags)
{
	unsigned int s = (unsigned int)((m / 4U) % QR), q = (unsigned int)(m % 4U);

	if (!c->qr[s].used || c->qr[s].m0 != m - q) {
		return false;
	}
	zassert_true((c->qr[s].valid & (1U << q)) != 0U, "frame %llu not valid in the fixed bank",
		     (unsigned long long)m);
	zassert_true((c->qr[s].matched & (1U << q)) == 0U);
	zassert_equal((c->qr[s].complete >> q) & 1U, (flags & ESDR_PFB_COMPLETE) != 0U ? 1U : 0U,
		      "frame %llu completeness", (unsigned long long)m);
	c->qr[s].matched |= (uint8_t)(1U << q);
	for (unsigned int k = 0; k < c->m; k++) {
		for (unsigned int i = 0; i < 2U; i++) {
			double e = fabs((double)c->qr[s].iq[8U * k + 2U * q + i] - (double)iq[2U * k + i]);

			c->worst = fmax(c->worst, e);
			c->sq += e * e;
			c->nsq++;
		}
	}
	c->matched++;
	c->complete += (flags & ESDR_PFB_COMPLETE) != 0U;
	return true;
}

static void vs_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	struct vs_ctx *c = user;

	c->ffloat++;
	if (!vs_match(c, m, iq, flags)) {
		zassert_true(c->npend < PENDS, "float frames ahead of the fixed bank");
		c->pend[c->npend].m = m;
		c->pend[c->npend].flags = flags;
		memcpy(c->pend[c->npend].iq, iq, 2U * c->m * sizeof(float));
		c->npend++;
	}
}

/* Pending float frames the fixed bank has sent since. */
static void vs_retry(struct vs_ctx *c)
{
	unsigned int k = 0;

	for (unsigned int i = 0; i < c->npend; i++) {
		if (!vs_match(c, c->pend[i].m, c->pend[i].iq, c->pend[i].flags)) {
			c->pend[k++] = c->pend[i];
		}
	}
	c->npend = k;
}

static int8_t items[2U * SPAN_MAX];

/*
 * One case: segments of random lengths separated by gaps of 0, under L (zeros), L - 1, L and
 * more (new runs), random items with random shifts and DC, the same pushes to both banks.
 */
static void vs_case(uint16_t m, uint16_t t, uint32_t chunk, float gain, uint32_t span,
		    uint64_t seed)
{
	const uint64_t i0 = (1ULL << 40) + 12345U;
	uint32_t off = 0, segs = 0;

	span = MIN(span, SPAN_MAX);
	test_rng_seed(seed);
	setup_f(m, t, 1, gain);
	setup_q(&pq, qmem, sizeof(qmem), m, t, 1, chunk, gain);
	memset(&vs, 0, sizeof(vs));
	vs.m = m;
	for (uint32_t i = 0; i < 2U * span; i++) {
		items[i] = (int8_t)(test_rng_u32() >> 24);
	}
	while (true) {
		uint32_t pairs = 1U + test_rng_u32() % PUSH_MAX, shift = test_rng_u32() % 3U;
		/* Gap kinds: none (KINDS - 5 of KINDS: runs outgrow L), shorter than L (zeros), L - 1, L,
		 * L + 1, longer (new runs).
		 */
		uint32_t kind = test_rng_u32() % KINDS, gap;
		int32_t dci = (int32_t)(test_rng_u32() >> 24) - 128;
		int32_t dcq = (int32_t)(test_rng_u32() >> 24) - 128;
		int ret;

		kind = kind < KINDS - 5U ? 0U : kind - (KINDS - 6U);
		gap = kind == 0U ? 0U : kind == 1U ? 1U + test_rng_u32() % (pq.l - 1U) :
			kind == 2U ? pq.l - 1U : kind == 3U ? pq.l : kind == 4U ? pq.l + 1U :
			pq.l + test_rng_u32() % 3000U;
		if (segs > 0U) {
			off += gap;
		}
		if (off + pairs > span) {
			break;
		}
		ret = esdr_pfbq_push(&pq, i0 + off, &items[2U * off], pairs, shift, dci, dcq,
				     vs_quad, &vs);
		zassert_true(ret >= 0, "fixed push %d", ret);
		vs_retry(&vs);
		ret = esdr_pfb_push(&pfb, i0 + off, &items[2U * off], pairs, shift, dci, dcq,
				    vs_frame, &vs);
		zassert_true(ret >= 0, "float push %d", ret);
		off += pairs;
		segs++;
	}
	zassert_true(esdr_pfbq_flush(&pq, vs_quad, &vs) >= 0);
	vs_retry(&vs);
	zassert_equal(vs.npend, 0U, "%u float frames the fixed bank never sent", vs.npend);
	zassert_equal(vs.ffixed, vs.ffloat, "fixed frames %u, float %u", vs.ffixed, vs.ffloat);
	zassert_equal(vs.matched, vs.ffloat);
	printk("M %2u T %2u gain %4.1f qt %2u chunk %4u: %u segments, %u frames (%u complete), "
	       "error worst %.0f rms %.2f output units\n", m, t, (double)gain, esdr_pfbq_qt(&pq),
	       chunk, segs, vs.matched, vs.complete, vs.worst, sqrt(vs.sq / (double)vs.nsq));
	/*
	 * Each branch sum is rounded once (0.29 units rms), the DFT adds M of them: the error grows
	 * as the root of M; the FFT's floored twiddles add a little.
	 */
	zassert_true(vs.worst <= 6.0 * sqrt(m), "worst %.0f", vs.worst);
	zassert_true(vs.matched > 50U && vs.complete > 10U);
}

ZTEST(esdr_pfbq, test_vs_float)
{
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		vs_case(channels[i], 8, 0, 32.0f, 30000, 11U + i);
	}
	vs_case(16, 4, 64, 8.0f, 20000, 21U);
	vs_case(32, 12, 0, 60.0f, 20000, 22U);
	vs_case(40, 12, 160, 16.0f, 20000, 23U);
	vs_case(64, 4, 256, 32.0f, 20000, 24U);
	vs_case(80, 12, 320, 40.0f, 30000, 25U);
}

/* ---- tones ---- */

struct tone_ctx {
	unsigned int k, n;
	double sum, worst_other;
};

static struct tone_ctx tc;

static void tone_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete,
		      void *user)
{
	struct tone_ctx *c = user;

	ARG_UNUSED(m0);
	ARG_UNUSED(valid);
	for (unsigned int q = 0; q < 4U; q++) {
		if ((complete & (1U << q)) == 0U) {
			continue;
		}
		for (unsigned int k = 0; k < pq.m; k++) {
			double a = hypot(iq[8U * k + 2U * q], iq[8U * k + 2U * q + 1U]);

			if (k == c->k) {
				c->sum += a;
				c->n++;
			} else if (k != 0U) {
				c->worst_other = fmax(c->worst_other, a);
			}
		}
	}
}

/* A carrier of 120 items (480 raw at shift 2) at channel k's centre: gain times that. */
ZTEST(esdr_pfbq, test_tones)
{
	int8_t *tone = items;
	const float gain = 32.0f;

	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		const uint16_t m = channels[i];
		const unsigned int ks[] = {1, m / 4U, m / 2U + 1U, m - 3U};

		for (size_t j = 0; j < ARRAY_SIZE(ks); j++) {
			double want, got;

			for (uint32_t n = 0; n < 8000U; n++) {
				double a = ESP_SDR_TAU * (double)((ks[j] * n) % m) / m;

				tone[2U * n] = (int8_t)lround(120.0 * cos(a));
				tone[2U * n + 1U] = (int8_t)lround(120.0 * sin(a));
			}
			setup_q(&pq, qmem, sizeof(qmem), m, 8, 1, 0, gain);
			memset(&tc, 0, sizeof(tc));
			tc.k = ks[j];
			/* Index a multiple of M: the tone's phase is 0 at index 0 of the axis. */
			zassert_true(esdr_pfbq_push(&pq, 1000U * m, tone, 8000U, 2, 0, 0, tone_quad,
						    &tc) > 0);
			want = (double)gain * 480.0;
			got = tc.sum / tc.n;
			zassert_true(tc.n > 100U);
			zassert_within(got, want, want * 0.005, "M %u k %u: %.1f, want %.1f", m,
				       ks[j], got, want);
			/* Neighbours see the edge of the passband; the rest are 60 dB down. */
			zassert_true(tc.worst_other < want * 0.6, "M %u k %u: other %.1f", m, ks[j],
				     tc.worst_other);
		}
		printk("M %2u: centre tones at gain A within 0.5 %%\n", m);
	}
}

/* ---- flush ---- */

struct fl_ctx {
	unsigned int quads;
	uint64_t last_m0;
	uint8_t last_valid;
};

static struct fl_ctx flc;

static void fl_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	struct fl_ctx *c = user;

	ARG_UNUSED(iq);
	ARG_UNUSED(complete);
	zassert_true(c->quads == 0U || m0 > c->last_m0, "quad %llu after %llu",
		     (unsigned long long)m0, (unsigned long long)c->last_m0);
	c->quads++;
	c->last_m0 = m0;
	c->last_valid = valid;
}

/* A flush sends the quad the input stopped in; the run goes on after it. */
ZTEST(esdr_pfbq, test_flush)
{
	const uint16_t m = 40, d = 20;
	const uint64_t i0 = 400000U;

	memset(items, 3, 2U * 4000U);
	setup_q(&pq, qmem, sizeof(qmem), m, 8, 1, 0, 32.0f);
	memset(&flc, 0, sizeof(flc));
	/* Pairs up to frame i0 / d + 41 (newest pair i0 + 41 d): its quad has frames 40 .. 43. */
	zassert_true(esdr_pfbq_push(&pq, i0, items, 41U * d + 1U, 0, 0, 0, fl_quad, &flc) >= 0);
	zassert_equal(flc.last_m0, i0 / d + 36U);
	zassert_equal(esdr_pfbq_flush(&pq, fl_quad, &flc), 1);
	zassert_equal(flc.last_m0, i0 / d + 40U);
	zassert_equal(flc.last_valid, 0x3U, "valid %x", flc.last_valid);
	/* Nothing more to flush. */
	zassert_equal(esdr_pfbq_flush(&pq, fl_quad, &flc), 0);
	/* The run goes on: the next quad is 44 .. 47, all valid. */
	zassert_true(esdr_pfbq_push(&pq, i0 + 41U * d + 1U, items, 8U * d, 0, 0, 0, fl_quad,
				    &flc) > 0);
	zassert_equal(flc.last_m0, i0 / d + 44U);
	zassert_equal(flc.last_valid, 0xFU);
	zassert_equal(flc.quads, 12U, "quads %u", flc.quads);
	/*
	 * A long gap flushes the quad (48 .. 49 valid) and starts a run: pairs from 5 after frame
	 * 5040 (in quad 5040 .. 5043, its first frame not valid), up to frame 5048.
	 */
	zassert_true(esdr_pfbq_push(&pq, i0 + 100000U + 5U, items, 8U * d, 0, 0, 0, fl_quad,
				    &flc) > 0);
	zassert_equal(flc.quads, 15U, "quads %u", flc.quads);
	zassert_equal(flc.last_m0, (i0 + 100000U) / d + 4U);
}

/* ---- PIE against C ---- */

/* One push's quads of each bank (pushes of up to 200 pairs: 8 quads of M 16 at most). */
#define PC_QUADS 10U

struct pc_ctx {
	unsigned int n, total;
	uint64_t m0[PC_QUADS];
	uint8_t valid[PC_QUADS];
	int16_t iq[PC_QUADS][8U * ESDR_PFB_M_MAX];
};

static struct pc_ctx pca, pcb;

static void pc_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	struct pc_ctx *c = user;

	ARG_UNUSED(complete);
	zassert_true(c->n < PC_QUADS);
	c->m0[c->n] = m0;
	c->valid[c->n] = valid;
	memcpy(c->iq[c->n], iq, 8U * pq.m * sizeof(int16_t));
	c->n++;
	c->total++;
}

static void pc_compare(uint16_t m)
{
	zassert_equal(pca.n, pcb.n);
	for (unsigned int q = 0; q < pca.n; q++) {
		zassert_equal(pca.m0[q], pcb.m0[q]);
		zassert_equal(pca.valid[q], pcb.valid[q]);
		for (unsigned int j = 0; j < 8U * m; j++) {
			zassert_equal(pca.iq[q][j], pcb.iq[q][j],
				      "M %u quad %llu channel %u lane %u: PIE %d, C %d", m,
				      (unsigned long long)pca.m0[q], j / 8U, j % 8U, pca.iq[q][j],
				      pcb.iq[q][j]);
		}
	}
	pca.n = 0;
	pcb.n = 0;
}

/* The PIE kernels give the C ones' values exactly. */
ZTEST(esdr_pfbq, test_pie_vs_c)
{
	if (!esdr_pfbq_pie()) {
		ztest_test_skip();
	}
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		const uint16_t m = channels[i];
		uint32_t off = 0;

		test_rng_seed(31U + i);
		for (uint32_t j = 0; j < 2U * SPAN_MAX; j++) {
			items[j] = (int8_t)(test_rng_u32() >> 24);
		}
		setup_q(&pq, qmem, sizeof(qmem), m, 8, 1, 0, 32.0f);
		setup_q(&pq2, fmem, sizeof(fmem), m, 8, 1, 0, 32.0f);
		zassert_ok(esdr_pfbq_kernels(&pq2, false));
		memset(&pca, 0, sizeof(pca));
		memset(&pcb, 0, sizeof(pcb));
		while (off < SPAN_MAX - 1000U) {
			uint32_t pairs = 1U + test_rng_u32() % 200U, shift = test_rng_u32() % 3U;
			int32_t dci = (int32_t)(test_rng_u32() >> 24) - 128;
			int32_t dcq = (int32_t)(test_rng_u32() >> 24) - 128;

			zassert_true(esdr_pfbq_push(&pq, 7777777U + off, &items[2U * off], pairs, shift,
						    dci, dcq, pc_quad, &pca) >= 0);
			zassert_true(esdr_pfbq_push(&pq2, 7777777U + off, &items[2U * off], pairs,
						    shift, dci, dcq, pc_quad, &pcb) >= 0);
			pc_compare(m);
			off += pairs + (test_rng_u32() % 4U == 0U ? test_rng_u32() % 400U : 0U);
		}
		printk("M %2u: %u quads, PIE and C equal\n", m, pca.total);
		zassert_true(pca.total > 50U);
	}
}

/* ---- detector ---- */

#define DET_M      64U
#define DET_B      16U
#define DET_HANG   4U
#define DET_SIGMA  8.0
#define DET_AMP    16.0

/* Bursts: channel, first and last pair (offsets). */
static const struct burst {
	unsigned int chan;
	uint32_t from, to;
} bursts[] = {
	{5, 12000, 16000}, {50, 20000, 26000}, {5, 30000, 31000}, {20, 42000, 42800},
};

/* Runs of the stream (offsets): one long, then two after gaps, like kept windows. */
static const uint32_t det_runs[][2] = {{0, 36000}, {41000, 46000}, {49000, 51400}};

struct det_side {
	unsigned int opens[DET_M], closes[DET_M], taken;
	uint64_t open_m[DET_M][4], close_m[DET_M][4], next[DET_M];
	/* Blocks esdr_detq_block() handed out. */
	uint32_t blocks;
};

static struct det_side dsf, dsq;

static void detf_event(const struct esdr_det *d, const struct esdr_det_ev *ev, void *user)
{
	struct det_side *r = user;
	float iq[2U * 2U * DET_B];
	uint32_t pos = 0, flags;
	uint64_t m0;
	unsigned int n, c = ev->chan;

	if ((ev->flags & ESDR_DET_OPEN) != 0U) {
		zassert_true(r->opens[c] < 4U);
		r->open_m[c][r->opens[c]++] = d->slot[ev->first].m;
	}
	while ((n = esdr_det_take(d, ev, &pos, iq, 2U * DET_B, &m0, &flags)) > 0U) {
		r->taken += n;
	}
	if ((ev->flags & ESDR_DET_CLOSE) != 0U) {
		zassert_true(r->closes[c] < 4U);
		r->close_m[c][r->closes[c]++] = d->slot[ev->first].m + ev->n;
	}
}

static void detf_frame(uint64_t m, const float *iq, uint32_t flags, void *user)
{
	esdr_det_frame(&det, m, iq, flags, detf_event, user);
}

static void detq_event(const struct esdr_detq *d, const struct esdr_detq_ev *ev, void *user)
{
	struct det_side *r = user;
	int16_t iq[2U * 2U * DET_B];
	uint32_t pos = 0, flags;
	uint64_t m0;
	unsigned int n, c = ev->chan;

	if ((ev->flags & ESDR_DET_OPEN) != 0U) {
		zassert_true(r->opens[c] < 4U);
		r->open_m[c][r->opens[c]++] = d->slot[ev->first].m0;
	}
	while ((n = esdr_detq_take(d, ev, &pos, iq, 2U * DET_B, &m0, &flags)) > 0U) {
		zassert_true(m0 >= r->next[c], "channel %u sent frame %llu twice", c,
			     (unsigned long long)m0);
		/* The samples are the quads' own. */
		for (unsigned int i = 0; i < n; i++) {
			uint64_t m = m0 + i;
			bool found = false;

			for (uint32_t s = 0; s < ev->n && !found; s++) {
				const struct esdr_detq_slot *sl = &d->slot[(ev->first + s) % d->nslot];

				if (m >= sl->m0 && m < sl->m0 + 4U) {
					unsigned int q = (unsigned int)(m - sl->m0);

					zassert_true(sl->iq[8U * c + 2U * q] == iq[2U * i] &&
						     sl->iq[8U * c + 2U * q + 1U] == iq[2U * i + 1U]);
					found = true;
				}
			}
			zassert_true(found);
		}
		r->next[c] = m0 + n;
		r->taken += n;
	}
	zassert_equal(pos, 4U * ev->n);
	/* The block being decided: the event's last quads, as they are. */
	if (esdr_detq_block(d, c, iq)) {
		for (unsigned int i = 0; i < DET_B; i++) {
			const struct esdr_detq_slot *sl =
				&d->slot[(ev->first + ev->n - DET_B / 4U + i / 4U) % d->nslot];

			zassert_equal(sl->m0 + i % 4U, d->cur_start + i);
			zassert_true(sl->iq[8U * c + 2U * (i % 4U)] == iq[2U * i] &&
				     sl->iq[8U * c + 2U * (i % 4U) + 1U] == iq[2U * i + 1U]);
		}
		r->blocks++;
	}
	if ((ev->flags & ESDR_DET_CLOSE) != 0U) {
		zassert_true(r->closes[c] < 4U);
		r->close_m[c][r->closes[c]++] = d->slot[ev->first].m0 + 4U * ev->n;
	}
}

static void detq_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete,
		      void *user)
{
	esdr_detq_quad(&detq, m0, iq, valid, complete, detq_event, user);
}

/* The same bursts through both banks and detectors: the same channels open and close. */
ZTEST(esdr_pfbq, test_detector)
{
	struct esdr_det_cfg dcfg = {.channels = DET_M, .block = DET_B, .hang = DET_HANG};
	struct esdr_detq_cfg qcfg = {.channels = DET_M, .block = DET_B, .hang = DET_HANG};
	const uint64_t i0 = 3200000000ULL;
	static int8_t in[2U * 2400U];

	zassert_true(esdr_det_mem_size(&dcfg) <= sizeof(dmem));
	zassert_true(esdr_detq_mem_size(&qcfg) <= sizeof(dqmem));
	zassert_ok(esdr_det_init(&det, &dcfg, dmem, sizeof(dmem)));
	zassert_ok(esdr_detq_init(&detq, &qcfg, dqmem, sizeof(dqmem)));
	setup_f(DET_M, 8, ESDR_DET_FRAMES(DET_B), 32.0f);
	setup_q(&pq, qmem, sizeof(qmem), DET_M, 8, ESDR_DETQ_QUADS(DET_B), 0, 32.0f);
	memset(&dsf, 0, sizeof(dsf));
	memset(&dsq, 0, sizeof(dsq));
	test_rng_seed(99);
	for (size_t r = 0; r < ARRAY_SIZE(det_runs); r++) {
		for (uint32_t off = det_runs[r][0]; off < det_runs[r][1]; off += 2400U) {
			uint32_t n = MIN(2400U, det_runs[r][1] - off);

			for (uint32_t j = 0; j < n; j++) {
				uint64_t i = i0 + off + j;
				double re = test_rng_gauss() * DET_SIGMA;
				double im = test_rng_gauss() * DET_SIGMA;

				for (size_t b = 0; b < ARRAY_SIZE(bursts); b++) {
					if (off + j < bursts[b].from || off + j >= bursts[b].to) {
						continue;
					}
					uint64_t num = 10U * bursts[b].chan + 1U, den = 10U * DET_M;
					double a = ESP_SDR_TAU *
						   (double)(num * (i % den) % den) / den;

					re += DET_AMP * cos(a);
					im += DET_AMP * sin(a);
				}
				in[2U * j] = (int8_t)CLAMP(lround(re), -128, 127);
				in[2U * j + 1U] = (int8_t)CLAMP(lround(im), -128, 127);
			}
			zassert_true(esdr_pfb_push(&pfb, i0 + off, in, n, 0, 0, 0, detf_frame,
						   &dsf) >= 0);
			zassert_true(esdr_pfbq_push(&pq, i0 + off, in, n, 0, 0, 0, detq_quad,
						    &dsq) >= 0);
		}
	}
	(void)esdr_pfbq_flush(&pq, detq_quad, &dsq);
	esdr_det_flush(&det, detf_event, &dsf);
	esdr_detq_flush(&detq, detq_event, &dsq);

	for (unsigned int c = 0; c < DET_M; c++) {
		unsigned int want = c == 5U ? 2U : (c == 50U || c == 20U) ? 1U : 0U;

		zassert_equal(dsq.opens[c], want, "channel %u opened %u times", c, dsq.opens[c]);
		zassert_equal(dsq.closes[c], want, "channel %u closed %u times", c, dsq.closes[c]);
		zassert_equal(dsq.opens[c], dsf.opens[c]);
		for (unsigned int i = 0; i < dsq.opens[c]; i++) {
			zassert_equal(dsq.open_m[c][i], dsf.open_m[c][i], "channel %u open %u", c, i);
			zassert_equal(dsq.close_m[c][i], dsf.close_m[c][i], "channel %u close %u", c,
				      i);
		}
	}
	printk("detector: %u samples taken (float %u), %u blocks looked at; opens and closes as "
	       "the float detector's\n", dsq.taken, dsf.taken, dsq.blocks);
	zassert_equal(dsq.taken, dsf.taken);
	zassert_true(dsq.blocks > 0U);
}

/* ---- detector on a pruned stream ---- */

struct snap_ctx {
	unsigned int opens, closes;
	uint64_t open_m[4];
};

static struct snap_ctx sc;

static void snap_event(const struct esdr_detq *d, const struct esdr_detq_ev *ev, void *user)
{
	struct snap_ctx *c = user;

	if (ev->chan != 5U) {
		return;
	}
	if ((ev->flags & ESDR_DET_OPEN) != 0U && c->opens < 4U) {
		c->open_m[c->opens++] = d->cur_start;
	}
	c->closes += (ev->flags & ESDR_DET_CLOSE) != 0U;
}

static void snap_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete,
		      void *user)
{
	esdr_detq_quad(&detq, m0, iq, valid, complete, snap_event, user);
}

/*
 * A pruned stream brings a channel's carrier first: its floor is learnt from the carrier. Quiet
 * blocks of noise then put it right, and the next burst opens the channel.
 */
ZTEST(esdr_pfbq, test_detector_snap)
{
	struct esdr_detq_cfg qcfg = {.channels = DET_M, .block = DET_B, .hang = DET_HANG};
	const uint64_t i0 = 64000000ULL;
	/* Carrier, quiet, carrier: pair offsets. */
	const uint32_t edge[] = {0, 24000, 30000, 40000};
	static int8_t in[2U * 2000U];
	uint64_t want;

	zassert_ok(esdr_detq_init(&detq, &qcfg, dqmem, sizeof(dqmem)));
	setup_q(&pq, qmem, sizeof(qmem), DET_M, 8, ESDR_DETQ_QUADS(DET_B), 0, 32.0f);
	memset(&sc, 0, sizeof(sc));
	test_rng_seed(7);
	for (uint32_t off = 0; off < edge[3]; off += 2000U) {
		for (uint32_t j = 0; j < 2000U; j++) {
			uint64_t i = i0 + off + j;
			bool on = off + j < edge[1] || off + j >= edge[2];
			double re = test_rng_gauss() * DET_SIGMA, im = test_rng_gauss() * DET_SIGMA;

			if (on) {
				uint64_t den = 10U * DET_M;
				double a = ESP_SDR_TAU * (double)(51U * (i % den) % den) / den;

				re += DET_AMP * 2.0 * cos(a);
				im += DET_AMP * 2.0 * sin(a);
			}
			in[2U * j] = (int8_t)CLAMP(lround(re), -128, 127);
			in[2U * j + 1U] = (int8_t)CLAMP(lround(im), -128, 127);
		}
		zassert_true(esdr_pfbq_push(&pq, i0 + off, in, 2000U, 0, 0, 0, snap_quad, &sc) >= 0);
	}
	(void)esdr_pfbq_flush(&pq, snap_quad, &sc);
	esdr_detq_flush(&detq, snap_event, &sc);
	/* The first carrier was the floor; the second opened, in its first block or the next. */
	want = (i0 + edge[2]) / (DET_M / 2U);
	zassert_equal(sc.opens, 1U, "channel 5 opened %u times", sc.opens);
	zassert_true(sc.open_m[0] + DET_B >= want && sc.open_m[0] <= want + DET_B,
		     "opened at frame %llu, the burst came at %llu",
		     (unsigned long long)sc.open_m[0], (unsigned long long)want);
	printk("detector: a floor learnt from a carrier came right in the quiet blocks; the next "
	       "burst opened %lld frames after its start\n",
	       (long long)sc.open_m[0] - (long long)want);
}

/* ---- benchmark ---- */

static volatile int32_t bench_sink;

static void bench_ev(const struct esdr_detq *d, const struct esdr_detq_ev *ev, void *user)
{
	ARG_UNUSED(d);
	ARG_UNUSED(user);
	bench_sink += (int32_t)ev->power;
}

static void bench_quad(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete,
		       void *user)
{
	if (user != NULL) {
		esdr_detq_quad(&detq, m0, iq, valid, complete, bench_ev, NULL);
	} else {
		bench_sink += iq[2];
	}
}

ZTEST(esdr_pfbq, test_bench)
{
	static int8_t seg[2 * 2000];
	const unsigned int segs = IS_ENABLED(CONFIG_ARCH_POSIX) ? 1000U : 100U;

	for (size_t i = 0; i < sizeof(seg); i++) {
		seg[i] = (int8_t)(test_rng_u32() >> 24);
	}
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		struct esdr_detq_cfg dcfg = {.channels = channels[i]};
		double cyc[2];

		zassert_ok(esdr_detq_init(&detq, &dcfg, dqmem, sizeof(dqmem)));
		for (int w = 0; w < 2; w++) {
			uint64_t idx = 1000000, c0;

			setup_q(&pq, qmem, sizeof(qmem), channels[i], 8, ESDR_DETQ_QUADS(16), 0,
				32.0f);
			memset(esdr_pfbq_prof, 0, sizeof(esdr_pfbq_prof));
			c0 = cycles();
			/* Gapless-like: segments of 2000 pairs, every fifth after a gap. */
			for (unsigned int s = 0; s < segs; s++) {
				esdr_pfbq_push(&pq, idx, seg, 2000, 1, 5, -3, bench_quad,
					       w ? &detq : NULL);
				idx += 2000U + (s % 5U == 4U ? 5000U : 0U);
			}
			cyc[w] = (double)(cycles() - c0) / (2000.0 * segs);
		}
		const double n = 2000.0 * segs;

		printk("bench M %2u T 8 %s: %.1f cycles a pair, with the detector %.1f (branch %.1f, "
		       "FFT %.1f, conversion %.1f, power %.1f, decisions %.1f)\n", channels[i],
		       esdr_pfbq_pie() ? "PIE" : "C", cyc[0], cyc[1], esdr_pfbq_prof[0] / n,
		       esdr_pfbq_prof[1] / n, esdr_pfbq_prof[2] / n, esdr_pfbq_prof[3] / n,
		       esdr_pfbq_prof[4] / n);
	}
}

ZTEST_SUITE(esdr_pfbq, NULL, NULL, NULL, NULL, NULL);
