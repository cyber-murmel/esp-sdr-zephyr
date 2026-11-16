/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Differential tests: the production demodulator gfsk_demod against the frozen reference
 * gfsk_ref on identical input cut into identical pushes. Every symbol (t, bit, pwr) must agree,
 * and after each push the level, the noise floor, the horizon and the sample counter. Streams:
 * BLE bursts from -40 to -1 dBFS in noise, the same under Wi-Fi like bursts, BR bursts, and
 * extremes (silence, DC, a full scale square wave, GFSK near full scale); always emitting and
 * behind a squelch; pushes of 256, of random sizes, of 1 and of 3000 samples; with and without
 * gaps and a squelch change in the middle of the stream.
 *
 * The streams are quantized as the sample ring stores them (int8, x >> 6): the production
 * demodulator reads those planes with GFSK_IN_HIST samples of history before each push, the
 * reference the same samples as (x8 << 6) + 32, each at the middle of its step.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "gen.h"
#include "gfsk_demod.h"
#include "gfsk_ref.h"

#define DN       24576U
#define MAX_PUSH 3000U

static int16_t di[DN], dq[DN];
/* Stream sample k at GFSK_IN_HIST + k, the history before it. */
static int8_t di8[GFSK_IN_HIST + DN];
static int8_t dq8[GFSK_IN_HIST + DN];
static struct gfsk_sym sa[GFSK_OUT_CAP(MAX_PUSH)];
static struct gfsk_sym sb[GFSK_REF_OUT_CAP(MAX_PUSH)];
static uint8_t bits[2][1600];

static struct gfsk_demod pd;
static struct gfsk_ref rd;

/* ---- the production demodulator, behind its current input contract ---------------------- */

static void prod_init(enum gfsk_modulation mod, uint8_t squelch)
{
	gfsk_demod_init(&pd, mod);
	if (squelch != 0U) {
		gfsk_demod_set_squelch(&pd, squelch, 16, false);
	}
}

static size_t prod_push(uint32_t at, size_t n)
{
	return gfsk_demod_push(&pd, &di8[GFSK_IN_HIST + at], &dq8[GFSK_IN_HIST + at], n, sa,
			       ARRAY_SIZE(sa));
}

/* di, dq as the ring stores them: the int8 planes, and the reference's (x8 << 6) + 32 in di, dq. */
static int8_t q8(int16_t x)
{
	int32_t v = x >> 6;

	return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
}

static void quantize(void)
{
	for (uint32_t k = 0; k < DN; k++) {
		di8[GFSK_IN_HIST + k] = q8(di[k]);
		dq8[GFSK_IN_HIST + k] = q8(dq[k]);
		di[k] = (int16_t)(di8[GFSK_IN_HIST + k] * 64 + 32);
		dq[k] = (int16_t)(dq8[GFSK_IN_HIST + k] * 64 + 32);
	}
}

/* ---- the comparison ---------------------------------------------------------------------- */

enum sched {
	SCHED_256,
	SCHED_RANDOM,
	SCHED_ONE,
	SCHED_3000,
};

static const char *const sched_name[] = {"256", "random", "1", "3000"};

struct diff {
	uint32_t syms, pushes, bad;
	uint32_t bad_push, bad_sym;
	const char *what;
};

static void differ(struct diff *r, uint32_t sym, const char *what)
{
	if (r->bad++ == 0U) {
		r->bad_push = r->pushes;
		r->bad_sym = sym;
		r->what = what;
	}
}

static void cmp_syms(struct diff *r, size_t na, size_t nb)
{
	if (na != nb) {
		differ(r, (uint32_t)MIN(na, nb), "symbol count");
	}
	for (size_t k = 0; k < MIN(na, nb); k++) {
		if (sa[k].t != sb[k].t) {
			differ(r, (uint32_t)k, "t");
		} else if (sa[k].bit != sb[k].bit) {
			differ(r, (uint32_t)k, "bit");
		} else if (sa[k].pwr != sb[k].pwr) {
			differ(r, (uint32_t)k, "pwr");
		}
	}
	r->syms += (uint32_t)nb;
}

static void cmp_state(struct diff *r)
{
	if (gfsk_demod_level(&pd) != gfsk_ref_level(&rd)) {
		differ(r, 0, "level");
	}
	if (gfsk_demod_noise_floor(&pd) != gfsk_ref_noise_floor(&rd)) {
		differ(r, 0, "noise floor");
	}
	if (gfsk_demod_horizon(&pd) != gfsk_ref_horizon(&rd)) {
		differ(r, 0, "horizon");
	}
	if (gfsk_demod_now(&pd) != gfsk_ref_now(&rd)) {
		differ(r, 0, "now");
	}
	if (gfsk_demod_cfo_hz(&pd) != gfsk_ref_cfo_hz(&rd)) {
		differ(r, 0, "cfo");
	}
}

