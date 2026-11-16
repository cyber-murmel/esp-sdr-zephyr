/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Binary BCH codes over GF(2^m), m 5 to 11, correcting up to t bit errors per
 * block (t 1 to 4; t 1 is a Hamming code), shortened to whole data bytes: a
 * block is k data bytes and rbytes parity bytes holding the r = deg g parity
 * bits, 8 k + r <= 2^m - 1. Bits go LSB first like a reflected CRC: the encoder
 * and the decoder's check run the remainder register from tables, four bytes
 * per step while r <= 32 (slicing by 4); only a block whose remainder is not
 * zero gets syndromes, Berlekamp-Massey and a Chien search. Portable C, no
 * Zephyr dependencies.
 */

#ifndef LINK_BCH_H_
#define LINK_BCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BCH_M_MIN 5U
#define BCH_M_MAX 11U
#define BCH_T_MAX 4U
#define BCH_N_MAX ((1U << BCH_M_MAX) - 1U)

struct bch_code {
	uint8_t m, t;
	/* Parity bits (the generator's degree) and the bytes that hold them. */
	uint8_t r, rbytes;
	/* Most data bytes per block. */
	uint16_t kmax;
	/* GF(2^m): alpha_to[i] = alpha^i, index_of[x] = log x, index_of[0] = 2^m - 1. */
	uint16_t alpha_to[BCH_N_MAX + 1U], index_of[BCH_N_MAX + 1U];
	/*
	 * The remainder register after one byte from v = b (t32[0], r <= 32) and after
	 * two, three and four (slicing by 4); a byte at a time in 64 bits for r > 32.
	 */
	union {
		uint32_t t32[4][256];
		uint64_t t64[256];
	} tab;
};

/*
 * The parity bits and bytes and the most data bytes per block of the code
 * (m, t). Returns false for parameters out of range.
 */
bool bch_params(unsigned int m, unsigned int t, unsigned int *r, unsigned int *rbytes,
		unsigned int *kmax);

/* Set up @p c for (m, t). Returns 0, or -1 for parameters out of range. */
int bch_init(struct bch_code *c, unsigned int m, unsigned int t);

/* The rbytes parity bytes of @p k data bytes (k <= kmax). */
void bch_encode(const struct bch_code *c, const uint8_t *data, size_t k, uint8_t *parity);

/*
 * Correct a block in place. Returns the number of corrected bits (0 for a
 * clean block) or -1 if it is not correctable (left as received).
 */
int bch_decode(const struct bch_code *c, uint8_t *data, size_t k, uint8_t *parity);

#endif /* LINK_BCH_H_ */
