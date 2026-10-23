/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esp_sdr_lte.h on synthetic synchronization symbols: a PSS and the SSS in front of it every 5 ms
 * at a known carrier offset, in noise, sampled at 2 MS/s like the WHY2025 badge's stream. And
 * the temperature model of esp_sdr_cal.h: its points and its fit.
 */
#include <math.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_cal.h>
#include <esp_sdr/esp_sdr_lte.h>
#include <test_rng.h>

#define FS      2e6
#define N       12500
#define NFFT    16384
#define SIGMA   20.0

static struct esp_sdr_lte_iq cap[N];
static float work_mem[ESP_SDR_LTE_WORK_FLOATS(NFFT)];
static struct esp_sdr_lte_work work;

/* One symbol with subcarriers d, useful part starting at sample `start` (cyclic prefix included). */
static void put_symbol(double *xr, double *xi, double start, const float *dr, const float *di,
		       double f_hz, double amp)
{
	double cp = 144.0 / 30.72e6 * FS;
	double len = FS / ESP_SDR_LTE_DF;

	for (long i = (long)ceil(start - cp); (double)i < start + len; i++) {
		if (i < 0 || i >= N) {
			continue;
		}
		double sr = 0.0, si = 0.0;

		for (int idx = 0; idx < ESP_SDR_LTE_SC; idx++) {
			double a = ESP_SDR_TAU * esp_sdr_lte_k(idx) * ESP_SDR_LTE_DF * ((double)i - start) / FS;

			sr += (double)dr[idx] * cos(a) - (double)di[idx] * sin(a);
			si += (double)dr[idx] * sin(a) + (double)di[idx] * cos(a);
		}
		double c = ESP_SDR_TAU * f_hz * (double)i / FS;

		xr[i] += amp * (sr * cos(c) - si * sin(c));
		xi[i] += amp * (sr * sin(c) + si * cos(c));
	}
}

/* Pairs every 5 ms from `first`; the first one is in half `half`. */
static void synth(int n_id1, int n_id2, int half, double f_hz, double first, double amp)
{
	static double xr[N], xi[N];
	float pr[ESP_SDR_LTE_SC], pi_[ESP_SDR_LTE_SC], sr_[ESP_SDR_LTE_SC], si_[ESP_SDR_LTE_SC] = {0};
	int j = 0;

	for (int i = 0; i < N; i++) {
		xr[i] = xi[i] = 0.0;
	}
	esp_sdr_lte_pss(n_id2, pr, pi_);
	for (double s = first; s + 140.0 < N; s += ESP_SDR_LTE_SYNC_PERIOD_S * FS, j++) {
		esp_sdr_lte_sss(n_id1, n_id2, (half + j) & 1, sr_);
		put_symbol(xr, xi, s, pr, pi_, f_hz, amp);
		put_symbol(xr, xi, s - ESP_SDR_LTE_SSS_LEAD_S * FS, sr_, si_, f_hz, amp);
	}
	for (int i = 0; i < N; i++) {
		cap[i].i = (int16_t)lround(xr[i] + SIGMA * test_rng_gauss());
		cap[i].q = (int16_t)lround(xi[i] + SIGMA * test_rng_gauss());
	}
}

static void *setup(void)
{
	esp_sdr_lte_work_init(&work, work_mem, NFFT);
	return NULL;
}

ZTEST_SUITE(lte, NULL, setup, NULL, NULL, NULL);

ZTEST(lte, test_sequences)
{
	float a[ESP_SDR_LTE_SC], b[ESP_SDR_LTE_SC], pr[ESP_SDR_LTE_SC], pi_[ESP_SDR_LTE_SC];

	for (int u = 0; u < 3; u++) {
		esp_sdr_lte_pss(u, pr, pi_);
		for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
			zassert_within(pr[k] * pr[k] + pi_[k] * pi_[k], 1.0f, 1e-5f);
		}
	}
	/* Different cell groups and half frames are far from each other. */
	esp_sdr_lte_sss(23, 0, 1, a);
	for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
		zassert_true(a[k] == 1.0f || a[k] == -1.0f);
	}
	float self = 0.0f;

	for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
		self += a[k] * a[k];
	}
	zassert_within(self, 62.0f, 1e-3f);
	for (int n1 = 0; n1 < 168; n1++) {
		for (int h = 0; h < 2; h++) {
			if (n1 == 23 && h == 1) {
				continue;
			}
			esp_sdr_lte_sss(n1, 0, h, b);
			float c = 0.0f;

			for (int k = 0; k < ESP_SDR_LTE_SC; k++) {
				c += a[k] * b[k];
			}
			zassert_true(fabsf(c) < 0.5f * 62.0f, "n1 %d half %d corr %f", n1, h, (double)c);
		}
	}
}

