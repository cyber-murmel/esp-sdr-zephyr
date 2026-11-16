/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the diagnostic-bus ring's signal processing (lib/esp_sdr/src/esp_sdr_diag_dsp.h):
 * box16 and the third-order CIC against naive references, bit-exact, then a quantized tone for the
 * scaling. Runs on native_sim.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "esp_sdr_diag_dsp.h"

#define RNG_SEED 0x2545F4914F6CDD1DULL

static int32_t nib(uint8_t v)
{
	return (v & 8) ? (int32_t)(v & 15) - 16 : (int32_t)(v & 15);
}

/* Q low nibble, I high nibble of byte k. */
static void bytes_iq(const uint8_t *b, size_t k, int32_t *i, int32_t *q)
{
	*q = nib(b[k] & 15);
	*i = nib(b[k] >> 4);
}

ZTEST(diag_dsp, test_box16)
{
	static uint32_t w[4 * 64];
	uint8_t *b = (uint8_t *)w;

	for (size_t k = 0; k < sizeof(w); k++) {
		b[k] = (uint8_t)test_rng_u32();
	}
	/* Extremes too: all -8, all 7. */
	memset(&b[16], 0x88, 16);
	memset(&b[32], 0x77, 16);
	for (size_t blk = 0; blk < sizeof(w) / 16U; blk++) {
		int32_t si, sq, ri = 0, rq = 0;

		diag_box16(&w[blk * 4U], &si, &sq);
		for (size_t k = 0; k < 16; k++) {
			int32_t i, q;

			bytes_iq(b, blk * 16U + k, &i, &q);
			ri += i;
			rq += q;
		}
		zassert_equal(si, ri, "I block %zu: %d vs %d", blk, si, ri);
		zassert_equal(sq, rq, "Q block %zu: %d vs %d", blk, sq, rq);
	}
}

/* Reference: the box16 outputs through three boxcars of R, at every R-th input. */
static void cic_ref(const int32_t *x, size_t n, uint32_t r, int32_t *y)
{
	for (size_t o = 0; (o + 1) * r <= n; o++) {
		size_t j = (o + 1) * r - 1;
		int64_t acc = 0;

		/* h = boxcar * boxcar * boxcar: h[m] counts the (a, b, c) in [0, R)^3 with a + b + c = m. */
		for (size_t m = 0; m < 3U * r - 2U && m <= j; m++) {
			int64_t cnt = 0;

			for (uint32_t a = 0; a < r; a++) {
				for (uint32_t bb = 0; bb < r; bb++) {
					int64_t cc = (int64_t)m - a - bb;

					if (cc >= 0 && cc < r) {
						cnt++;
					}
				}
			}
			acc += cnt * x[j - m];
		}
		y[o] = (int32_t)acc;
	}
}

static void check_cic(uint32_t r, size_t chunk_bytes)
{
	/* Enough for 12 outputs after the three dropped. */
	size_t boxes = (15U * r) + 7U;
	size_t nbytes = boxes * 16U;
	uint32_t *w = malloc(nbytes);
	int32_t *xi = malloc(boxes * 4U), *xq = malloc(boxes * 4U);
	int32_t *ri = malloc(boxes * 4U), *rq = malloc(boxes * 4U);
	int32_t *out = malloc(boxes * 8U);
	struct diag_cic c;
	size_t got = 0;
	uint8_t *b = (uint8_t *)w;

	zassert_not_null(w);
	for (size_t k = 0; k < nbytes; k++) {
		b[k] = (uint8_t)test_rng_u32();
	}
	for (size_t k = 0; k < boxes; k++) {
		diag_box16(&w[k * 4U], &xi[k], &xq[k]);
	}
	cic_ref(xi, boxes, r, ri);
	cic_ref(xq, boxes, r, rq);
	diag_cic_init(&c, r);
	for (size_t off = 0; off < nbytes; off += chunk_bytes) {
		size_t n = MIN(chunk_bytes, nbytes - off);

		got += diag_cic_run(&c, &w[off / 4U], n, &out[2U * got]);
	}
	zassert_equal(got, boxes / r - 3U, "R %u: %zu outputs", r, got);
	for (size_t o = 0; o < got; o++) {
		zassert_equal(out[2U * o], ri[o + 3U], "R %u I out %zu: %d vs %d", r, o, out[2U * o],
			      ri[o + 3U]);
		zassert_equal(out[2U * o + 1U], rq[o + 3U], "R %u Q out %zu", r, o);
	}
	free(w);
	free(xi);
	free(xq);
	free(ri);
	free(rq);
	free(out);
}

