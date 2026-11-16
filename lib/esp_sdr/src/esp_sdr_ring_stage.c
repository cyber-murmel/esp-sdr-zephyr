/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio's output staged in internal RAM (esp_sdr_ring_stage.h): the S3's SRAM bank 2, the
 * S31's capture bank. Everything lives in the stage itself: its header (the counts, the hole
 * log, the sink's own state) and the words. The sink writes its claim (the count the put will
 * reach), the words, its position and then (after a barrier) the count; a reader keeps its own
 * count and word position, which agree, takes the newest count to see what is new and trusts
 * the words it read while the claim stays within a stage of them. Internal SRAM is not cached
 * on either, so the two CPUs see the same bytes.
 */

#include <stddef.h>
#include <string.h>

#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_ring_stage.h>

#ifdef ESP_SDR_STAGE_HOST
/* tests/host/test_ring_stage.py: the stage in the test's buffer, built with the host compiler. */
#define ALWAYS_INLINE                 inline __attribute__((always_inline))
#define ARG_UNUSED(x)                 (void)(x)
#define MIN(a, b)                     ((a) < (b) ? (a) : (b))
#define BUILD_ASSERT(c, m)            _Static_assert(c, m)
#define barrier_dmem_fence_full()     __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define STAGE_SPACE                   0x10000U
extern unsigned char esp_sdr_stage_host_bank[];
#define STAGE_BANK                    ((void *)esp_sdr_stage_host_bank)
#else
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <esp_attr.h>

#include "esp_sdr_priv.h"

#if defined(CONFIG_SOC_SERIES_ESP32S31)
/*
 * The capture bank (TCM group 3) past the dump ring's words there (its RING, 32 KiB, from the
 * bank's start). During a ring run group 2 is the engine's but for the turns, so the CPUs see
 * none of it (measured: reads give the engine's words, writes are lost); group 3 is theirs but
 * for the turns, which park the other core. No burst lands in the bank meanwhile. Not cached,
 * as the S3's banks are not.
 */
#define STAGE_OFFSET 0x8000U
#define STAGE_BANK   ((void *)((uintptr_t)__esp_sdr_bank_start + STAGE_OFFSET))
#define STAGE_SPACE  (0x1A010U - STAGE_OFFSET)
#else
/* Bank 2: the burst bank the ring leaves alone with spare_bank_app. */
#define STAGE_BANK  ((void *)(DUMP_BANK0_ADDR + 2U * DUMP_BANK_SIZE))
#define STAGE_SPACE DUMP_BANK_SIZE
#endif
#endif

#define STAGE_HOLES 32U

struct hole {
	/* The written count of the hole's first sample, and that sample's stream index. */
	uint32_t at, rsvd;
	uint64_t index;
};

struct stage {
	/*
	 * Samples written (modulo 2^32), hole log entries written, where the next word goes, and
	 * the count a put under way will reach (its words may be overwritten already).
	 */
	volatile uint32_t written, holes, wpos, claim;
	/* The sink's: the ring's next output index (before halving), the stream index of the next
	 * sample, the held half of a pair and its ring index. */
	uint8_t halve, have, fresh, rsvd;
	uint64_t expect, next, h_at;
	int16_t hi, hq;
	/* Counts esp_sdr_stage_init() calls: a reader of an earlier one starts over. */
	volatile uint32_t gen;
	struct hole hole[STAGE_HOLES];
	uint32_t w[ESP_SDR_STAGE_WORDS];
};

BUILD_ASSERT(sizeof(struct stage) <= STAGE_SPACE, "stage: larger than its room");

#define STAGE ((struct stage *)STAGE_BANK)

void esp_sdr_stage_init(bool halve)
{
	struct stage *st = STAGE;

	/*
	 * The new generation last: a reader that sees it sees the header ready (published first,
	 * a reader restarted on the old counts; one that meets the new counts under the old
	 * generation is caught by the generation's change before it trusts anything).
	 */
	memset(st, 0, offsetof(struct stage, gen));
	memset(st->hole, 0, sizeof(st->hole));
	st->halve = halve ? 1U : 0U;
	/* Sample 0 is stream index 0. */
	st->hole[0] = (struct hole){.at = 0U, .index = 0U};
	st->holes = 1U;
	barrier_dmem_fence_full();
	st->gen = st->gen + 1U;
	barrier_dmem_fence_full();
}

void esp_sdr_stage_run_start(void)
{
	struct stage *st = STAGE;

	st->have = 0U;
	st->fresh = st->written != 0U ? 1U : 0U;
}

uint32_t esp_sdr_stage_written(void)
{
	return STAGE->written;
}

/*
 * n samples whose first has (halved) ring index @p index into the words: from I and Q apart, or
 * (i NULL) from words that are I, Q pairs already (q then points at them).
 */
