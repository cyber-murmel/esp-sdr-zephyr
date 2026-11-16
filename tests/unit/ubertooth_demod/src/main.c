/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the receive side of apps/ubertooth below the modes: the GFSK demodulator (the
 * frozen reference gfsk_ref.c; diff.c holds gfsk_demod.c to it bit for bit) on bursts from the
 * suite's generator (gen.h), independent of the app's modulator gfsk_mod.c; the RX_SYMBOLS block
 * builder and the sweep and AFH hop sequence (ubt_br.c); the RSSI conventions and the carrier
 * sense (ubt_rssi.c). Runs on native_sim.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "gen.h"
#include "gfsk_ref.h"
#include "ubt_br.h"
#include "ubt_rssi.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_rng_seed(0x9e3779b97f4a7c15ULL);
	ubt_rssi_set_cal(UBT_RSSI_K_DEFAULT);
}

ZTEST_SUITE(ubertooth_demod, NULL, NULL, before_each, NULL, NULL);

#define NMAX  32768U
#define SYMAX (NMAX / 4U + 256U)

static int16_t si[NMAX], sq[NMAX];
static struct gfsk_sym syms[SYMAX];

/* The whole stream through the demodulator in pieces of up to @p chunk, then flushed. */
static size_t demod_all(struct gfsk_ref *d, size_t n, size_t chunk)
{
	size_t ns = 0;

	for (size_t at = 0; at < n; at += chunk) {
		size_t len = n - at < chunk ? n - at : chunk;

		ns += gfsk_ref_push(d, &si[at], &sq[at], len, &syms[ns], SYMAX - ns);
	}
	ns += gfsk_ref_flush(d, &syms[ns], SYMAX - ns);
	return ns;
}

struct match {
	size_t found, errors;
	uint32_t first_t;
	unsigned int pwr_min, pwr_max;
};

/* Every symbol of the burst against the emitted symbol within half a symbol of its centre. */
static struct match match_burst(const struct burst *b, size_t ns)
{
	struct match m = {0, 0, UINT32_MAX, 255, 0};
	size_t j = 0;

	for (size_t k = 0; k < b->nsym; k++) {
		uint32_t want = (uint32_t)(b->start + 4U * k + 2U);

		while (j < ns && syms[j].t + 2U < want) {
			j++;
		}
		if (j < ns && syms[j].t <= want + 2U) {
			m.found++;
			m.errors += syms[j].bit != b->bits[k] ? 1U : 0U;
			m.first_t = MIN(m.first_t, syms[j].t);
			if (k >= 8U) {
				m.pwr_min = MIN(m.pwr_min, syms[j].pwr);
				m.pwr_max = MAX(m.pwr_max, syms[j].pwr);
			}
		}
	}
	return m;
}

/* ---- demodulator ------------------------------------------------------------------------ */

static uint8_t bits_a[2048];

ZTEST(ubertooth_demod, test_ble_clean)
{
	static struct gfsk_ref d;
	struct burst b = {.start = 400, .bits = bits_a, .nsym = 1200, .h = 0.5, .dbfs = -12};
	struct match m;
	size_t ns;

	gauss_init(0.5);
	ble_bits(bits_a, b.nsym, 0x8e89bed6U);
	gen(si, sq, 6000, &b, 1, -60);
	gfsk_ref_init(&d, GFSK_BLE);
	ns = demod_all(&d, 6000, 256);
	m = match_burst(&b, ns);
	/*
	 * Always emitting: a symbol per period from the first one the filters fill (some 35
	 * samples in) to the end, every one of the burst right.
	 */
	zassert_true(ns >= (6000U - 40U) / 4U, "%zu symbols", ns);
	zassert_equal(m.found, b.nsym, "found %zu", m.found);
	zassert_equal(m.errors, 0U, "%zu errors", m.errors);
	/* -12 dBFS: pwr 2 (dBFS + 100) = 176. */
	zassert_true(m.pwr_min >= 173U && m.pwr_max <= 179U, "pwr %u..%u", m.pwr_min, m.pwr_max);
	/* Inside the burst the phase moves by a sample at most (where it starts it jumps). */
	for (size_t k = 1; k < ns; k++) {
		int32_t step = (int32_t)(syms[k].t - syms[k - 1].t);

		if (syms[k - 1].t > b.start + 16U && syms[k].t < b.start + 4U * b.nsym) {
			zassert_true(step >= 3 && step <= 5, "symbol %zu: step %d", k, step);
		}
	}
}