static size_t next_push(enum sched sc, size_t left)
{
	size_t n;

	switch (sc) {
	case SCHED_RANDOM:
		n = 1U + test_rng_u32() % 256U;
		break;
	case SCHED_ONE:
		n = 1;
		break;
	case SCHED_3000:
		n = MAX_PUSH;
		break;
	default:
		n = 256;
		break;
	}
	return MIN(n, left);
}

/*
 * One run over di/dq[0, n): @p squelch 0 always emits. With @p events, three gaps (the samples
 * are skipped) and a squelch change half way.
 */
static void run(enum gfsk_modulation mod, uint8_t squelch, enum sched sc, bool events, size_t n,
		const char *stream)
{
	static const uint32_t gap_at[3] = {5000, 11000, 17500};
	static const uint32_t gap_len[3] = {37, 900, 2048};
	struct diff r;
	unsigned int g = 0;
	bool changed = false;

	memset(&r, 0, sizeof(r));
	prod_init(mod, squelch);
	gfsk_ref_init(&rd, mod);
	if (squelch != 0U) {
		gfsk_ref_set_squelch(&rd, squelch, 16, false);
	}
	for (uint32_t at = 0; at < n;) {
		size_t len = next_push(sc, n - at);
		size_t na, nb;

		if (events && g < ARRAY_SIZE(gap_at) && at >= gap_at[g]) {
			uint32_t lost = MIN(gap_len[g], (uint32_t)(n - at));

			gfsk_demod_gap(&pd, lost);
			gfsk_ref_gap(&rd, lost);
			at += lost;
			g++;
			continue;
		}
		if (events && !changed && at >= n / 2U) {
			uint8_t sq2 = squelch != 0U ? (uint8_t)(squelch + 20U) : 0U;

			gfsk_demod_set_squelch(&pd, sq2, 8, false);
			gfsk_ref_set_squelch(&rd, sq2, 8, false);
			changed = true;
		}
		na = prod_push(at, len);
		nb = gfsk_ref_push(&rd, &di[at], &dq[at], len, sb, ARRAY_SIZE(sb));
		cmp_syms(&r, na, nb);
		cmp_state(&r);
		r.pushes++;
		at += (uint32_t)len;
	}
	cmp_syms(&r, gfsk_demod_flush(&pd, sa, ARRAY_SIZE(sa)),
		 gfsk_ref_flush(&rd, sb, ARRAY_SIZE(sb)));
	zassert_equal(r.bad, 0U,
		      "%s, %s, squelch %u, pushes %s%s: %u mismatches in %u symbols, first in push %u, "
		      "symbol %u (%s)", stream, mod == GFSK_BLE ? "BLE" : "BR", squelch,
		      sched_name[sc], events ? ", gaps" : "", r.bad, r.syms, r.bad_push, r.bad_sym,
		      r.what);
	zassert_true(r.syms > 0U || squelch != 0U, "%s: no symbols", stream);
}

/* The squelch of the modes: the floor the reference tracks always emitting, plus 7 dB. */
static uint8_t floor_squelch(enum gfsk_modulation mod, size_t n)
{
	gfsk_ref_init(&rd, mod);
	for (uint32_t at = 0; at < n; at += 256U) {
		(void)gfsk_ref_push(&rd, &di[at], &dq[at], MIN(256U, n - at), sb, ARRAY_SIZE(sb));
	}
	return (uint8_t)MIN(255U, gfsk_ref_noise_floor(&rd) + 14U);
}

static void run_all(enum gfsk_modulation mod, size_t n, const char *stream)
{
	uint8_t sq;

	quantize();
	sq = floor_squelch(mod, n);

	for (unsigned int s = 0; s <= SCHED_3000; s++) {
		for (unsigned int e = 0; e < 2U; e++) {
			run(mod, 0, (enum sched)s, e != 0U, n, stream);
			run(mod, sq, (enum sched)s, e != 0U, n, stream);
		}
	}
}

/* ---- streams ----------------------------------------------------------------------------- */

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_rng_seed(0x5851f42d4c957f2dULL);
}

ZTEST_SUITE(ubertooth_demod_diff, NULL, NULL, before_each, NULL, NULL);

