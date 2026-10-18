/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/link/src/bch.c: for every supported (m, t), at the longest
 * and shorter block lengths and every byte alignment (the remainder register
 * reads aligned words), each error count up to t must be corrected exactly;
 * t + 1 errors must never pass as a clean block. Runs on native_sim.
 */
#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "bch.h"

#define RNG_SEED 0x9e3779b97f4a7c15ULL
static struct bch_code code;

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_rng_seed(RNG_SEED);
}

ZTEST_SUITE(link_bch, NULL, NULL, before_each, NULL, NULL);

/* Flip bit p of the block (data bits, then the r parity bits; each byte LSB first). */
static void flip(uint8_t *data, unsigned int k, uint8_t *parity, unsigned int p)
{
	if (p < 8U * k) {
		data[p / 8U] ^= (uint8_t)(1U << (p % 8U));
	} else {
		p -= 8U * k;
		parity[p / 8U] ^= (uint8_t)(1U << (p % 8U));
	}
}

/* e distinct bit positions of a block of len bits. */
static void positions(unsigned int *pos, unsigned int e, unsigned int len)
{
	for (unsigned int j = 0; j < e; j++) {
		bool dup;

		do {
			pos[j] = test_rng_u32() % len;
			dup = false;
			for (unsigned int q = 0; q < j; q++) {
				dup |= pos[q] == pos[j];
			}
		} while (dup);
	}
}

/* Only the r parity bits count, not the spare high bits of the last byte. */
static bool parity_equal(const uint8_t *a, const uint8_t *b, unsigned int r)
{
	for (unsigned int i = 0; i < r; i++) {
		if (((a[i / 8U] ^ b[i / 8U]) >> (i % 8U) & 1U) != 0U) {
			return false;
		}
	}
	return true;
}

ZTEST(link_bch, test_params)
{
	unsigned int r, rb, kmax;

	zassert_true(bch_params(10, 2, &r, &rb, &kmax));
	zassert_equal(r, 20);
	zassert_equal(rb, 3);
	zassert_equal(kmax, 125);
	/* Hamming, 56 data bits per 64. */
	zassert_true(bch_params(6, 1, &r, &rb, &kmax));
	zassert_equal(r, 6);
	zassert_equal(kmax, 7);
	zassert_false(bch_params(12, 2, &r, &rb, &kmax));
	zassert_false(bch_params(10, 5, &r, &rb, &kmax));
	zassert_equal(bch_init(&code, 4, 1), -1);
}

ZTEST(link_bch, test_correct_up_to_t)
{
	for (unsigned int m = BCH_M_MIN; m <= BCH_M_MAX; m++) {
		for (unsigned int t = 1; t <= BCH_T_MAX; t++) {
			unsigned int r, rb, kmax;

			if (!bch_params(m, t, &r, &rb, &kmax)) {
				continue;
			}
			zassert_equal(bch_init(&code, m, t), 0);
			for (unsigned int ki = 0; ki < 3U; ki++) {
				unsigned int k = ki == 0U ? kmax : (ki == 1U ? (kmax + 1U) / 2U : 1U);

				for (unsigned int e = 0; e <= t; e++) {
					for (unsigned int rep = 0; rep < 40U; rep++) {
						uint8_t buf[264] __aligned(4), ref[256], par[8], pref[8];
						uint8_t *d = &buf[rep % 4U];
						unsigned int pos[BCH_T_MAX];

						for (unsigned int i = 0; i < k; i++) {
							d[i] = (uint8_t)test_rng_u32();
						}
						bch_encode(&code, d, k, par);
						memcpy(ref, d, k);
						memcpy(pref, par, rb);
						positions(pos, e, 8U * k + r);
						for (unsigned int j = 0; j < e; j++) {
							flip(d, k, par, pos[j]);
						}
						zassert_equal(bch_decode(&code, d, k, par), (int)e,
							      "m %u t %u k %u e %u", m, t, k, e);
						zassert_mem_equal(d, ref, k, "m %u t %u k %u e %u", m,
								  t, k, e);
						zassert_true(parity_equal(par, pref, r),
							     "m %u t %u k %u e %u: parity", m, t, k, e);
					}
				}
			}
		}
	}
}

ZTEST(link_bch, test_more_than_t_never_clean)
{
	/* t + 1 errors: detected or miscorrected (the frame CRC's job), never a clean 0. */
	for (unsigned int t = 1; t <= BCH_T_MAX; t++) {
		unsigned int r, rb, kmax, detected = 0;

		zassert_true(bch_params(10, t, &r, &rb, &kmax));
		zassert_equal(bch_init(&code, 10, t), 0);
		for (unsigned int rep = 0; rep < 200U; rep++) {
			uint8_t d[128], par[8];
			unsigned int pos[BCH_T_MAX + 1U];
			int ret;

			for (unsigned int i = 0; i < kmax; i++) {
				d[i] = (uint8_t)test_rng_u32();
			}
			bch_encode(&code, d, kmax, par);
			positions(pos, t + 1U, 8U * kmax + r);
			for (unsigned int j = 0; j <= t; j++) {
				flip(d, kmax, par, pos[j]);
			}
			ret = bch_decode(&code, d, kmax, par);
			zassert_not_equal(ret, 0, "t %u: %u errors passed as clean", t, t + 1U);
			detected += ret < 0;
		}
		TC_PRINT("t %u: %u of 200 blocks with t + 1 errors detected\n", t, detected);
	}
}