static ALWAYS_INLINE void put(struct stage *st, const int16_t *i, const int16_t *q, uint32_t n,
			      uint64_t index)
{
	uint32_t wr = st->written, wpos = st->wpos;

	if (st->fresh != 0U || index != st->expect) {
		/* A hole: the samples lost move the stream on (a new run: by one). */
		if (st->fresh == 0U && index > st->expect) {
			st->next += index - st->expect;
		} else {
			st->next += 1U;
		}
		st->fresh = 0U;
		st->hole[st->holes % STAGE_HOLES] = (struct hole){.at = wr, .index = st->next};
		barrier_dmem_fence_full();
		st->holes = st->holes + 1U;
	}
	st->expect = index + n;
	/* Before the words: a reader's advance() then knows them overwritten. */
	st->claim = wr + n;
	barrier_dmem_fence_full();
	for (uint32_t k = 0; k < n;) {
		uint32_t run = MIN(n - k, ESP_SDR_STAGE_WORDS - wpos);
		uint32_t *w = &st->w[wpos];

		if (i == NULL) {
			const uint32_t *src = (const uint32_t *)(const void *)q + k;

			for (uint32_t j = 0; j < run; j++) {
				w[j] = src[j];
			}
		} else {
			for (uint32_t j = 0; j < run; j++) {
				w[j] = (uint32_t)(uint16_t)i[k + j] |
				       ((uint32_t)(uint16_t)q[k + j] << 16);
			}
		}
		k += run;
		wpos += run;
		if (wpos == ESP_SDR_STAGE_WORDS) {
			wpos = 0U;
		}
	}
	st->next += n;
	st->wpos = wpos;
	barrier_dmem_fence_full();
	st->written = wr + n;
}

/* Samples averaged a call at most (on the ring thread's stack). */
#define HALF_CHUNK 64U
/* Words a put writes at most (the S31's decimator: GL_OUT): a reader closer to a lap stops. */
#define PUT_MAX 128U

ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_stage_sink)
void esp_sdr_stage_sink(void *user, const int16_t *i, const int16_t *q, size_t n,
				  uint64_t index)
{
	struct stage *st = STAGE;
	int16_t oi[HALF_CHUNK], oq[HALF_CHUNK];
	size_t k = 0;

	ARG_UNUSED(user);
	if (st->halve == 0U) {
		put(st, i, q, (uint32_t)n, index);
		return;
	}
	/* Samples k and k + 1 (k even) as one, their mean; a pair cut by a hole is dropped. */
	if (st->have != 0U && (n == 0U || index != st->h_at + 1U)) {
		st->have = 0U;
	}
	while (k < n) {
		uint64_t at = index + k;
		uint32_t m = 0;

		if (st->have != 0U) {
			oi[m] = (int16_t)(((int32_t)st->hi + i[k]) >> 1);
			oq[m] = (int16_t)(((int32_t)st->hq + q[k]) >> 1);
			m++;
			k++;
			at = st->h_at;
			st->have = 0U;
		} else if ((at & 1U) != 0U) {
			k++;
			continue;
		}
		for (; m < HALF_CHUNK && k + 1U < n; m++, k += 2U) {
			oi[m] = (int16_t)(((int32_t)i[k] + i[k + 1U]) >> 1);
			oq[m] = (int16_t)(((int32_t)q[k] + q[k + 1U]) >> 1);
		}
		if (m != 0U) {
			put(st, oi, oq, m, at / 2U);
		}
		if (k + 1U == n) {
			st->hi = i[k];
			st->hq = q[k];
			st->h_at = index + k;
			st->have = 1U;
			k++;
		}
	}
}

ESP_SDR_CODE(PACED_SAMPLE, esp_sdr_stage_put)
void esp_sdr_stage_put(const int16_t *iq, size_t n, uint64_t index)
{
	put(STAGE, NULL, iq, (uint32_t)n, index);
}

/*
 * The reader at @p back samples before the newest. Its index follows from its own place, whose
 * distances to the new place and to the holes it has not applied stay small however long the
 * run (a test against the written count alone went wrong once the newest hole was 2^31 samples
 * old): the newest such hole at or before the new place gives it, else it moves on by the
 * distance. When the log lost holes it had not applied, it goes to the oldest one kept (and laps
 * again at the next peek). Slot k of the log holds hole k until the sink logs hole
 * k + STAGE_HOLES, which it writes before it counts it: what was read stands while the count
 * stays below that afterwards.
 */
