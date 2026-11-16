/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Binary BCH codec, see bch.h. A block's bits in order (data bytes, then the
 * parity bytes, each byte LSB first) are the coefficients of the codeword
 * polynomial from the highest degree down, n' = 8 k + r of them. The remainder
 * register is reflected like a CRC's: bit i holds the coefficient of
 * x^(r - 1 - i).
 */

#include <string.h>

#include "bch.h"

#if defined(__ZEPHYR__)
#include <esp_attr.h>
#define BCH_HOT IRAM_ATTR __attribute__((noinline))
#else
#define BCH_HOT
#endif

/* Primitive polynomials of GF(2^m), m 5 to 11. */
static const uint16_t prim_poly[BCH_M_MAX + 1U] = {
	[5] = 0x25, [6] = 0x43, [7] = 0x89, [8] = 0x11d, [9] = 0x211, [10] = 0x409, [11] = 0x805,
};

static bool in_range(unsigned int m, unsigned int t)
{
	return m >= BCH_M_MIN && m <= BCH_M_MAX && t >= 1U && t <= BCH_T_MAX;
}

/* The generator's degree: the size of the union of the cyclotomic cosets of 1, 3, .., 2t - 1. */
static unsigned int gen_degree(unsigned int m, unsigned int t)
{
	unsigned int n = (1U << m) - 1U, r = 0;
	uint32_t seen[(BCH_N_MAX + 32U) / 32U];

	memset(seen, 0, sizeof(seen));
	for (unsigned int i = 1; i < 2U * t; i += 2U) {
		unsigned int e = i;

		do {
			if ((seen[e / 32U] & (1U << (e % 32U))) == 0U) {
				seen[e / 32U] |= 1U << (e % 32U);
				r++;
			}
			e = (2U * e) % n;
		} while (e != i);
	}
	return r;
}

bool bch_params(unsigned int m, unsigned int t, unsigned int *r, unsigned int *rbytes,
		unsigned int *kmax)
{
	unsigned int deg;

	if (!in_range(m, t)) {
		return false;
	}
	deg = gen_degree(m, t);
	if (deg > 64U || deg + 8U > (1U << m) - 1U) {
		return false;
	}
	*r = deg;
	*rbytes = (deg + 7U) / 8U;
	*kmax = ((1U << m) - 1U - deg) / 8U;
	return true;
}

static inline unsigned int gf_mul(const struct bch_code *c, unsigned int a, unsigned int b)
{
	unsigned int n = (1U << c->m) - 1U, s;

	if (a == 0U || b == 0U) {
		return 0;
	}
	s = c->index_of[a] + c->index_of[b];
	return c->alpha_to[s >= n ? s - n : s];
}

static inline unsigned int gf_div(const struct bch_code *c, unsigned int a, unsigned int b)
{
	unsigned int n = (1U << c->m) - 1U, s;

	if (a == 0U) {
		return 0;
	}
	s = c->index_of[a] + n - c->index_of[b];
	return c->alpha_to[s >= n ? s - n : s];
}

int bch_init(struct bch_code *c, unsigned int m, unsigned int t)
{
	unsigned int n, r, rbytes, kmax, deg = 0, x = 1;
	/* The generator over GF(2^m) while it is multiplied up: coefficient of x^i. */
	uint16_t g[64U + 1U];
	uint64_t refl = 0;
	uint32_t seen[(BCH_N_MAX + 32U) / 32U];

	if (!bch_params(m, t, &r, &rbytes, &kmax)) {
		return -1;
	}
	n = (1U << m) - 1U;
	c->m = (uint8_t)m;
	c->t = (uint8_t)t;
	c->r = (uint8_t)r;
	c->rbytes = (uint8_t)rbytes;
	c->kmax = (uint16_t)kmax;

	for (unsigned int i = 0; i < n; i++) {
		c->alpha_to[i] = (uint16_t)x;
		c->index_of[x] = (uint16_t)i;
		x <<= 1;
		if ((x & (1U << m)) != 0U) {
			x ^= prim_poly[m];
		}
	}
	c->alpha_to[n] = 0;
	c->index_of[0] = (uint16_t)n;

	/* g(x): the product of (x + alpha^e) over the cosets of 1, 3, .., 2t - 1. */
	memset(g, 0, sizeof(g));
	g[0] = 1;
	memset(seen, 0, sizeof(seen));
	for (unsigned int i = 1; i < 2U * t; i += 2U) {
		unsigned int e = i;

		do {
			if ((seen[e / 32U] & (1U << (e % 32U))) == 0U) {
				unsigned int root = c->alpha_to[e];

				seen[e / 32U] |= 1U << (e % 32U);
				/* g = g (x + root) */
				deg++;
				for (unsigned int j = deg; j > 0U; j--) {
					g[j] = (uint16_t)(g[j - 1U] ^ gf_mul(c, g[j], root));
				}
				g[0] = (uint16_t)gf_mul(c, g[0], root);
			}
			e = (2U * e) % n;
		} while (e != i);
	}
	/* Binary coefficients; without the leading term, reflected: x^k at bit r - 1 - k. */
	for (unsigned int k = 0; k < r; k++) {
		if (g[k] != 0U) {
			refl |= (uint64_t)1U << (r - 1U - k);
		}
	}

	for (unsigned int b = 0; b < 256U; b++) {
		uint64_t v = b;

		for (unsigned int s = 1; s <= 32U; s++) {
			v = (v & 1U) != 0U ? (v >> 1) ^ refl : v >> 1;
			if (r > 32U) {
				if (s == 8U) {
					c->tab.t64[b] = v;
					break;
				}
			} else if (s % 8U == 0U) {
				c->tab.t32[s / 8U - 1U][b] = (uint32_t)v;
			}
		}
	}
	return 0;
}

