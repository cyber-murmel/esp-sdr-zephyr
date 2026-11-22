/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, position model (doc/dev/txs.md): from the CPU's 32 bit cycle counter to the
 * 64 bit index of the word the DAC engine plays now. Plain C, no Zephyr calls: runs on native_sim.
 *
 * The engine plays one word every cpw cycles (4, 8, 16 or 24, an exact divisor of the crystal
 * derived CPU clock), starting at the counter value t0 with the word of index idx0. The model adds
 * the counter's difference since the last call to a cycle remainder, so a wrap of the counter costs
 * nothing: no per-call branch, no division (cpw is 2^s or 3 * 2^s). The counter must be read at
 * least once in 2^32 cycles (13 s at 320 MHz); the CPU1 thread reads it every chunk.
 */

#ifndef TXS_POS_H_
#define TXS_POS_H_

#include <stdbool.h>
#include <stdint.h>

/* Words of a group: the unit of the flip. Two groups make the bank. */
#define TXS_GROUP_SHIFT 15U
#define TXS_GROUP_WORDS (1U << TXS_GROUP_SHIFT)
#define TXS_BANK_WORDS  (2U * TXS_GROUP_WORDS)

struct txs_pos {
	uint32_t cpw;
	/* cpw = (m3 ? 3 : 1) << shift */
	uint8_t shift;
	bool m3;
	/* Counter at the last update and the cycles gone into the word playing then. */
	uint32_t last;
	uint32_t rem;
	/* Index of the word playing at `last`. */
	uint64_t idx;
};

/** @return true for a cycles per word value the model takes (4, 8, 16, 24). */
static inline bool txs_cpw_ok(uint32_t cpw)
{
	return cpw == 4U || cpw == 8U || cpw == 16U || cpw == 24U;
}

/**
 * Start the model: word idx0 starts playing at counter value t0.
 * @retval 0 on success, -1 for a bad cpw.
 */
static inline int txs_pos_init(struct txs_pos *p, uint32_t t0, uint32_t cpw, uint64_t idx0)
{
	if (!txs_cpw_ok(cpw)) {
		return -1;
	}
	p->cpw = cpw;
	p->m3 = (cpw % 3U) == 0U;
	p->shift = (uint8_t)__builtin_ctz(cpw);
	p->last = t0;
	p->rem = 0;
	p->idx = idx0;
	return 0;
}

/**
 * Bring the model up to the counter value `now`, never backwards.
 * @return the index of the word playing at `now`.
 */
static inline uint64_t txs_pos_update(struct txs_pos *p, uint32_t now)
{
	/* All in 32 bits (RV32: no 64 bit shifts or multiplies): the sum rem + delta may carry out
	 * once, and the new remainder (below cpw) is right modulo 2^32.
	 */
	const uint32_t d = now - p->last;
	const uint32_t u = p->rem + d;
	uint32_t q = (u >> p->shift) | (u < d ? 1U << (32U - p->shift) : 0U);

	if (p->m3) {
		/* q below 2^30 (shift is 3): the multiply is exact */
		q = (uint32_t)(((uint64_t)q * 0xAAAAAAABULL) >> 33);
	}
	p->idx += q;
	p->rem = u - q * p->cpw;
	p->last = now;
	return p->idx;
}

/**
 * @return the index of the first word that starts at or after the last update: the one that sees
 * a change made now (txs_flip_done()'s play), the playing one if a word starts exactly now.
 */
static inline uint64_t txs_pos_word_after(const struct txs_pos *p)
{
	return p->idx + (p->rem != 0U ? 1U : 0U);
}

/** @return the group (0 or 1) that plays word index idx. */
static inline uint32_t txs_group_of(uint64_t idx)
{
	return (uint32_t)(idx >> TXS_GROUP_SHIFT) & 1U;
}

/** @return the bank slot (word offset in the two groups) of index idx. */
static inline uint32_t txs_slot_of(uint64_t idx)
{
	return (uint32_t)idx & (TXS_BANK_WORDS - 1U);
}

/** @return the index of the first word of the group after the one playing at idx. */
static inline uint64_t txs_next_flip(uint64_t idx)
{
	return ((idx >> TXS_GROUP_SHIFT) + 1U) << TXS_GROUP_SHIFT;
}

/** @return cycles from the last update until word `idx` starts (negative: it started). */
static inline int64_t txs_pos_cycles_to(const struct txs_pos *p, uint64_t idx)
{
	const uint64_t du = idx - p->idx;
	const int32_t lo = (int32_t)du;

	/* within 2^26 words (3 s at 20 MS/s) a 32 bit multiply is exact: no 64 bit multiply */
	if ((int64_t)lo == (int64_t)du && lo > -(1 << 26) && lo < (1 << 26)) {
		return (int64_t)(lo * (int32_t)p->cpw) - (int64_t)p->rem;
	}
	return (int64_t)du * (int64_t)p->cpw - (int64_t)p->rem;
}

/** @return the counter value (low 32 bits) at which word `idx` starts. */
static inline uint32_t txs_pos_counter_at(const struct txs_pos *p, uint64_t idx)
{
	return p->last + (uint32_t)txs_pos_cycles_to(p, idx);
}

#endif /* TXS_POS_H_ */