/* Strong packets (a transmitter next to the board) up to full scale decode as well. */
ZTEST(ubertooth_demod, test_ble_strong)
{
	static struct gfsk_ref d;
	const double levels[3] = {-6, -3, -1};

	gauss_init(0.5);
	for (unsigned int u = 0; u < ARRAY_LEN(levels); u++) {
		struct burst b = {.start = 400, .bits = bits_a, .nsym = 1000, .h = 0.5,
				  .dbfs = levels[u], .cfo_hz = 40000};
		struct match m;
		size_t ns;

		ble_bits(bits_a, b.nsym, 0x8e89bed6U);
		gen(si, sq, 5000, &b, 1, -50);
		gfsk_ref_init(&d, GFSK_BLE);
		ns = demod_all(&d, 5000, 256);
		m = match_burst(&b, ns);
		zassert_equal(m.found, b.nsym, "%g dBFS: found %zu", levels[u], m.found);
		zassert_equal(m.errors, 0U, "%g dBFS: %zu errors", levels[u], m.errors);
		zassert_true(m.pwr_min >= 2U * (unsigned int)(100 + levels[u]) - 3U, "%g dBFS: pwr %u",
			     levels[u], m.pwr_min);
	}
}

ZTEST(ubertooth_demod, test_br_clean)
{
	static struct gfsk_ref d;
	struct burst b = {.start = 300, .bits = bits_a, .nsym = 1000, .h = 0.32, .dbfs = -20};
	struct match m;
	size_t ns;

	gauss_init(0.5);
	for (size_t k = 0; k < b.nsym; k++) {
		bits_a[k] = k < 4U ? (uint8_t)(k & 1U) : (uint8_t)(test_rng_u32() >> 31);
	}
	gen(si, sq, 5000, &b, 1, -60);
	gfsk_ref_init(&d, GFSK_BR);
	ns = demod_all(&d, 5000, 100);
	m = match_burst(&b, ns);
	zassert_equal(m.found, b.nsym, "found %zu", m.found);
	zassert_equal(m.errors, 0U, "%zu errors", m.errors);
	zassert_true(m.pwr_min >= 157U && m.pwr_max <= 163U, "pwr %u..%u", m.pwr_min, m.pwr_max);
}

/*
 * BR in noise behind the modes' squelch (the floor plus 14): 40 bursts at 15 dB (Eb/N0), the
 * preamble alternating into the first sync bit, the rest at random. Every symbol found, some 2 %
 * wrong (64 in 2880 when written).
 */
ZTEST(ubertooth_demod, test_br_noise_behind_squelch)
{
	struct burst b = {.start = 1600, .bits = bits_a, .nsym = 72, .h = 0.32, .dbfs = -20};
	size_t n = b.start + 4U * b.nsym + 200U;
	size_t errors = 0, found = 0;
	struct gfsk_ref d;

	gauss_init(0.5);
	for (unsigned int u = 0; u < 40U; u++) {
		uint8_t s0 = (uint8_t)(test_rng_u32() >> 31);
		struct match m;
		size_t ns = 0;
		uint8_t floor;

		for (size_t k = 0; k < b.nsym; k++) {
			bits_a[k] = k < 5U ? (uint8_t)(s0 ^ ((4U - k) & 1U)) :
					     (uint8_t)(test_rng_u32() >> 31);
		}
		gen(si, sq, n, &b, 1, b.dbfs - 15 + 6);
		gfsk_ref_init(&d, GFSK_BR);
		(void)gfsk_ref_push(&d, si, sq, 1500, syms, SYMAX);
		floor = gfsk_ref_noise_floor(&d);
		gfsk_ref_set_squelch(&d, (uint8_t)(floor + 14U), 16, false);
		for (size_t at = 1500; at < n; at += 256U) {
			ns += gfsk_ref_push(&d, &si[at], &sq[at], MIN(256U, n - at), &syms[ns],
					      SYMAX - ns);
		}
		ns += gfsk_ref_flush(&d, &syms[ns], SYMAX - ns);
		m = match_burst(&b, ns);
		errors += m.errors;
		found += m.found;
	}
	zassert_true(found >= 40U * (b.nsym - 4U), "found %zu", found);
	zassert_true(errors <= 100U, "%zu errors in %zu symbols", errors, found);
}

/*
 * A carrier offset of 60 kHz (the crystals of both ends) and noise 20 dB under the carrier in
 * 1 MHz. The offset is read where the burst ends: in noise the levels drift (always emitting).
 */
