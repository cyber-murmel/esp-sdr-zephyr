/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Paced transmit, blocks ahead (esp_sdr_paced_blk.h). The fill runs phase by phase over a
 * block's inputs: the eight coefficients of a phase stay in registers, a word costs the loads
 * of two windows, 16 multiplies and the packing, and no state is kept between words.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_place.h>

#include "esp_sdr_paced_blk.h"

/* One word, each component clamped to +-511 and packed as esp_sdr_tx_word() does. */
static inline uint32_t pack(int32_t i, int32_t q)
{
	if (unlikely((uint32_t)(i + 511) > 1022U)) {
		i = i < 0 ? -511 : 511;
	}
	if (unlikely((uint32_t)(q + 511) > 1022U)) {
		q = q < 0 ? -511 : 511;
	}
	return ((uint32_t)i & 0x3ffU) | (((uint32_t)q & 0x3ffU) << 10);
}

/*
 * The word of phase @p ph at input @p n: the coefficients loaded where they are used, which keeps
 * the register pressure low enough to spend no callee-saved registers (a step of one word is
 * the common one: it fits in a gap between two stores).
 */
ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_paced_blk_word) __attribute__((optimize("O3")))
static void esp_sdr_paced_blk_word(struct esp_sdr_paced_blk *s, uint32_t ph, uint32_t n,
				   uint32_t wi)
{
	const int16_t *c = s->coef[ph];
	const int16_t *xi = &s->xi[n], *xq = &s->xq[n];
	int32_t ai = 0, aq = 0;

	/* Opaque to the optimizer: no hoisting of eight coefficients into eight saved registers. */
	__asm__ volatile("" : "+r"(c));
	for (uint32_t k = 0; k < PBLK_TAPS; k++) {
		ai += c[k] * xi[k];
		aq += c[k] * xq[k];
	}
	s->words[s->fb][wi] = s->upper | pack(ai >> 15, aq >> 15);
}

/* @p cnt words of phase @p ph from input @p n0 on, the coefficients in registers. */
ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_paced_blk_run) __attribute__((optimize("O3")))
static void esp_sdr_paced_blk_run(struct esp_sdr_paced_blk *s, uint32_t ph, uint32_t n0,
				  uint32_t cnt, uint32_t wi)
{
	const int16_t *c = s->coef[ph];
	const int32_t c0 = c[0], c1 = c[1], c2 = c[2], c3 = c[3], c4 = c[4], c5 = c[5], c6 = c[6],
		      c7 = c[7];
	const int16_t *xi = &s->xi[n0], *xq = &s->xq[n0];
	uint32_t *out = &s->words[s->fb][wi];
	const uint32_t upper = s->upper, l = s->l;

	for (uint32_t k = 0; k < cnt; k++) {
		int32_t ai = c0 * xi[0] + c1 * xi[1] + c2 * xi[2] + c3 * xi[3] + c4 * xi[4] +
			     c5 * xi[5] + c6 * xi[6] + c7 * xi[7];
		int32_t aq = c0 * xq[0] + c1 * xq[1] + c2 * xq[2] + c3 * xq[3] + c4 * xq[4] +
			     c5 * xq[5] + c6 * xq[6] + c7 * xq[7];

		*out = upper | pack(ai >> 15, aq >> 15);
		out += l;
		xi++;
		xq++;
	}
}

