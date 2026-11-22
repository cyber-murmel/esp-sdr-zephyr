/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The paced transmit's block fill (esp_sdr_paced_blk.c) against the per-word path it replaces,
 * copied here from esp_sdr_paced.c as the reference: the same DAC words for interpolation 1, 2, 4
 * and 8, filter designs and random coefficients, full-scale inputs (saturation), missing inputs,
 * blocks of random size and fill steps of random size between the pops, and the same counts of
 * samples and missing inputs at every point; a starved swap, stop and start again, rings of more
 * than two blocks (PBLOCKS) and block sizes that are not a multiple of 32 words (PWORDS 16, 40,
 * 128). The two sides run in turn on one thread here: the ordering between the cores is tested on
 * the board (tests/integration/esp_sdr_tx_paced_api).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_tx.h>

#include "esp_sdr_paced_blk.h"

#define TAPS 8U
#define N_IN 6000U
#define UPPER 0x40000000U

static uint32_t rng_state;

static uint32_t rnd(void)
{
	rng_state = rng_state * 1664525U + 1013904223U;
	return rng_state >> 8;
}

/* The inputs of one run, handed out in order to either path. */
struct src {
	int16_t i[N_IN + 4096U], q[N_IN + 4096U];
	bool have[N_IN + 4096U];
	uint32_t calls;
};

static bool src_input(void *user, int16_t *i, int16_t *q)
{
	struct src *s = user;
	uint32_t k = s->calls++;

	if (!s->have[k]) {
		return false;
	}
	*i = s->i[k];
	*q = s->q[k];
	return true;
}

/* esp_sdr_paced.c: the Blackman-windowed sinc, the same design. */
static void fir_design(int16_t coef[][TAPS], uint32_t l)
{
	uint32_t n = l * TAPS;
	double h[PBLK_MAX_L * TAPS], sum = 0.0;

	for (uint32_t k = 0; k < n; k++) {
		double x = ((double)k - (double)(n - 1U) / 2.0) / (double)l;

		h[k] = esp_sdr_sinc(x) * (double)esp_sdr_window(ESP_SDR_WINDOW_BLACKMAN, k, n - 1U);
		sum += h[k];
	}
	for (uint32_t ph = 0; ph < l; ph++) {
		for (uint32_t k = 0; k < TAPS; k++) {
			coef[ph][k] = (int16_t)lround(h[ph + k * l] * (double)l / sum * 32767.0);
		}
	}
}

/* The per-word path: its state and the body of esp_sdr_tx_paced_poll() after the store. */
struct ref {
	uint32_t ph, l, pos, upper, next;
	int16_t coef[PBLK_MAX_L][TAPS];
	int16_t hi[2 * TAPS], hq[2 * TAPS];
	uint64_t samples;
	uint32_t events;
	struct src *src;
};

static inline uint32_t iq10(int32_t i, int32_t q)
{
	return esp_sdr_tx_word((int16_t)CLAMP(i, -511, 511), (int16_t)CLAMP(q, -511, 511));
}

static void ref_init(struct ref *r, uint32_t l, int16_t coef[][TAPS], struct src *src)
{
	memset(r, 0, sizeof(*r));
	r->l = l;
	r->upper = UPPER;
	r->next = UPPER;
	r->src = src;
	memcpy(r->coef, coef, sizeof(r->coef));
}

static uint32_t ref_poll(struct ref *r)
{
	uint32_t out = r->next;
	const int16_t *c, *xi, *xq;
	int32_t ai, aq;

	if (r->ph == 0U) {
		int16_t i = 0, q = 0;

		if (!src_input(r->src, &i, &q)) {
			i = 0;
			q = 0;
			r->events++;
		}
		r->samples++;
		r->pos = r->pos == 0U ? TAPS - 1U : r->pos - 1U;
		r->hi[r->pos] = r->hi[r->pos + TAPS] = (int16_t)(i >> 6);
		r->hq[r->pos] = r->hq[r->pos + TAPS] = (int16_t)(q >> 6);
	}
	if (r->l == 1U) {
		r->next = r->upper | iq10(r->hi[r->pos], r->hq[r->pos]);
		return out;
	}
	c = r->coef[r->ph];
	xi = &r->hi[r->pos];
	xq = &r->hq[r->pos];
	ai = c[0] * xi[0] + c[1] * xi[1] + c[2] * xi[2] + c[3] * xi[3] + c[4] * xi[4] +
	     c[5] * xi[5] + c[6] * xi[6] + c[7] * xi[7];
	aq = c[0] * xq[0] + c[1] * xq[1] + c[2] * xq[2] + c[3] * xq[3] + c[4] * xq[4] +
	     c[5] * xq[5] + c[6] * xq[6] + c[7] * xq[7];
	r->next = r->upper | iq10(ai >> 15, aq >> 15);
	r->ph = (r->ph + 1U) & (r->l - 1U);
	return out;
}