ZTEST(ubertooth_demod, test_ble_offset_noise)
{
	static struct gfsk_ref d;
	struct burst b = {.start = 500, .bits = bits_a, .nsym = 2000, .h = 0.5, .dbfs = -20};
	const int32_t cfo[3] = {60000, -60000, 0};
	struct match m;
	size_t ns;

	gauss_init(0.5);
	ble_bits(bits_a, b.nsym, 0x8e89bed6U);
	for (unsigned int u = 0; u < ARRAY_LEN(cfo); u++) {
		size_t n = b.start + 4U * b.nsym;
		int32_t est;

		b.cfo_hz = cfo[u];
		/* Noise power over 4 MHz: 6 dB more than in 1 MHz. */
		gen(si, sq, n, &b, 1, -20 - 20 + 6);
		gfsk_ref_init(&d, GFSK_BLE);
		ns = 0;
		for (size_t at = 0; at < n; at += 256U) {
			ns += gfsk_ref_push(&d, &si[at], &sq[at], MIN(256U, n - at), &syms[ns],
					      SYMAX - ns);
		}
		est = gfsk_ref_cfo_hz(&d);
		ns += gfsk_ref_flush(&d, &syms[ns], SYMAX - ns);
		m = match_burst(&b, ns);
		zassert_true(m.found >= b.nsym - 2U, "cfo %d: found %zu", cfo[u], m.found);
		zassert_true(m.errors <= 4U, "cfo %d: %zu errors of %zu", cfo[u], m.errors, m.found);
		/* Rough: within 25 kHz. */
		zassert_true(est > cfo[u] - 25000 && est < cfo[u] + 25000, "cfo %d reads %d",
			     cfo[u], est);
	}
}

/*
 * The squelch: closed on noise (no symbols), opened by a burst with its preamble intact (the
 * pre-roll is replayed), closed again after the burst.
 */
ZTEST(ubertooth_demod, test_squelch_preamble)
{
	static struct gfsk_ref d;
	struct burst b[2] = {
		{.start = 12000, .bits = bits_a, .nsym = 300, .h = 0.5, .dbfs = -30},
		{.start = 20000, .bits = bits_a + 300, .nsym = 300, .h = 0.5, .dbfs = -30},
	};
	size_t ns, noise_syms = 0, after = 0;
	uint8_t floor;
	struct match m;

	gauss_init(0.5);
	ble_bits(bits_a, 300, 0x8e89bed6U);
	ble_bits(bits_a + 300, 300, 0x12345678U);
	gen(si, sq, 26000, b, 2, -56);
	gfsk_ref_init(&d, GFSK_BLE);
	/* The noise floor first (always emitting), then the squelch 7 dB over it. */
	ns = gfsk_ref_push(&d, si, sq, 4000, syms, SYMAX);
	floor = gfsk_ref_level(&d);
	zassert_true(floor > 60U && floor < 110U, "floor %u", floor);
	gfsk_ref_set_squelch(&d, (uint8_t)(floor + 14U), 16, false);
	ns = 0;
	for (size_t at = 4000; at < 26000; at += 256U) {
		size_t len = MIN(256U, 26000U - at);

		ns += gfsk_ref_push(&d, &si[at], &sq[at], len, &syms[ns], SYMAX - ns);
	}
	ns += gfsk_ref_flush(&d, &syms[ns], SYMAX - ns);
	for (size_t k = 0; k < ns; k++) {
		/*
		 * Before 4000: the first push's look-ahead, emitted always; then the open squelch
		 * holds for its 16 symbols and the look-ahead: 100 symbols are plenty.
		 */
		if (syms[k].t >= 4400U && syms[k].t < 11900U) {
			noise_syms++;
		}
		if (syms[k].t > 12000U + 4U * 300U + 200U && syms[k].t < 19900U) {
			after++;
		}
	}
	zassert_equal(noise_syms, 0U, "%zu symbols from noise", noise_syms);
	zassert_true(after < 40U, "%zu symbols after the first burst", after);
	for (size_t u = 0; u < 2U; u++) {
		m = match_burst(&b[u], ns);
		zassert_true(m.found >= b[u].nsym - 4U, "burst %zu: found %zu", u, m.found);
		/* The first preamble bits may go to the acquisition; the access address may not. */
		zassert_true(m.first_t <= b[u].start + 4U * 4U, "burst %zu: first %u", u, m.first_t);
		zassert_true(m.errors <= 3U, "burst %zu: %zu errors", u, m.errors);
	}
}

