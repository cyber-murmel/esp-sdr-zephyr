/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sample ring's bookkeeping between its writer (the ring sink on CPU 1) and its reader (the
 * mode thread on CPU 0), apart from the samples themselves: how far the writer is, the holes the
 * ring left in the stream, laps (ubt_radio.c). Header only: the hot reader inlines its common
 * path, and tests/unit/ubertooth_core runs it on native_sim.
 *
 * The writer publishes two positions: w, past the last sample written, and before it writes a
 * call's samples wmax, past the last one it is about to write (after a hole, far beyond w). Slots
 * of the stream before wmax minus the ring's length may hold newer samples already: the reader
 * checks a copy against wmax as read after it. Until 2026-11-12 it checked against w, and a copy
 * of the oldest slots taken while the sink wrote over them passed.
 *
 * A hole is a stretch [start, end) of the stream that the ring skipped: the start of every run (its
 * first unit begins a little after pair 0) and every abandoned unit. The sink records it before it
 * publishes the samples after it; the reader delivers what precedes the hole, then skips it. The
 * records form a queue: until 2026-11-12 there was a single one, and a second hole before the
 * reader reached the first replaced it, so the first hole's ring slots (the stream of one lap
 * before, or of the previous run) went out as samples.
 */

#ifndef UBT_SRING_H_
#define UBT_SRING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* Hole records, a power of 2: the reader takes a record while fewer than this many are newer. */
#define UBT_SRING_HOLES 8U

/* The sink's records: hole k in start/end[k % UBT_SRING_HOLES], published by count = k + 1. */
struct ubt_sring_holes {
	atomic_t count;
	atomic_t start[UBT_SRING_HOLES];
	atomic_t end[UBT_SRING_HOLES];
};

/* The reader's side. */
struct ubt_sring_rd {
	/* The next stream index to read, and the next hole record to take. */
	uint32_t r;
	uint32_t hole;
	/* Samples skipped because the writer lapped the reader. */
	uint64_t overrun;
	/* Holes skipped with a later one recorded already (a single record lost the earlier). */
	uint32_t queued;
	/* Times the sink reused records the reader had not taken (stream to the writer dropped). */
	uint32_t lost;
};

/* A new run, whose stream starts at 0: the sink's side, before the sink runs. */
static inline void ubt_sring_holes_reset(struct ubt_sring_holes *h)
{
	atomic_set(&h->count, 0);
}

/* A new run: the reader's side (its counters stay). */
static inline void ubt_sring_rd_reset(struct ubt_sring_rd *rd)
{
	rd->r = 0;
	rd->hole = 0;
}

/* The sink, the only writer: record the hole [start, end) before it publishes samples after it. */
static inline void ubt_sring_hole_put(struct ubt_sring_holes *h, uint32_t start, uint32_t end)
{
	uint32_t k = (uint32_t)atomic_get(&h->count);

	atomic_set(&h->start[k % UBT_SRING_HOLES], (atomic_val_t)start);
	atomic_set(&h->end[k % UBT_SRING_HOLES], (atomic_val_t)end);
	atomic_set(&h->count, (atomic_val_t)(k + 1U));
}

/*
 * Hole k, read as recorded unless the sink may have begun to reuse its record meanwhile: it does
 * once its count reaches k + UBT_SRING_HOLES.
 */
static inline bool ubt_sring_hole_get(const struct ubt_sring_holes *h, uint32_t k, uint32_t *start,
				      uint32_t *end)
{
	*start = (uint32_t)atomic_get(&h->start[k % UBT_SRING_HOLES]);
	*end = (uint32_t)atomic_get(&h->end[k % UBT_SRING_HOLES]);
	return (uint32_t)atomic_get(&h->count) - k < UBT_SRING_HOLES;
}

/*
 * The holes counted (count, read after w) from rd->hole on: skip those the reader has reached and
 * stop before the next one ahead. Returns the samples to take from rd->r on, at most avail.
 * Inline, in the reader's IRAM: out of line in flash it moved the flash code after it by 200 B,
 * and CPU 0 lapped two to three times as often (the instruction cache, doc/ubertooth.md).
 */