enum kind { SINE, FULL, NOISE, STEPS };

static void src_make(struct src *s, enum kind kind, uint32_t miss_every)
{
	memset(s, 0, sizeof(*s));
	for (uint32_t k = 0; k < ARRAY_SIZE(s->i); k++) {
		switch (kind) {
		case SINE:
			s->i[k] = (int16_t)lround(20000.0 * sin(0.37 * k));
			s->q[k] = (int16_t)lround(20000.0 * cos(0.37 * k));
			break;
		case FULL:
			s->i[k] = (rnd() & 1U) ? 32767 : -32768;
			s->q[k] = (rnd() & 1U) ? 32767 : -32768;
			break;
		case NOISE:
			s->i[k] = (int16_t)rnd();
			s->q[k] = (int16_t)rnd();
			break;
		default:
			s->i[k] = (k / 7U) & 1U ? 32767 : -32768;
			s->q[k] = (k / 11U) & 1U ? -32768 : 32767;
			break;
		}
		s->have[k] = miss_every == 0U || rnd() % miss_every != 0U;
	}
}

/* The block after the playing one. */
static uint32_t next_block(const struct esp_sdr_paced_blk *b)
{
	return b->pb + 1U == PBLK_N ? 0U : b->pb + 1U;
}

/*
 * Pop @p words words from both paths, filling in random steps between the pops; where the swap
 * is next and the block is not ready, finish the fill first (a late fill would starve it: tested
 * on its own below).
 */
static void compare(uint32_t l, uint32_t words, int16_t coef[][TAPS], enum kind kind,
		    uint32_t miss_every, uint32_t n_words)
{
	static struct src sa, sb;
	static struct esp_sdr_paced_blk blk;
	struct ref ref;
	uint32_t next;

	src_make(&sa, kind, miss_every);
	sb = sa;
	ref_init(&ref, l, coef, &sa);
	esp_sdr_paced_blk_init(&blk, l, words, coef, UPPER, src_input, &sb);
	esp_sdr_paced_blk_prime(&blk);
	next = UPPER;
	for (uint32_t k = 0; k < n_words; k++) {
		uint64_t samples;
		uint32_t events, want = ref_poll(&ref);

		zassert_equal(next, want, "l %u words %u word %u: %08x, per-word path %08x", l, words,
			      k, next, want);
		if (blk.idx == blk.w) {
			while (esp_sdr_paced_blk_work(&blk) && blk.state[next_block(&blk)] != PBLK_READY) {
				esp_sdr_paced_blk_fill(&blk, 1U + rnd() % 9U);
			}
		}
		next = esp_sdr_paced_blk_pop(&blk);
		for (uint32_t m = rnd() % 4U; m > 0U; m--) {
			if (esp_sdr_paced_blk_work(&blk)) {
				esp_sdr_paced_blk_fill(&blk, 1U + rnd() % 9U);
			}
		}
		/* The counts as played: the input count follows the pops, not the fill. */
		if (rnd() % 64U == 0U) {
			esp_sdr_paced_blk_stats(&blk, &samples, &events);
			zassert_equal(samples, ref.samples, "samples %llu, %llu",
				      (unsigned long long)samples, (unsigned long long)ref.samples);
			zassert_equal(events, ref.events, "events %u, %u", events, ref.events);
			zassert_equal(esp_sdr_paced_blk_played(&blk), ref.samples, "played");
		}
	}
	zassert_equal(blk.starved, 0U, "starved");
}

static const uint32_t interps[] = {1, 2, 4, 8};

ZTEST(paced_blk, test_fir_design)
{
	int16_t coef[PBLK_MAX_L][TAPS];

	for (size_t a = 0; a < ARRAY_SIZE(interps); a++) {
		fir_design(coef, interps[a]);
		for (enum kind kind = SINE; kind <= STEPS; kind++) {
			for (uint32_t miss = 0; miss <= 5U; miss += 5U) {
				for (uint32_t words = 16; words <= PBLK_MAX_WORDS; words += 8) {
					rng_state = 1U + words + 100U * kind;
					compare(interps[a], words, coef, kind, miss, 700U);
				}
				rng_state = 7U + kind;
				compare(interps[a], PBLK_MAX_WORDS, coef, kind, miss, 3000U);
			}
		}
	}
}

/* Random coefficients over the whole range: the sums reach the limits of 32 bits' use. */
ZTEST(paced_blk, test_random_coefficients)
{
	int16_t coef[PBLK_MAX_L][TAPS];

	for (uint32_t trial = 0; trial < 40U; trial++) {
		uint32_t l = interps[trial % 4U];

		rng_state = 99U + trial;
		for (uint32_t ph = 0; ph < l; ph++) {
			for (uint32_t k = 0; k < TAPS; k++) {
				coef[ph][k] = trial % 3U == 0U ? ((rnd() & 1U) ? 32767 : -32768)
							       : (int16_t)rnd();
			}
		}
		compare(l, 8U * (1U + rnd() % (PBLK_MAX_WORDS / 8U)), coef, trial % 2U ? FULL : NOISE,
			trial % 4U == 0U ? 3U : 0U, 1500U);
	}
}