/* Inputs @p units more, from the callback: the words themselves when there is no interpolation. */
ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_paced_blk_pull) __attribute__((optimize("O3")))
static void esp_sdr_paced_blk_pull(struct esp_sdr_paced_blk *s, uint32_t units)
{
	const uint32_t b = s->b, fb = s->fb;
	uint32_t n = s->pulled, end = MIN(b, n + units);

	if (n == 0U) {
		for (uint32_t k = 0; k < ARRAY_SIZE(s->miss[fb]); k++) {
			s->miss[fb][k] = 0;
		}
		s->miss_n[fb] = 0;
	}
	for (; n < end; n++) {
		int16_t i = 0, q = 0;

		if (!s->input(s->user, &i, &q)) {
			i = 0;
			q = 0;
			s->miss[fb][n / 32U] |= 1U << (n % 32U);
			s->miss_n[fb]++;
		}
		/* Full scale 32767 to the DAC's 511. */
		s->xi[PBLK_TAPS - 1U + n] = (int16_t)(i >> 6);
		s->xq[PBLK_TAPS - 1U + n] = (int16_t)(q >> 6);
		if (s->l == 1U) {
			/* No interpolation: the input goes to the DAC as it is. */
			s->words[fb][n] = s->upper | pack(i >> 6, q >> 6);
		}
	}
	s->pulled = n;
}

ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_paced_blk_fill) void esp_sdr_paced_blk_fill(
	struct esp_sdr_paced_blk *s, uint32_t units)
{
	if (!esp_sdr_paced_blk_work(s)) {
		return;
	}
	if (s->pulled < s->b) {
		esp_sdr_paced_blk_pull(s, units);
	} else if (s->l > 1U && s->ph < s->l) {
		uint32_t cnt = MIN(units, s->b - s->n);

		if (cnt == 1U) {
			esp_sdr_paced_blk_word(s, s->ph, s->n, s->wi);
		} else {
			esp_sdr_paced_blk_run(s, s->ph, s->n, cnt, s->wi);
		}
		s->n += cnt;
		s->wi += cnt * s->l;
		if (s->n == s->b) {
			s->n = 0;
			s->ph++;
			s->wi = s->ph;
		}
	} else {
		/* Done, a step of its own: the next block's history is the last inputs of this one. */
		for (uint32_t k = 0; k < PBLK_TAPS - 1U; k++) {
			s->xi[k] = s->xi[s->b + k];
			s->xq[k] = s->xq[s->b + k];
		}
		/* The words and the miss map first, then the block is the play side's. */
		__atomic_store_n(&s->state[s->fb], PBLK_READY, __ATOMIC_RELEASE);
		s->fb = s->fb + 1U == PBLK_N ? 0U : s->fb + 1U;
		s->pulled = 0;
		s->ph = 0;
		s->n = 0;
		s->wi = 0;
	}
}

ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_paced_blk_swap) uint32_t esp_sdr_paced_blk_swap(
	struct esp_sdr_paced_blk *s)
{
	uint32_t nb = s->pb + 1U == PBLK_N ? 0U : s->pb + 1U;

	s->idx = 0;
	if (s->primed && __atomic_load_n(&s->state[nb], __ATOMIC_ACQUIRE) == PBLK_READY) {
		s->done_in += s->b;
		s->done_miss += s->miss_n[s->pb];
		s->replay = false;
		/* Our last read of the block is done: the fill side may take it. */
		__atomic_store_n(&s->state[s->pb], PBLK_FREE, __ATOMIC_RELEASE);
		__atomic_store_n(&s->state[nb], PBLK_PLAYING, __ATOMIC_RELAXED);
		s->pb = nb;
		s->cur = s->words[nb];
	} else {
		/* Not primed (no sync, or its prefill late): the idle block again. */
		s->starved++;
		s->replay = true;
	}
	return s->starved;
}

void esp_sdr_paced_blk_init(struct esp_sdr_paced_blk *s, uint32_t l, uint32_t words,
			    int16_t coef[][PBLK_TAPS], uint32_t upper,
			    esp_sdr_tx_paced_input_t input, void *user)
{
	memset(s, 0, sizeof(*s));
	s->input = input;
	s->user = user;
	s->upper = upper;
	s->l = l;
	s->w = words;
	s->b = words / l;
	for (uint32_t ph = 0; ph < l; ph++) {
		for (uint32_t k = 0; k < PBLK_TAPS; k++) {
			s->coef[ph][k] = coef[ph][PBLK_TAPS - 1U - k];
		}
	}
	for (uint32_t k = 0; k < PBLK_MAX_WORDS; k++) {
		s->idle[k] = upper;
	}
	s->cur = s->idle;
	/* The first pop is a swap, which replays the idle block until the play. */
	s->idx = s->w;
}

void esp_sdr_paced_blk_prefill(struct esp_sdr_paced_blk *s)
{
	while (__atomic_load_n(&s->state[0], __ATOMIC_ACQUIRE) != PBLK_READY ||
	       __atomic_load_n(&s->state[1], __ATOMIC_ACQUIRE) != PBLK_READY) {
		esp_sdr_paced_blk_fill(s, s->w);
	}
}

void esp_sdr_paced_blk_play(struct esp_sdr_paced_blk *s)
{
	s->pb = 0;
	s->idx = 0;
	s->cur = s->words[0];
	s->replay = false;
	__atomic_store_n(&s->state[0], PBLK_PLAYING, __ATOMIC_RELAXED);
	s->primed = true;
}

void esp_sdr_paced_blk_prime(struct esp_sdr_paced_blk *s)
{
	esp_sdr_paced_blk_prefill(s);
	esp_sdr_paced_blk_play(s);
}

void esp_sdr_paced_blk_stats(const struct esp_sdr_paced_blk *s, uint64_t *samples,
			     uint32_t *events)
{
	/* The per-word path takes an input with the first word of its phases. */
	uint32_t in = s->replay ? s->b : MIN(s->b, (s->idx + s->l - 1U) / s->l);
	uint32_t miss = 0;

	if (!s->primed) {
		in = 0;
	}

	for (uint32_t n = 0; n < in; n++) {
		miss += (s->miss[s->pb][n / 32U] >> (n % 32U)) & 1U;
	}
	*samples = (uint64_t)s->done_in + in;
	*events = s->done_miss + miss;
}