/*
 * A strong burst starting inside a weak one, the squelch open and every group above the gate: the
 * step up (a group 6 dB over the smoothed power before it) restarts the gate's run, so the strong
 * burst is acquired anew, at its own timing (half a symbol off the weak one's) and carrier
 * offset. Before, a run that had already passed its second group never looked at the step again
 * and the strong burst was sliced with the weak one's phase and levels; behind a squelch that
 * noise had opened that lost 1 to 7 % of the packets at the board's levels (tools/gfsk_sim.c).
 */
ZTEST(ubertooth_demod, test_step_up_in_open_squelch)
{
	static uint8_t bits_b[400];
	struct burst b[2] = {
		{.start = 2000, .bits = bits_a, .nsym = 300, .h = 0.5, .dbfs = -32, .cfo_hz = 30000},
		{.start = 2000 + 4 * 150 + 2, .bits = bits_b, .nsym = 400, .h = 0.5, .dbfs = -6,
		 .cfo_hz = -40000},
	};
	struct gfsk_ref d;
	static uint8_t got[SYMAX];
	size_t ns, ng = 0, k;
	uint8_t floor;
	bool found = false;

	gauss_init(0.5);
	ble_bits(bits_a, 300, 0x8e89bed6U);
	ble_bits(bits_b, 400, 0x12345678U);
	gen(si, sq, 6000, b, 2, -50);
	gfsk_ref_init(&d, GFSK_BLE);
	(void)gfsk_ref_push(&d, si, sq, 1500, syms, SYMAX);
	floor = gfsk_ref_noise_floor(&d);
	gfsk_ref_set_squelch(&d, (uint8_t)(floor + 14U), 16, false);
	ns = 0;
	for (size_t at = 1500; at < 6000; at += 256U) {
		ns += gfsk_ref_push(&d, &si[at], &sq[at], MIN(256U, 6000U - at), &syms[ns], SYMAX - ns);
	}
	ns += gfsk_ref_flush(&d, &syms[ns], SYMAX - ns);
	/* As the finder: the strong burst's AA exactly, then the 200 bits after it. */
	for (size_t j = 0; j < ns; j++) {
		if (syms[j].t >= b[1].start - 64U && syms[j].t < b[1].start + 4U * 400U) {
			got[ng++] = syms[j].bit;
		}
	}
	for (size_t j = 0; j + 232U <= ng && !found; j++) {
		for (k = 0; k < 232U && got[j + k] == bits_b[8U + k]; k++) {
		}
		found = k == 232U;
	}
	zassert_true(found, "the strong burst's AA and the 200 bits after it not found");
}

/*
 * The noise floor the demodulator tracks (and the modes set their squelch from) comes down to the
 * noise after a start near full scale, the receiver's transient after a tune.
 */
ZTEST(ubertooth_demod, test_floor_after_loud_start)
{
	static struct gfsk_ref d;
	struct burst b = {.start = 0, .bits = bits_a, .nsym = 50, .h = 0.5, .dbfs = -0.5};
	uint8_t floor;

	gauss_init(0.5);
	ble_bits(bits_a, b.nsym, 0x8e89bed6U);
	gen(si, sq, 24000, &b, 1, -50);
	gfsk_ref_init(&d, GFSK_BLE);
	(void)demod_all(&d, 24000, 256);
	floor = gfsk_ref_noise_floor(&d);
	/* Noise of -50 dBFS over 4 MHz: some -56 dBFS in the channel, pwr 88. */
	zassert_true(floor >= 80U && floor <= 96U, "floor %u", floor);
}

/* ... and a gap followed by a burst does not reseed it (the burst would read as the floor). */
ZTEST(ubertooth_demod, test_floor_across_a_gap)
{
	static struct gfsk_ref d;
	struct burst b = {.start = 20000, .bits = bits_a, .nsym = 100, .h = 0.5, .dbfs = -10};
	uint8_t floor;

	gauss_init(0.5);
	ble_bits(bits_a, b.nsym, 0x8e89bed6U);
	gen(si, sq, 21000, &b, 1, -50);
	gfsk_ref_init(&d, GFSK_BLE);
	(void)gfsk_ref_push(&d, si, sq, 16000, syms, SYMAX);
	gfsk_ref_gap(&d, 4000);
	(void)gfsk_ref_push(&d, &si[20000], &sq[20000], 1000, syms, SYMAX);
	floor = gfsk_ref_noise_floor(&d);
	zassert_true(floor >= 80U && floor <= 96U, "floor %u", floor);
}