static inline uint32_t ubt_sring_take_holes(struct ubt_sring_rd *rd,
					    const struct ubt_sring_holes *h, uint32_t w,
					    uint32_t count, uint32_t avail, uint32_t *gap)
{
	while (rd->hole != count) {
		uint32_t start;
		uint32_t end;

		if (!ubt_sring_hole_get(h, rd->hole, &start, &end)) {
			/*
			 * The sink reused records the reader had not taken. Their holes lay
			 * before the newest one: drop the stream up to its end or to the
			 * writer, as for a lap.
			 */
			uint32_t to;

			if (!ubt_sring_hole_get(h, count - 1U, &start, &end)) {
				return 0;
			}
			to = (int32_t)(end - w) > 0 ? end : w;
			if ((int32_t)(to - rd->r) > 0) {
				*gap += to - rd->r;
				rd->r = to;
			}
			rd->hole = count;
			rd->lost++;
			return (int32_t)(w - rd->r) > 0 ? w - rd->r : 0U;
		}
		if ((int32_t)(start - rd->r) > 0) {
			/* Deliver what precedes the hole first. */
			return MIN(avail, start - rd->r);
		}
		if ((int32_t)(end - rd->r) > 0) {
			rd->queued += count - rd->hole > 1U ? 1U : 0U;
			*gap += end - rd->r;
			rd->r = end;
			/* The sink records a hole before it publishes w past its end. */
			avail = (int32_t)(w - rd->r) > 0 ? w - rd->r : 0U;
		}
		rd->hole++;
	}
	return avail;
}

/*
 * Before a copy, with the writer's w, wmax and the holes' count read in that order: the samples to
 * take now from rd->r on (0: none), at most max, with *first = rd->r and *gap the samples skipped
 * before it. ring: the ring's length in samples, less what a copy reads before rd->r.
 */
static inline uint32_t ubt_sring_begin(struct ubt_sring_rd *rd, const struct ubt_sring_holes *h,
				       uint32_t w, uint32_t wmax, uint32_t count, uint32_t ring,
				       size_t max, uint32_t *first, uint32_t *gap)
{
	uint32_t avail;
	uint32_t n;

	*gap = 0;
	if ((int32_t)(wmax - rd->r) > (int32_t)ring) {
		/*
		 * The writer lapped the reader: skip to where three quarters of the ring are left.
		 * A lap of the modes is a short stretch of load: skipping to the writer instead
		 * lost some 8100 samples a lap where this loses 2000, with as many laps (2 minute
		 * sniffs of a busy channel, 2026-11-12).
		 */
		uint32_t to = wmax - (ring - ring / 4U);

		*gap = to - rd->r;
		rd->overrun += *gap;
		rd->r = to;
	}
	/* Behind w after skipping a hole recorded after its reading of w: nothing yet. */
	avail = (int32_t)(w - rd->r) > 0 ? w - rd->r : 0U;
	if (count != rd->hole) {
		avail = ubt_sring_take_holes(rd, h, w, count, avail, gap);
	}
	n = MIN((uint32_t)max, avail);
	if (n != 0U) {
		*first = rd->r;
	}
	return n;
}

/*
 * After the copy of the stream from @p from on, with the writer's wmax and w read in that order
 * after it: if the writer may have written over the copy's oldest slots meanwhile, the chunk is
 * dropped as a gap up to w (one reading of the writer: the gap reported and the new read position
 * agree). Otherwise the n samples from rd->r on count as read.
 */
static inline bool ubt_sring_commit(struct ubt_sring_rd *rd, uint32_t wmax, uint32_t w,
				    uint32_t ring, uint32_t from, uint32_t n, uint32_t *gap)
{
	if (wmax - from > ring) {
		*gap += w - rd->r;
		rd->overrun += w - rd->r;
		rd->r = w;
		return false;
	}
	rd->r += n;
	return true;
}

#endif /* UBT_SRING_H_ */