/* Six BLE packets from -40 to -1 dBFS, two of them 40 kHz off, in noise of -40 dBFS. */
static void ble_stream(void)
{
	static const double lvl[6] = {-40, -30, -20, -12, -6, -1};
	struct burst b[6];

	gauss_init(0.5);
	for (unsigned int k = 0; k < 6U; k++) {
		ble_bits(bits[k & 1U], 376, 0x8e89bed6U);
		b[k] = (struct burst){.start = 300U + 900U * k, .bits = bits[k & 1U], .nsym = 376,
				      .h = 0.5, .dbfs = lvl[k],
				      .cfo_hz = k == 2U ? 40000.0 : (k == 4U ? -40000.0 : 0.0)};
	}
	gen(di, dq, DN, b, ARRAY_SIZE(b), -40);
}

ZTEST(ubertooth_demod_diff, test_diff_ble_bursts)
{
	ble_stream();
	run_all(GFSK_BLE, DN, "BLE bursts");
}

ZTEST(ubertooth_demod_diff, test_diff_ble_under_wifi)
{
	ble_stream();
	add_bursts(di, dq, DN, -25, 600, 200);
	run_all(GFSK_BLE, DN, "BLE under Wi-Fi");
}

ZTEST(ubertooth_demod_diff, test_diff_br_bursts)
{
	struct burst b[4];

	gauss_init(0.5);
	for (unsigned int k = 0; k < 4U; k++) {
		for (unsigned int j = 0; j < 1600U; j++) {
			bits[k & 1U][j] = (uint8_t)(test_rng_u32() >> 31);
		}
		b[k] = (struct burst){.start = 400U + 1400U * k, .bits = bits[k & 1U], .nsym = 1200,
				      .h = 0.32, .dbfs = -30.0 + 8.0 * k, .cfo_hz = 20000.0 * k};
	}
	gen(di, dq, DN, b, ARRAY_SIZE(b), -40);
	run_all(GFSK_BR, DN, "BR bursts");
}

/*
 * Forty random streams: four to eight BLE or BR bursts of random level (-45 to 0 dBFS), carrier
 * offset and length, noise from -55 to -35 dBFS, Wi-Fi like bursts half of the time; each run
 * always emitting or behind the squelch, with random pushes, gaps and a squelch change. Some
 * 10^6 samples per mode, for the gate of each optimisation step.
 */
ZTEST(ubertooth_demod_diff, test_diff_random_long)
{
	static struct burst b[8];

	gauss_init(0.5);
	for (unsigned int round = 0; round < 40U; round++) {
		enum gfsk_modulation mod = (round & 3U) == 3U ? GFSK_BR : GFSK_BLE;
		unsigned int nb = 4U + test_rng_u32() % 5U;
		double noise = -55.0 + (double)(test_rng_u32() % 21U);
		uint32_t at = 200;

		for (unsigned int k = 0; k < 2U; k++) {
			for (unsigned int j = 0; j < ARRAY_SIZE(bits[k]); j++) {
				bits[k][j] = (uint8_t)(test_rng_u32() >> 31);
			}
		}
		ble_bits(bits[0], 40, 0x8e89bed6U);
		for (unsigned int k = 0; k < nb; k++) {
			size_t nsym = 100U + test_rng_u32() % 500U;

			b[k] = (struct burst){.start = at, .bits = bits[k & 1U], .nsym = nsym,
					      .h = mod == GFSK_BLE ? 0.5 : 0.32,
					      .dbfs = -45.0 + (double)(test_rng_u32() % 46U),
					      .cfo_hz = (double)((int32_t)(test_rng_u32() % 160001U) - 80000)};
			/* Starts and lengths in samples (4 a symbol): 20 to 300 us apart. */
			at += (uint32_t)(4U * nsym + 80U + test_rng_u32() % 1200U);
			if (at + 4U * 600U > DN) {
				nb = k + 1U;
				break;
			}
		}
		gen(di, dq, DN, b, nb, noise);
		if ((test_rng_u32() & 1U) != 0U) {
			add_bursts(di, dq, DN, -45.0 + (double)(test_rng_u32() % 30U),
				   200U + test_rng_u32() % 1500U, 50U + test_rng_u32() % 400U);
		}
		quantize();
		run(mod, (test_rng_u32() & 1U) != 0U ? floor_squelch(mod, DN) : 0U, SCHED_RANDOM,
		    (test_rng_u32() & 1U) != 0U, DN, "random long");
	}
}