/*
 * The modes' floor measurement (ubt_rx_sess_floor): the stream through an always emitting
 * demodulator, a low percentile of the symbols' power. Returns it, the tracker's in @p tracker.
 */
static uint8_t floor_measured(size_t n, uint8_t *tracker)
{
	static struct gfsk_ref d;
	static struct ubt_floor_hist h;
	size_t ns;

	gfsk_ref_init(&d, GFSK_BLE);
	ns = demod_all(&d, n, 256);
	ubt_floor_hist_init(&h);
	for (size_t k = 0; k < ns; k++) {
		ubt_floor_hist_add(&h, syms[k].pwr);
	}
	*tracker = gfsk_ref_noise_floor(&d);
	return ubt_floor_hist_get(&h);
}

/* On plain noise the percentile reads what the tracker reads (the squelch margins assume it). */
ZTEST(ubertooth_demod, test_floor_percentile_noise)
{
	uint8_t floor, tracker;

	gen(si, sq, NMAX, NULL, 0, -50);
	floor = floor_measured(NMAX, &tracker);
	TC_PRINT("plain noise: percentile %u, tracker %u\n", floor, tracker);
	zassert_true(floor >= 80U && floor <= 96U, "floor %u", floor);
	zassert_true(floor + 3U >= tracker && floor <= tracker + 3U, "floor %u, tracker %u", floor,
		     tracker);
}

/*
 * Wi-Fi far over the noise most of the time with short gaps: 25 dB up for 500 us, 50 us apart
 * (91 % busy), and 20 dB up for 150 us, 25 us apart. The tracker read these some 24 and 18 dB
 * over the noise; the percentile stays within 4 dB of it.
 */
ZTEST(ubertooth_demod, test_floor_percentile_bursts)
{
	static const struct {
		double dbfs;
		size_t on, off;
	} cs[] = {{-25, 2000, 200}, {-30, 600, 100}};
	uint8_t clean, floor, tracker;

	gen(si, sq, NMAX, NULL, 0, -50);
	clean = floor_measured(NMAX, &tracker);
	for (size_t c = 0; c < ARRAY_LEN(cs); c++) {
		gen(si, sq, NMAX, NULL, 0, -50);
		add_bursts(si, sq, NMAX, cs[c].dbfs, cs[c].on, cs[c].off);
		floor = floor_measured(NMAX, &tracker);
		TC_PRINT("bursts %zu: percentile %u (plain %u), tracker %u\n", c, floor, clean,
			 tracker);
		zassert_true(floor <= clean + 8U, "case %zu: floor %u against %u without bursts", c,
			     floor, clean);
	}
}

/* A gap moves the time stamps over the lost samples. */
ZTEST(ubertooth_demod, test_gap_time)
{
	static struct gfsk_ref d;
	struct burst b = {.start = 100, .bits = bits_a, .nsym = 1500, .h = 0.5, .dbfs = -12};
	size_t ns;

	gauss_init(0.5);
	ble_bits(bits_a, b.nsym, 0x8e89bed6U);
	gen(si, sq, 6200, &b, 1, -60);
	gfsk_ref_init(&d, GFSK_BLE);
	ns = gfsk_ref_push(&d, si, sq, 3000, syms, SYMAX);
	gfsk_ref_gap(&d, 1000);
	zassert_equal(gfsk_ref_now(&d), 4000U);
	ns += gfsk_ref_push(&d, &si[4000], &sq[4000], 2200, &syms[ns], SYMAX - ns);
	ns += gfsk_ref_flush(&d, &syms[ns], SYMAX - ns);
	for (size_t k = 1; k < ns; k++) {
		zassert_true(syms[k].t > syms[k - 1].t, "symbol %zu out of order", k);
		zassert_false(syms[k].t > 3000U + 8U && syms[k].t < 4000U, "symbol at %u in the gap",
			      syms[k].t);
	}
	zassert_true(syms[ns - 1U].t > 6000U, "last at %u", syms[ns - 1U].t);
}

/* ---- RSSI ------------------------------------------------------------------------------- */