/*
 * A fill that does not keep up: the swap finds no block, the playing one is played again, and the
 * count says so; with steps again the stream goes on from the next block.
 */
ZTEST(paced_blk, test_starved_replays)
{
	static struct src s;
	static struct esp_sdr_paced_blk blk;
	int16_t coef[PBLK_MAX_L][TAPS];
	uint32_t last, want, nxt;

	fir_design(coef, 4);
	rng_state = 5;
	src_make(&s, NOISE, 0);
	esp_sdr_paced_blk_init(&blk, 4, 16, coef, UPPER, src_input, &s);
	esp_sdr_paced_blk_prime(&blk);
	/* Blocks 0 and 1 are filled, nothing fills the rest: they play, then block 1 again. */
	for (int k = 0; k < 32; k++) {
		(void)esp_sdr_paced_blk_pop(&blk);
	}
	zassert_equal(blk.starved, 0U);
	last = blk.pb;
	for (int k = 0; k < 16; k++) {
		zassert_equal(esp_sdr_paced_blk_pop(&blk), blk.words[last][k], "replay word %d", k);
		zassert_equal(blk.starved, 1U);
		zassert_equal(blk.pb, last);
	}
	/* The fill is back: the block after the replayed one plays next, the count stays. */
	while (blk.state[next_block(&blk)] != PBLK_READY) {
		esp_sdr_paced_blk_fill(&blk, 5U);
	}
	nxt = next_block(&blk);
	want = blk.words[nxt][0];
	zassert_equal(esp_sdr_paced_blk_pop(&blk), want, "first word of the next block");
	zassert_equal(blk.pb, nxt);
	zassert_equal(blk.starved, 1U);
	zassert_false(blk.replay);
	/* The replay does not count its inputs twice: two blocks of 4 inputs, one more taken. */
	zassert_equal(esp_sdr_paced_blk_played(&blk), 2U * 4U + 1U);
}

/* A poll before the first sync: the idle word, every swap a starved one, nothing played. */
ZTEST(paced_blk, test_unprimed_idles)
{
	static struct src s;
	static struct esp_sdr_paced_blk blk;
	int16_t coef[PBLK_MAX_L][TAPS];

	fir_design(coef, 2);
	src_make(&s, SINE, 0);
	esp_sdr_paced_blk_init(&blk, 2, 16, coef, UPPER, src_input, &s);
	for (int k = 0; k < 40; k++) {
		zassert_equal(esp_sdr_paced_blk_pop(&blk), UPPER);
	}
	zassert_equal(blk.starved, 3U);
	zassert_equal(esp_sdr_paced_blk_played(&blk), 0U);
	zassert_equal(s.calls, 0U, "no input was taken");
}

/* Stop in mid-block, start again with another setup: nothing of the first stream is left. */
ZTEST(paced_blk, test_stop_and_start_again)
{
	static struct src sa, sb;
	static struct esp_sdr_paced_blk blk;
	int16_t c8[PBLK_MAX_L][TAPS], c2[PBLK_MAX_L][TAPS];
	uint64_t samples;
	uint32_t events;

	fir_design(c8, 8);
	fir_design(c2, 2);
	rng_state = 11;
	src_make(&sa, NOISE, 3);
	esp_sdr_paced_blk_init(&blk, 8, MIN(64U, PBLK_MAX_WORDS), c8, UPPER, src_input, &sa);
	esp_sdr_paced_blk_prime(&blk);
	for (int k = 0; k < 150; k++) {
		if (blk.idx == blk.w) {
			while (blk.state[next_block(&blk)] != PBLK_READY) {
				esp_sdr_paced_blk_fill(&blk, 7U);
			}
		}
		(void)esp_sdr_paced_blk_pop(&blk);
	}
	zassert_equal(blk.state[blk.pb], PBLK_PLAYING);
	/* The second stream compares equal to the per-word path from its first word. */
	rng_state = 12;
	compare(2, MIN(64U, PBLK_MAX_WORDS), c2, SINE, 0, 500U);
	rng_state = 13;
	src_make(&sb, SINE, 0);
	esp_sdr_paced_blk_init(&blk, 2, MIN(64U, PBLK_MAX_WORDS), c2, UPPER, src_input, &sb);
	esp_sdr_paced_blk_stats(&blk, &samples, &events);
	zassert_equal(samples, 0U);
	zassert_equal(events, 0U);
	zassert_equal(blk.starved, 0U);
	for (uint32_t b = 0; b < PBLK_N; b++) {
		zassert_equal(blk.state[b], PBLK_FREE);
	}
}

ZTEST_SUITE(paced_blk, NULL, NULL, NULL, NULL, NULL);