/* Silence, DC, a full scale square wave, then GFSK at -0.5 dBFS without noise. */
ZTEST(ubertooth_demod_diff, test_diff_extremes)
{
	struct burst b = {.start = 4500, .bits = bits[0], .nsym = 1200, .h = 0.5, .dbfs = -0.5};

	gauss_init(0.5);
	ble_bits(bits[0], 1200, 0x8e89bed6U);
	gen(di, dq, DN, &b, 1, -200);
	for (uint32_t k = 0; k < 6000U; k++) {
		if (k < 2000U) {
			di[k] = 0;
			dq[k] = 0;
		} else if (k < 4000U) {
			di[k] = 4096;
			dq[k] = -4096;
		} else if (k < 4400U) {
			di[k] = (k & 2U) != 0U ? 8128 : -8192;
			dq[k] = (k & 1U) != 0U ? -8192 : 8128;
		}
	}
	run_all(GFSK_BLE, DN, "extremes");
}

/* FNV-1a over the symbols of a run (t, bit, pwr), and their count. */
struct sym_sum {
	uint32_t n, h;
};

static void sum_syms(struct sym_sum *s, const struct gfsk_sym *y, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		const uint32_t v[3] = {y[k].t, y[k].bit, y[k].pwr};

		for (unsigned int j = 0; j < ARRAY_SIZE(v); j++) {
			s->h = (s->h ^ v[j]) * 16777619U;
		}
	}
	s->n += (uint32_t)n;
}

/*
 * The symbols of di/dq behind the squelch @p sq, pushed in 256s after a reset: with @p pre 0 the
 * baseline, the squelch set again after the reset (set while always emitting, the default of
 * gfsk_demod_init(), it starts open; after a reset closed); otherwise the reset after the first
 * @p pre samples, as a retune in the modes does.
 */
static struct sym_sum prod_sum(uint8_t sq, uint32_t pre)
{
	struct sym_sum s = {0, 2166136261U};

	prod_init(GFSK_BLE, sq);
	for (uint32_t at = 0; at < pre; at += 256U) {
		(void)prod_push(at, MIN(256U, pre - at));
	}
	gfsk_demod_reset(&pd);
	if (pre == 0U) {
		gfsk_demod_set_squelch(&pd, sq, 16, false);
	}
	for (uint32_t at = 0; at < DN; at += 256U) {
		sum_syms(&s, sa, prod_push(at, MIN(256U, DN - at)));
	}
	sum_syms(&s, sa, gfsk_demod_flush(&pd, sa, ARRAY_SIZE(sa)));
	return s;
}

static struct sym_sum ref_sum(uint8_t sq, uint32_t pre)
{
	struct sym_sum s = {0, 2166136261U};

	gfsk_ref_init(&rd, GFSK_BLE);
	gfsk_ref_set_squelch(&rd, sq, 16, false);
	for (uint32_t at = 0; at < pre; at += 256U) {
		(void)gfsk_ref_push(&rd, &di[at], &dq[at], MIN(256U, pre - at), sb, ARRAY_SIZE(sb));
	}
	gfsk_ref_reset(&rd);
	if (pre == 0U) {
		gfsk_ref_set_squelch(&rd, sq, 16, false);
	}
	for (uint32_t at = 0; at < DN; at += 256U) {
		sum_syms(&s, sb,
			 gfsk_ref_push(&rd, &di[at], &dq[at], MIN(256U, DN - at), sb, ARRAY_SIZE(sb)));
	}
	sum_syms(&s, sb, gfsk_ref_flush(&rd, sb, ARRAY_SIZE(sb)));
	return s;
}

/*
 * A reset (every retune in the modes) starts over with the configuration kept, the squelch's
 * gate included. The gate was zeroed with the stream's state, so after the first
 * retune of a mode every group passed it: each burst's edge fell on the first replayed group
 * and its sampling phase and levels were seeded from the noise before it (2026-11-12, found
 * in the follow mode: 9 % of the connection events missed, 1 % with the gate kept).
 */
ZTEST(ubertooth_demod_diff, test_reset_keeps_squelch)
{
	struct sym_sum a, b;
	uint8_t sq;

	ble_stream();
	quantize();
	sq = floor_squelch(GFSK_BLE, DN);
	a = prod_sum(sq, 0);
	b = prod_sum(sq, 3000);
	zassert_true(a.n > 0U, "no symbols");
	zassert_true(a.n == b.n && a.h == b.h, "production: %u and %u symbols, not the same", a.n,
		     b.n);
	a = ref_sum(sq, 0);
	b = ref_sum(sq, 3000);
	zassert_true(a.n == b.n && a.h == b.h, "reference: %u and %u symbols, not the same", a.n,
		     b.n);
}