ZTEST(ubertooth_demod, test_rssi_conversions)
{
	/* k = -132: 0 dBFS (pwr 200) reads -66 dBm, raw -66 + 54. */
	zassert_equal(ubt_rssi_dbm_x2(200), -132);
	zassert_equal(ubt_rssi_raw(200), -12);
	zassert_equal(ubt_rssi_raw(199), -12); /* -66.5 dBm rounds up */
	zassert_equal(ubt_rssi_raw(198), -13);
	zassert_equal(ubt_rssi_raw(0), -112);
	ubt_rssi_set_cal(-100);
	zassert_equal(ubt_rssi_raw(200), 4);
	zassert_equal(ubt_rssi_raw(255), 32);
	ubt_rssi_set_cal(UBT_RSSI_K_DEFAULT);
	/* The BR byte comes back as the dBm it was made from through the host's map. */
	for (int dbm = -95; dbm <= -30; dbm++) {
		int back = ubt_rssi_br_to_dbm(ubt_rssi_br_raw_from_dbm(dbm));

		zassert_true(back >= dbm - 1 && back <= dbm + 1, "%d dBm reads %d", dbm, back);
	}
	zassert_equal(ubt_rssi_br_raw_from_dbm(-130), -128);
	zassert_equal(ubt_rssi_br_raw_from_dbm(3), 36);
}

ZTEST(ubertooth_demod, test_squelch_levels)
{
	struct ubt_rssi r;

	ubt_rssi_init(&r);
	ubt_rssi_squelch_update(&r, 2441);
	zassert_true(r.cs_no_squelch);
	zassert_equal(ubt_rssi_squelch_pwr(&r), 0U);
	/* Absolute: the CC2400's 4 dB grid, rounded down. */
	ubt_rssi_set_squelch(&r, -70);
	ubt_rssi_squelch_update(&r, 2441);
	zassert_false(r.cs_no_squelch);
	zassert_equal(r.cs_cur, -72);
	zassert_equal(ubt_rssi_get_squelch(&r), -70);
	zassert_equal(ubt_rssi_squelch_raw(&r), -18);
	/* pwr = 2 dBm + 200 - k */
	zassert_equal(ubt_rssi_squelch_pwr(&r), 188U);
	/* Relative: over the channel's average, a typical floor without history. */
	ubt_rssi_set_squelch(&r, 10);
	ubt_rssi_squelch_update(&r, 2441);
	zassert_equal(r.cs_cur, -92);
	/* Clamped to -20 dBm at the top. */
	ubt_rssi_set_squelch(&r, -5);
	ubt_rssi_squelch_update(&r, 2441);
	zassert_equal(r.cs_cur, -20);
}

ZTEST(ubertooth_demod, test_rssi_average)
{
	struct ubt_rssi r;
	struct ubt_rssi_blk b;

	ubt_rssi_init(&r);
	zassert_equal(ubt_rssi_iir_get(&r, 2402), UBT_RSSI_NOISE_RAW);
	ubt_rssi_blk_reset(&b);
	ubt_rssi_blk_add(&b, -40);
	ubt_rssi_blk_add(&b, -30);
	zassert_equal(ubt_rssi_blk_avg_x256(&b), -35 * 256);
	/* The first block seeds the channel, other channels keep none. */
	ubt_rssi_iir_update(&r, 2402, &b);
	zassert_equal(ubt_rssi_iir_get(&r, 2402), -35);
	zassert_equal(ubt_rssi_iir_get(&r, 2403), UBT_RSSI_NOISE_RAW);
	/* Then 3/256 of each new block. */
	ubt_rssi_blk_reset(&b);
	for (int k = 0; k < 10; k++) {
		ubt_rssi_blk_add(&b, 50);
	}
	for (int k = 0; k < 85; k++) {
		ubt_rssi_iir_update(&r, 2402, &b);
	}
	/* -35 + 85 (1 - (253/256)^85): about 50 - 85 * 0.37. */
	zassert_true(ubt_rssi_iir_get(&r, 2402) >= 17 && ubt_rssi_iir_get(&r, 2402) <= 21,
		     "%d", ubt_rssi_iir_get(&r, 2402));
	/* Outside the band: one shared slot, never past the table. */
	ubt_rssi_iir_update(&r, 2794, &b);
}

/* The clip guard's cases moved to tests/unit/gain_guard with it (lib/esp_sdr). */

/* ---- RX_SYMBOLS blocks ------------------------------------------------------------------ */

#define MAXPKT 64
static usb_pkt_rx pkts[MAXPKT];
static size_t npkts;

static void collect(void *ctx, const usb_pkt_rx *pkt)
{
	ARG_UNUSED(ctx);
	if (npkts < MAXPKT) {
		pkts[npkts] = *pkt;
	}
	npkts++;
}