ZTEST(lte, test_detect)
{
	static const struct {
		int n_id1, n_id2, half;
		double f_hz, first, amp;
	} t[] = {
		{23, 0, 1, -490636.0, 4617.37, 5.0},
		{0, 1, 0, 120000.0, 2310.8, 3.6},
		{167, 2, 1, -17250.0, 3000.0, 3.6},
		{80, 0, 0, 640000.0, 1800.62, 5.0},
		{95, 1, 1, -750000.0, 5200.25, 5.0},
	};

	for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
		struct esp_sdr_lte_cell c;

		test_rng_seed(1000 + i);
		synth(t[i].n_id1, t[i].n_id2, t[i].half, t[i].f_hz, t[i].first, t[i].amp);
		int ret = esp_sdr_lte_detect(cap, N, FS, 900e3, 20.0f, &work, &c);

		zassert_equal(ret, 0, "case %zu ret %d ratio %f", i, ret, (double)c.pss_ratio);
		zassert_true(c.valid, "case %zu sss %f second %f", i, (double)c.sss_metric,
			     (double)c.sss_second);
		zassert_equal(c.n_id2, t[i].n_id2, "case %zu", i);
		zassert_equal(c.n_id1, t[i].n_id1, "case %zu", i);
		zassert_equal(c.pairs, t[i].first + ESP_SDR_LTE_SYNC_PERIOD_S * FS + 140.0 < N ? 2 : 1,
			      "case %zu", i);
		zassert_within(c.offset_hz, t[i].f_hz, 1500.0, "case %zu offset %f", i, c.offset_hz);
		zassert_within(c.timing, t[i].first, 1.0, "case %zu timing %f", i, c.timing);
	}
}

ZTEST(lte, test_one_pair_in_a_short_capture)
{
	/* A 1 ms window with one pair: the parity of the half frame is free, the cell is not. */
	struct esp_sdr_lte_cell c;

	test_rng_seed(7);
	synth(23, 0, 0, -312500.0, 700.5, 5.0);
	int ret = esp_sdr_lte_detect(cap, 2047, FS, 900e3, 20.0f, &work, &c);

	zassert_equal(ret, 0, "ret %d ratio %f", ret, (double)c.pss_ratio);
	zassert_true(c.valid);
	zassert_equal(c.cell_id, 69);
	zassert_equal(c.pairs, 1);
	zassert_within(c.offset_hz, -312500.0, 1500.0, "offset %f", c.offset_hz);
}

ZTEST(lte, test_noise_is_rejected)
{
	struct esp_sdr_lte_cell c;

	test_rng_seed(99);
	for (int i = 0; i < N; i++) {
		cap[i].i = (int16_t)lround(SIGMA * test_rng_gauss());
		cap[i].q = (int16_t)lround(SIGMA * test_rng_gauss());
	}
	zassert_equal(esp_sdr_lte_detect(cap, 2047, FS, 900e3, 20.0f, &work, &c), -ENOENT,
		      "ratio %f", (double)c.pss_ratio);
	zassert_equal(esp_sdr_lte_detect(cap, N, FS, 900e3, 20.0f, &work, &c), -ENOENT,
		      "ratio %f", (double)c.pss_ratio);
}