/* The remainder of the @p k bytes' polynomial times x^r. */
BCH_HOT static uint64_t bch_rem(const struct bch_code *c, const uint8_t *p, size_t k)
{
	if (c->r > 32U) {
		uint64_t reg = 0;

		for (size_t i = 0; i < k; i++) {
			reg = (reg >> 8) ^ c->tab.t64[(reg ^ p[i]) & 0xffU];
		}
		return reg;
	}

	const uint32_t(*t)[256] = c->tab.t32;
	uint32_t reg = 0;

	/* Bytes to a word boundary, words (little endian: the first byte lowest), the rest. */
	while (k > 0U && ((uintptr_t)p & 3U) != 0U) {
		reg = (reg >> 8) ^ t[0][(reg ^ *p++) & 0xffU];
		k--;
	}
	for (; k >= 4U; k -= 4U, p += 4) {
		uint32_t w = reg ^ *(const uint32_t *)(const void *)p;

		reg = t[3][w & 0xffU] ^ t[2][(w >> 8) & 0xffU] ^ t[1][(w >> 16) & 0xffU] ^
		      t[0][w >> 24];
	}
	while (k-- > 0U) {
		reg = (reg >> 8) ^ t[0][(reg ^ *p++) & 0xffU];
	}
	return reg;
}

void bch_encode(const struct bch_code *c, const uint8_t *data, size_t k, uint8_t *parity)
{
	uint64_t reg = bch_rem(c, data, k);

	for (unsigned int q = 0; q < c->rbytes; q++) {
		parity[q] = (uint8_t)(reg >> (8U * q));
	}
}

int bch_decode(const struct bch_code *c, uint8_t *data, size_t k, uint8_t *parity)
{
	unsigned int n = (1U << c->m) - 1U, t = c->t, r = c->r, len = 8U * (unsigned int)k + r;
	/* The locator may grow to degree 2t on the way (more than t errors: rejected below). */
	unsigned int s_[2U * BCH_T_MAX], lam[2U * BCH_T_MAX + 1U], bb[2U * BCH_T_MAX + 1U],
		tmp[2U * BCH_T_MAX + 1U];
	unsigned int el = 0, mm = 1, bd = 1, nroots = 0, pos[BCH_T_MAX];
	unsigned int term[BCH_T_MAX + 1U];
	uint64_t rx = 0, s;

	for (unsigned int q = 0; q < c->rbytes; q++) {
		rx |= (uint64_t)parity[q] << (8U * q);
	}
	if (r < 64U) {
		rx &= ((uint64_t)1U << r) - 1U;
	}
	s = bch_rem(c, data, k) ^ rx;
	if (s == 0U) {
		return 0;
	}

	/* Syndromes S_j = s(alpha^j), j = 1 .. 2t (s_[j - 1]); the even ones as squares. */
	for (unsigned int j = 1; j <= 2U * t; j += 2U) {
		unsigned int acc = 0;

		for (unsigned int i = 0; i < r; i++) {
			if (((s >> i) & 1U) != 0U) {
				acc ^= c->alpha_to[(j * (r - 1U - i)) % n];
			}
		}
		s_[j - 1U] = acc;
	}
	for (unsigned int j = 2; j <= 2U * t; j += 2U) {
		s_[j - 1U] = gf_mul(c, s_[j / 2U - 1U], s_[j / 2U - 1U]);
	}

	/* Berlekamp-Massey: the error locator lam(x), degree el. */
	memset(lam, 0, sizeof(lam));
	memset(bb, 0, sizeof(bb));
	lam[0] = 1;
	bb[0] = 1;
	for (unsigned int i = 0; i < 2U * t; i++) {
		unsigned int d = s_[i];

		for (unsigned int j = 1; j <= el; j++) {
			d ^= gf_mul(c, lam[j], s_[i - j]);
		}
		if (d == 0U) {
			mm++;
			continue;
		}
		{
			unsigned int f = gf_div(c, d, bd);

			memcpy(tmp, lam, sizeof(tmp));
			for (unsigned int j = 0; j + mm <= 2U * t; j++) {
				lam[j + mm] ^= gf_mul(c, f, bb[j]);
			}
			if (2U * el <= i) {
				el = i + 1U - el;
				memcpy(bb, tmp, sizeof(bb));
				bd = d;
				mm = 1;
			} else {
				mm++;
			}
		}
	}
	if (el == 0U || el > t) {
		return -1;
	}

	/* Chien search over the block's degrees e = 0 .. len - 1: lam(alpha^-e) = 0. */
	for (unsigned int j = 0; j <= el; j++) {
		term[j] = c->index_of[lam[j]];
	}
	for (unsigned int e = 0; e < len; e++) {
		unsigned int sum = 0;

		for (unsigned int j = 0; j <= el; j++) {
			if (term[j] != n) {
				sum ^= c->alpha_to[term[j]];
				term[j] = term[j] >= j ? term[j] - j : term[j] + n - j;
			}
		}
		if (sum == 0U) {
			if (nroots == el) {
				return -1;
			}
			pos[nroots++] = e;
		}
	}
	if (nroots != el) {
		return -1;
	}
	for (unsigned int i = 0; i < nroots; i++) {
		/* Degree e is bit len - 1 - e in order: data, then parity. */
		unsigned int b = len - 1U - pos[i];

		if (b < 8U * (unsigned int)k) {
			data[b / 8U] ^= (uint8_t)(1U << (b % 8U));
		} else {
			b -= 8U * (unsigned int)k;
			parity[b / 8U] ^= (uint8_t)(1U << (b % 8U));
		}
	}
	return (int)nroots;
}