static unsigned int block_bit(const usb_pkt_rx *p, unsigned int k)
{
	return (p->data[k / 8U] >> (7U - k % 8U)) & 1U;
}

ZTEST(ubertooth_demod, test_br_blocks_bits_and_stamps)
{
	static struct ubt_br b;
	static struct gfsk_sym s[900];
	static uint8_t bits[900];

	npkts = 0;
	ubt_br_init(&b, collect, NULL, NULL, NULL);
	ubt_br_retune(&b, 2441);
	for (unsigned int k = 0; k < 900U; k++) {
		bits[k] = (uint8_t)(test_rng_u32() >> 31);
		s[k].t = 1000U + 4U * k;
		s[k].bit = bits[k];
		s[k].pwr = 150;
	}
	ubt_br_push(&b, s, 900);
	zassert_equal(npkts, 2U);
	for (unsigned int n = 0; n < 2U; n++) {
		const usb_pkt_rx *p = &pkts[n];

		zassert_equal(p->pkt_type, BR_PACKET);
		zassert_equal(p->channel, 39U);
		for (unsigned int k = 0; k < UBT_BR_BLOCK_SYMS; k++) {
			zassert_equal(block_bit(p, k), bits[n * UBT_BR_BLOCK_SYMS + k],
				      "block %u symbol %u", n, k);
		}
		/* Stamped when the last symbol ends: 2 samples after its centre, 2.5 units each. */
		zassert_equal(p->clk100ns,
			      (uint32_t)((1000U + 4U * (400U * n + 399U) + 2U) * 5U / 2U));
	}
	/* The first block after a retune is flagged for the host to drop. */
	zassert_equal(pkts[0].status & DISCARD, DISCARD);
	zassert_equal(pkts[1].status & DISCARD, 0);
}

ZTEST(ubertooth_demod, test_br_blocks_idle)
{
	static struct ubt_br b;
	static struct gfsk_sym s[100];

	npkts = 0;
	ubt_br_init(&b, collect, NULL, NULL, NULL);
	ubt_br_set_drop(&b, 2);
	/* 40 ones, then the squelch closes; 1000 slots later 60 ones again. */
	for (unsigned int k = 0; k < 100U; k++) {
		s[k].t = k < 40U ? 4U * k : 4U * (k - 40U) + 4000U;
		s[k].bit = 1;
		s[k].pwr = 160;
	}
	ubt_br_push(&b, s, 40);
	zassert_equal(npkts, 0U);
	/* The demodulator says nothing comes before 3000: the slots up to it are idle. */
	ubt_br_advance(&b, 3000, 60);
	zassert_equal(npkts, 1U);
	for (unsigned int k = 0; k < UBT_BR_BLOCK_SYMS; k++) {
		zassert_equal(block_bit(&pkts[0], k), k < 40U ? 1U : 0U, "symbol %u", k);
	}
	ubt_br_push(&b, &s[40], 60);
	/* Slot 1000 is in the third block: the second (all idle) went out, the third is open. */
	zassert_equal(npkts, 2U);
	ubt_br_advance(&b, 4U * 1600U, 60);
	zassert_equal(npkts, 4U);
	for (unsigned int k = 0; k < UBT_BR_BLOCK_SYMS; k++) {
		unsigned int slot = 800U + k;

		zassert_equal(block_bit(&pkts[2], k), slot >= 1000U && slot < 1060U ? 1U : 0U,
			      "slot %u", slot);
	}
	/*
	 * Slots 1600 to 3999: the first idle block is the second in a row and goes out with the
	 * gap's flag, the five after it are dropped.
	 */
	ubt_br_gap(&b);
	ubt_br_advance(&b, 4U * 4000U, 60);
	zassert_equal(npkts, 5U, "%zu", npkts);
	zassert_equal(b.dropped_blocks, 5U, "%u", b.dropped_blocks);
	zassert_equal(pkts[4].status & DMA_OVERFLOW, DMA_OVERFLOW);
	/* A symbol at slot 5000 sends its block (4800 to 5199) again. */
	s[0].t = 4U * 5000U;
	ubt_br_push(&b, s, 1);
	ubt_br_advance(&b, 4U * 5400U, 60);
	zassert_equal(npkts, 6U);
	zassert_equal(block_bit(&pkts[5], 200), 1U);
	zassert_equal(block_bit(&pkts[5], 199), 0U);
	zassert_equal(pkts[5].status & DMA_OVERFLOW, 0);
}