static void place(struct esp_sdr_stage_reader *r, uint32_t back)
{
	const struct stage *st = STAGE;

	for (;;) {
		uint32_t wr, h, first, pos, applied, lo;
		uint64_t index;

		if (st->gen != r->gen) {
			/* Started over meanwhile: the caller's next try restarts the reader. */
			return;
		}
		wr = st->written;
		barrier_dmem_fence_full();
		h = st->holes;
		first = h >= STAGE_HOLES ? h - STAGE_HOLES + 1U : 0U;
		back = MIN(back, wr - r->pos);
		pos = wr - back;
		if (r->holes < first) {
			const struct hole *e = &st->hole[first % STAGE_HOLES];

			lo = first;
			pos = e->at;
			index = e->index;
			applied = first + 1U;
		} else {
			uint32_t k;

			lo = r->holes;
			for (k = h; k > r->holes; k--) {
				if (st->hole[(k - 1U) % STAGE_HOLES].at - r->pos <= pos - r->pos) {
					break;
				}
			}
			if (k > r->holes) {
				const struct hole *e = &st->hole[(k - 1U) % STAGE_HOLES];

				index = e->index + (pos - e->at);
				applied = k;
			} else {
				index = r->index + (pos - r->pos);
				applied = r->holes;
			}
		}
		barrier_dmem_fence_full();
		if (st->holes - lo >= STAGE_HOLES) {
			continue;
		}
		/* Its word from its own place, which agrees with its count (the sink's wpos may
		 * already be a put ahead of the written count read). */
		r->rpos = (r->rpos + (pos - r->pos) % ESP_SDR_STAGE_WORDS) % ESP_SDR_STAGE_WORDS;
		r->pos = pos;
		r->index = index;
		r->holes = applied;
		return;
	}
}

/* A reader of the stage as esp_sdr_stage_init() left it: at its oldest sample. */
static void restart(struct esp_sdr_stage_reader *r)
{
	const uint32_t lapped = r->lapped;

	*r = (struct esp_sdr_stage_reader){.gen = STAGE->gen, .lapped = lapped};
	place(r, ESP_SDR_STAGE_WORDS);
}

void esp_sdr_stage_reader_init(struct esp_sdr_stage_reader *r)
{
	*r = (struct esp_sdr_stage_reader){0};
	restart(r);
}

uint32_t esp_sdr_stage_peek(struct esp_sdr_stage_reader *r, const uint32_t **w, uint64_t *index,
			    uint32_t max)
{
	const struct stage *st = STAGE;

	/* Again after a lap, or after a log slot the sink reused under the reader. */
	for (uint32_t tries = 0; tries < 3U; tries++) {
		uint32_t wr, avail, holes;
		bool reused = false;

		if (st->gen != r->gen) {
			/* The stage started over (esp_sdr_stage_init() again). */
			r->lapped++;
			restart(r);
		}
		wr = st->written;
		barrier_dmem_fence_full();
		avail = wr - r->pos;
		holes = st->holes;
		if (avail > ESP_SDR_STAGE_WORDS - PUT_MAX || holes - r->holes >= STAGE_HOLES) {
			/* Lapped (or too many holes to follow): on from half a stage back. */
			r->lapped++;
			place(r, ESP_SDR_STAGE_WORDS / 2U);
			continue;
		}
		/* Holes at this position: the index jumps; the next one ends the run. */
		while (r->holes < holes) {
			const struct hole *e = &st->hole[r->holes % STAGE_HOLES];
			const uint32_t to = e->at - r->pos;
			const uint64_t to_index = e->index;

			barrier_dmem_fence_full();
			if (st->holes - r->holes >= STAGE_HOLES) {
				reused = true;
				break;
			}
			if (to != 0U) {
				if (to < avail) {
					avail = to;
				}
				break;
			}
			r->index = to_index;
			r->holes++;
		}
		if (reused) {
			r->lapped++;
			place(r, ESP_SDR_STAGE_WORDS / 2U);
			continue;
		}
		avail = MIN(avail, MIN(max, ESP_SDR_STAGE_WORDS - r->rpos));
		*w = &st->w[r->rpos];
		*index = r->index;
		return avail;
	}
	return 0U;
}

bool esp_sdr_stage_advance(struct esp_sdr_stage_reader *r, uint32_t n)
{
	uint32_t wr;
	bool same;

	/* The words were read before the count that tells whether they still stood: the claim of
	 * a put under way, whose words may be overwritten already. */
	barrier_dmem_fence_full();
	wr = STAGE->claim;
	same = STAGE->gen == r->gen;
	r->pos += n;
	r->index += n;
	r->rpos += n;
	if (r->rpos >= ESP_SDR_STAGE_WORDS) {
		r->rpos -= ESP_SDR_STAGE_WORDS;
	}
	/* What was read is good while the sink has not come round to it (nor started over). */
	return same && wr - (r->pos - n) <= ESP_SDR_STAGE_WORDS;
}
