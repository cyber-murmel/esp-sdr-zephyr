/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Paced transmit, blocks ahead (esp_sdr_paced.c, doc/samples/paced.md): a ring of blocks of DAC
 * words, one played by the poll while the other core's fill thread fills the free ones. Free of
 * hardware: the unit test (tests/unit/paced_blk) runs it on native_sim against the per-word path.
 *
 * Two sides, no lock. The play side (the caller of the poll) owns pb, idx, replay, primed and
 * the counts; the fill side owns fb, the fill position, the input history and the miss maps of a
 * block it is filling. A block's state is the only thing both write: the fill side FREE to READY
 * (release, after the words), the play side READY to PLAYING and PLAYING to FREE (release, after
 * its last read). Both read it with acquire. Internal RAM has no cache between the cores.
 */

#ifndef ESP_SDR_PACED_BLK_H_
#define ESP_SDR_PACED_BLK_H_

#include <stdbool.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr_paced.h>

#ifndef CONFIG_ESP_SDR_PACED_BLOCK_WORDS
#define CONFIG_ESP_SDR_PACED_BLOCK_WORDS 128
#endif

#ifndef CONFIG_ESP_SDR_PACED_BLOCKS
#define CONFIG_ESP_SDR_PACED_BLOCKS 4
#endif

#define PBLK_TAPS      8U
#define PBLK_MAX_L     8U
#define PBLK_MAX_WORDS ((uint32_t)CONFIG_ESP_SDR_PACED_BLOCK_WORDS)
#define PBLK_N         ((uint32_t)CONFIG_ESP_SDR_PACED_BLOCKS)
/* Words of a miss map: a bit per input, at most one per word. */
#define PBLK_MISS_COLS ((PBLK_MAX_WORDS + 31U) / 32U)

enum { PBLK_FREE, PBLK_READY, PBLK_PLAYING };

struct esp_sdr_paced_blk {
	esp_sdr_tx_paced_input_t input;
	void *user;
	/* The DAC word's bits above I and Q, ORed into each word. */
	uint32_t upper;
	/* Interpolation, inputs and words a block. */
	uint32_t l, b, w;
	/* Fill side: the block being filled. */
	uint32_t fb;
	/* Fill position: inputs pulled, phase and first input of the next run. */
	uint32_t pulled, ph, n, wi;
	/* Shared: the state of each block, accessed atomically (acquire, release). */
	uint32_t state[PBLK_N];
	/* Fill side: missing inputs of a block, as a map and a count. */
	uint32_t miss_n[PBLK_N];
	uint32_t miss[PBLK_N][PBLK_MISS_COLS];
	/* Play side: words taken from the playing block, which one plays, and where the words are. */
	uint32_t idx, pb;
	const uint32_t *cur;
	/* What is played until the first play, and after a timed out prefill: the idle word. */
	uint32_t idle[PBLK_MAX_WORDS];
	/* The playing block is played again: the next was not ready at the swap. */
	bool replay;
	/* Block 0 plays (the first fill is done and its words are taken). */
	bool primed;
	/* Blocks not ready at the swap. */
	uint32_t starved;
	/* Inputs and missing inputs of the blocks played out. */
	uint64_t done_in;
	uint32_t done_miss;
	/* Coefficients oldest input first. */
	int16_t coef[PBLK_MAX_L][PBLK_TAPS];
	/* The inputs (full scale 511), in time order after 7 of the block before. */
	int16_t xi[PBLK_TAPS - 1U + PBLK_MAX_WORDS], xq[PBLK_TAPS - 1U + PBLK_MAX_WORDS];
	uint32_t words[PBLK_N][PBLK_MAX_WORDS];
};

/**
 * Set up (@p l 1, 2, 4 or 8; @p words a multiple of @p l, at most PBLK_MAX_WORDS; @p coef the
 * polyphase FIR, newest input first as esp_sdr_paced.c has it). Calls no input; the blocks hold
 * the idle word @p upper until esp_sdr_paced_blk_prefill() and esp_sdr_paced_blk_play().
 */
void esp_sdr_paced_blk_init(struct esp_sdr_paced_blk *s, uint32_t l, uint32_t words,
			    int16_t coef[][PBLK_TAPS], uint32_t upper,
			    esp_sdr_tx_paced_input_t input, void *user);

/** Fill side: fill blocks 0 and 1 (the input callback runs 2 x inputs a block times). */
void esp_sdr_paced_blk_prefill(struct esp_sdr_paced_blk *s);

/** Play side: block 0 plays from its first word (after the prefill). */
void esp_sdr_paced_blk_play(struct esp_sdr_paced_blk *s);

/** Both of the above on one core (the unit test). */
void esp_sdr_paced_blk_prime(struct esp_sdr_paced_blk *s);

/** Fill side. @return true when a fill step has something to do (a free block). */
static inline bool esp_sdr_paced_blk_work(const struct esp_sdr_paced_blk *s)
{
	return __atomic_load_n(&s->state[s->fb], __ATOMIC_ACQUIRE) == PBLK_FREE;
}

/**
 * Fill side. One fill step of up to @p units: that many input callbacks, or that many words of one
 * phase's run. Does nothing when there is no free block.
 */
void esp_sdr_paced_blk_fill(struct esp_sdr_paced_blk *s, uint32_t units);

/** Play side. The playing block ran out: the next one, or the same again (starved). */
uint32_t esp_sdr_paced_blk_swap(struct esp_sdr_paced_blk *s);

/** Play side. @return the next DAC word. */
static inline __attribute__((always_inline)) uint32_t
esp_sdr_paced_blk_pop(struct esp_sdr_paced_blk *s)
{
	if (__builtin_expect(s->idx == s->w, 0)) {
		(void)esp_sdr_paced_blk_swap(s);
	}
	return s->cur[s->idx++];
}

/** @return the input samples played so far (the per-word path's count of inputs taken). */
static inline uint64_t esp_sdr_paced_blk_played(const struct esp_sdr_paced_blk *s)
{
	uint32_t in = !s->primed ? 0U : (s->replay ? s->b : (s->idx + s->l - 1U) / s->l);

	return (uint64_t)s->done_in + (in < s->b ? in : s->b);
}

/** Input samples and missing ones played so far, as the per-word path counts them. */
void esp_sdr_paced_blk_stats(const struct esp_sdr_paced_blk *s, uint64_t *samples,
			     uint32_t *events);

#endif /* ESP_SDR_PACED_BLK_H_ */