ZTEST(lte, test_ppb)
{
	double raster;
	/* Measured on the WHY2025 badge's C6: the carrier 9.4 kHz above the raster point. */
	double ppb = esp_sdr_lte_ppb(2135e6, -490636.6, &raster);

	zassert_within(raster, 2134.5e6, 1.0);
	zassert_within(ppb, -4385.7, 0.5);
	/* The carrier seen 8 kHz above where it should be: the LO runs low. */
	ppb = esp_sdr_lte_ppb(2140e6, 8000.0, &raster);
	zassert_within(raster, 2140.0e6, 1.0);
	zassert_within(ppb, -8000.0 / 2140e6 * 1e9, 0.5);
	/* 8 kHz below: the LO runs high. */
	ppb = esp_sdr_lte_ppb(2140e6, -8000.0, &raster);
	zassert_within(ppb, 8000.0 / 2140e6 * 1e9, 0.5);
	/* Half a raster step off the grid snaps to the nearer point. */
	ppb = esp_sdr_lte_ppb(2140e6, 49000.0, &raster);
	zassert_within(raster, 2140.0e6, 1.0);
}

ZTEST(lte, test_cal_points)
{
	struct esp_sdr_cal_record rec = {0};

	/* Within 2 degC of a point: replaced, and the newest. */
	esp_sdr_cal_add_point(&rec, 250, -4000);
	esp_sdr_cal_add_point(&rec, 400, -3000);
	esp_sdr_cal_add_point(&rec, 262, -4100);
	zassert_equal(rec.points, 2);
	zassert_equal(rec.point[0].temp_dc, 400);
	zassert_equal(rec.point[1].temp_dc, 262);
	zassert_equal(rec.point[1].ppb, -4100);
	/* Full: the oldest goes. */
	for (int i = 0; i < ESP_SDR_CAL_POINTS; i++) {
		esp_sdr_cal_add_point(&rec, (int16_t)(500 + 50 * i), i);
	}
	zassert_equal(rec.points, ESP_SDR_CAL_POINTS);
	zassert_equal(rec.point[0].temp_dc, 500);
	zassert_equal(rec.point[ESP_SDR_CAL_POINTS - 1].temp_dc, 500 + 50 * (ESP_SDR_CAL_POINTS - 1));
}

ZTEST(lte, test_cal_eval)
{
	struct esp_sdr_cal_record rec = {0};
	int32_t ppb;

	zassert_equal(esp_sdr_cal_eval(&rec, 250, &ppb), -ENOENT);
	/* One point: a constant. */
	esp_sdr_cal_add_point(&rec, 250, -4386);
	zassert_ok(esp_sdr_cal_eval(&rec, 600, &ppb));
	zassert_equal(ppb, -4386);
	/* Two: the line, clamped 10 degC beyond the points. */
	esp_sdr_cal_add_point(&rec, 450, -2386);
	zassert_ok(esp_sdr_cal_eval(&rec, 350, &ppb));
	zassert_equal(ppb, -3386);
	zassert_ok(esp_sdr_cal_eval(&rec, 550, &ppb));
	zassert_equal(ppb, -1386);
	zassert_ok(esp_sdr_cal_eval(&rec, 900, &ppb));
	zassert_equal(ppb, -1386, "clamped at 55 degC: %d", ppb);
	zassert_ok(esp_sdr_cal_eval(&rec, -200, &ppb));
	zassert_equal(ppb, -5386, "clamped at 15 degC: %d", ppb);

	/* Three or more over 10 degC: a quadratic, recovered exactly from exact points. */
	struct esp_sdr_cal_record q = {0};

	for (int t = 100; t <= 600; t += 100) {
		double x = (t - 300) / 100.0;

		esp_sdr_cal_add_point(&q, (int16_t)t, (int32_t)lround(-3000.0 + 250.0 * x - 40.0 * x * x));
	}
	zassert_ok(esp_sdr_cal_eval(&q, 450, &ppb));
	zassert_within(ppb, (int32_t)lround(-3000.0 + 250.0 * 1.5 - 40.0 * 2.25), 1);
	zassert_ok(esp_sdr_cal_eval(&q, 150, &ppb));
	zassert_within(ppb, (int32_t)lround(-3000.0 - 250.0 * 1.5 - 40.0 * 2.25), 1);

	/* Three within 10 degC: a least-squares line, not a quadratic. */
	struct esp_sdr_cal_record l = {0};

	esp_sdr_cal_add_point(&l, 250, -4000);
	esp_sdr_cal_add_point(&l, 280, -3700);
	esp_sdr_cal_add_point(&l, 320, -3300);
	zassert_ok(esp_sdr_cal_eval(&l, 300, &ppb));
	zassert_within(ppb, -3500, 15);
}