ZTEST(diag_dsp, test_cic_exact)
{
	/* R values in and around the supported range, one chunk and the ring's 8192-byte blocks. */
	static const uint32_t rs[] = {8, 10, 20, 40, 80, 160};

	for (size_t k = 0; k < ARRAY_SIZE(rs); k++) {
		check_cic(rs[k], 16U * 1000000U);
		check_cic(rs[k], 8192U);
		/* Chunks that are not a multiple of a decimation period. */
		check_cic(rs[k], 16U * 7U);
	}
}

ZTEST(diag_dsp, test_scale)
{
	/* A tone 2 kHz above the LO at 40 MS/s, amplitude 200 raw counts, window lsb 5. */
	const unsigned int lsb = 5;
	const uint32_t r = 40;
	const double fs = 40e6, f = 2e3, amp = 200.0;
	size_t boxes = 600U * r / 4U;
	size_t n = boxes * 16U;
	uint32_t *w = malloc(n);
	int32_t *out = malloc(boxes * 8U);
	struct diag_cic c;
	uint8_t *b = (uint8_t *)w;
	size_t got;
	uint32_t inv = diag_cic_inv(r, lsb);
	double sum = 0;
	size_t cnt = 0;

	for (size_t k = 0; k < n; k++) {
		double ph = ESP_SDR_TAU * f / fs * (double)k;
		double di = ((int)(test_rng_u32() & 1023) - 512) / 1024.0;
		double dq = ((int)(test_rng_u32() & 1023) - 512) / 1024.0;
		int32_t i = (int32_t)floor(amp * cos(ph) / (1 << lsb) + 0.5 + di);
		int32_t q = (int32_t)floor(amp * sin(ph) / (1 << lsb) + 0.5 + dq);

		i = CLAMP(i, -8, 7);
		q = CLAMP(q, -8, 7);
		b[k] = (uint8_t)(((i & 15) << 4) | (q & 15));
	}
	diag_cic_init(&c, r);
	got = diag_cic_run(&c, w, n, out);
	zassert_true(got > 100, "%zu outputs", got);
	for (size_t o = 10; o < got; o++) {
		double i = diag_norm(out[2U * o], inv), q = diag_norm(out[2U * o + 1U], inv);

		sum += sqrt(i * i + q * q);
		cnt++;
	}
	/* Raw counts times 16. The CIC droops by sinc^3(f R / 2.5 MHz): 2 kHz at R = 40 is under 0.01 dB. */
	double mean = sum / cnt, want = amp * 16.0;

	zassert_within(mean, want, want * 0.05, "mean magnitude %.1f, want %.1f", mean, want);
	zassert_true(inv < 0xffffffffU, "reciprocal fits");
	for (uint32_t rr = DIAG_CIC_R_MIN; rr <= DIAG_CIC_R_MAX; rr++) {
		uint64_t r3 = (uint64_t)rr * rr * rr;

		zassert_true(((1ULL << 38) + r3 / 2U) / r3 < 0xffffffffULL, "R %u at lsb 6", rr);
	}
	free(w);
	free(out);
}

static void before_each(void *f)
{
	ARG_UNUSED(f);
	test_rng_seed(RNG_SEED);
}

ZTEST_SUITE(diag_dsp, NULL, NULL, before_each, NULL, NULL);