ZTEST(ubertooth_demod, test_br_blocks_carrier_sense)
{
	static struct ubt_br b;
	static struct ubt_rssi r;
	static struct gfsk_sym s[400];

	npkts = 0;
	ubt_rssi_init(&r);
	ubt_rssi_set_squelch(&r, -80);
	ubt_br_init(&b, collect, NULL, NULL, &r);
	ubt_br_retune(&b, 2402);
	zassert_equal(r.cs_cur, -80);
	/* A burst at pwr 140 (-96 dBm with k = -132): carrier sense, under the RSSI trigger. */
	for (unsigned int k = 0; k < 400U; k++) {
		s[k].t = 4U * k;
		s[k].bit = (uint8_t)(k & 1U);
		s[k].pwr = 140;
	}
	ubt_br_push(&b, s, 400);
	zassert_equal(npkts, 1U);
	zassert_equal(pkts[0].status & CS_TRIGGER, CS_TRIGGER);
	zassert_equal(pkts[0].status & RSSI_TRIGGER, 0);
	zassert_equal(pkts[0].rssi_count, 255U);
	zassert_true(ubt_rssi_br_to_dbm(pkts[0].rssi_max) >= -97 &&
		     ubt_rssi_br_to_dbm(pkts[0].rssi_max) <= -95);
	/* Louder (pwr 180, -76 dBm): over -80 dBm. The burst goes on: no new carrier sense. */
	for (unsigned int k = 0; k < 400U; k++) {
		s[k].t = 4U * (k + 400U);
		s[k].pwr = 180;
	}
	ubt_br_push(&b, s, 400);
	zassert_equal(npkts, 2U);
	zassert_equal(pkts[1].status & RSSI_TRIGGER, RSSI_TRIGGER);
	zassert_equal(pkts[1].status & CS_TRIGGER, 0);
	zassert_true(ubt_rssi_br_to_dbm(pkts[1].rssi_max) >= -77 &&
		     ubt_rssi_br_to_dbm(pkts[1].rssi_max) <= -75);
	/* The channel's average saw both blocks. */
	zassert_true(ubt_rssi_iir_get(&r, 2402) < -96 + 54 + 2);
}

/* ---- hop sequence ----------------------------------------------------------------------- */

ZTEST(ubertooth_demod, test_sweep_visits_every_channel)
{
	uint8_t map[10] = {0};
	uint8_t seen[79] = {0};
	uint16_t ch = 2402;

	for (unsigned int k = 0; k < 79U; k++) {
		ch = ubt_br_next_channel(HOP_SWEEP, false, map, ch);
		zassert_true(ch >= 2402U && ch <= 2480U);
		seen[ch - 2402U]++;
	}
	zassert_equal(ch, 2402U);
	for (unsigned int k = 0; k < 79U; k++) {
		zassert_equal(seen[k], 1U, "channel %u seen %u times", 2402U + k, seen[k]);
	}
	zassert_equal(ubt_br_next_channel(HOP_SWEEP, false, map, 2402), 2434U);
	zassert_equal(ubt_br_next_channel(HOP_SWEEP, false, map, 2470), 2423U);
}

static void map_set(uint8_t *map, unsigned int mhz)
{
	map[(mhz - 2402U) / 8U] |= (uint8_t)(1U << ((mhz - 2402U) % 8U));
}

ZTEST(ubertooth_demod, test_hops_with_afh_map)
{
	uint8_t map[10] = {0};
	uint16_t ch = 2402;

	/* The sweep with a map: only the channels in use. */
	map_set(map, 2410);
	map_set(map, 2440);
	map_set(map, 2470);
	for (unsigned int k = 0; k < 20U; k++) {
		ch = ubt_br_next_channel(HOP_SWEEP, true, map, ch);
		zassert_true(ch == 2410U || ch == 2440U || ch == 2470U, "%u", ch);
	}
	/* ... unless the map is not enabled. */
	zassert_equal(ubt_br_next_channel(HOP_SWEEP, false, map, 2402), 2434U);
	/* The AFH mode: only the channels the map does not hold yet. */
	ch = 2402;
	for (unsigned int k = 0; k < 100U; k++) {
		ch = ubt_br_next_channel(HOP_AFH, true, map, ch);
		zassert_true(ch != 2410U && ch != 2440U && ch != 2470U, "%u", ch);
	}
	/* All held: it goes on 32 MHz at a time. */
	memset(map, 0xff, sizeof(map));
	map[9] = 0x7f;
	zassert_equal(ubt_br_next_channel(HOP_AFH, true, map, 2402), 2434U);
}
