/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sample ring's bookkeeping (apps/ubertooth/src/ubt_sring.h) against a model ring whose slots
 * remember the stream index last written to them. The model sink works as ring_sink() does: it
 * publishes wmax, records a hole, writes a call's samples, then publishes w; the reader reads w,
 * wmax and the holes' count in turn and copies in two halves, the sink free to act between any
 * two of these. The reader must never accept a slot that does not hold the index it stands for,
 * and its positions and gaps must add up to the stream.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "ubt_sring.h"

/* The model ring, and the most the sink writes in a call. */
#define RING     64U
#define INFLIGHT 8U

struct model {
	struct ubt_sring_holes h;
	struct ubt_sring_rd rd;
	uint32_t slot[RING];
	/* Sink: where it stops, what it published, a call written but not yet published. */
	uint32_t next;
	uint32_t w;
	uint32_t wmax;
	bool inflight;
	/* Reader: the session's next index (first == expect + gap), samples taken and skipped. */
	uint32_t expect;
	uint64_t taken;
	uint64_t skipped;
	uint32_t dropped;
};

static struct model m;

static void m_init(void)
{
	memset(&m, 0, sizeof(m));
	memset(m.slot, 0xff, sizeof(m.slot));
	ubt_sring_holes_reset(&m.h);
	ubt_sring_rd_reset(&m.rd);
}

/* The sink's call, first half: wmax, the hole before it if at is not where it stopped, slots. */
static void m_write(uint32_t at, uint32_t n)
{
	zassert_false(m.inflight);
	zassert_true(n <= INFLIGHT);
	m.wmax = at + n;
	if (at != m.next) {
		ubt_sring_hole_put(&m.h, m.next, at);
	}
	for (uint32_t i = at; i < at + n; i++) {
		m.slot[i % RING] = i;
	}
	m.next = at + n;
	m.inflight = true;
}

/* Second half: publish. */
static void m_publish(void)
{
	m.w = m.next;
	m.inflight = false;
}

static void m_put(uint32_t at, uint32_t n)
{
	m_write(at, n);
	m_publish();
}

/* Write [from, to) in calls of INFLIGHT. */
static void m_fill(uint32_t from, uint32_t to)
{
	while (from < to) {
		uint32_t n = MIN(INFLIGHT, to - from);

		m_put(from, n);
		from += n;
	}
}

static uint32_t m_count(void)
{
	return (uint32_t)atomic_get(&m.h.count);
}

/*
 * One read as the session does it: begin with w0, wmax0 and count as the reader read them, copy
 * the first half, let the sink act (mid), copy the rest, commit with wmax and w read then. Checks
 * every sample and the session's index; returns the samples taken, *gapp the gap reported.
 */
static uint32_t m_read_with(uint32_t w0, uint32_t wmax0, uint32_t count, uint32_t max,
			    void (*mid)(void), uint32_t *gapp)
{
	uint32_t copy[RING];
	uint32_t first = 0;
	uint32_t gap = 0;
	uint32_t n = ubt_sring_begin(&m.rd, &m.h, w0, wmax0, count, RING, max, &first, &gap);
	bool ok = false;

	zassert_true(n <= RING);
	for (uint32_t k = 0; k < n / 2U; k++) {
		copy[k] = m.slot[(first + k) % RING];
	}
	if (mid != NULL) {
		mid();
	}
	for (uint32_t k = n / 2U; k < n; k++) {
		copy[k] = m.slot[(first + k) % RING];
	}
	if (n != 0U) {
		ok = ubt_sring_commit(&m.rd, m.wmax, m.w, RING, first, n, &gap);
	}
	if (ok) {
		zassert_equal(first, m.expect + gap, "first %u, expected %u + gap %u", first,
			      m.expect, gap);
		for (uint32_t k = 0; k < n; k++) {
			zassert_equal(copy[k], first + k, "sample %u holds %u", first + k, copy[k]);
		}
		m.expect = first + n;
		m.taken += n;
	} else {
		/* A dropped chunk or a gap alone: the session's index moves on by the gap. */
		m.expect += gap;
		m.dropped += n != 0U ? 1U : 0U;
		n = 0;
	}
	m.skipped += gap;
	zassert_equal(m.expect, m.rd.r, "session at %u, reader at %u", m.expect, m.rd.r);
	if (gapp != NULL) {
		*gapp = gap;
	}
	return n;
}

static uint32_t m_read(uint32_t max, uint32_t *gapp)
{
	return m_read_with(m.w, m.wmax, m_count(), max, NULL, gapp);
}

ZTEST(ubertooth_core, test_sring_holes_in_order)
{
	uint32_t gap;

	m_init();
	/* A lap of the stream first, so the holes' slots hold old samples, as on the board. */
	m_fill(0, RING);
	zassert_equal(m_read(RING, &gap), RING);
	zassert_equal(gap, 0);
	/* Two holes before the reader reaches the first one. */
	m_fill(64, 70);
	m_put(80, 6);
	m_put(96, 6);
	zassert_equal(m_read(RING, &gap), 6);
	zassert_equal(gap, 0);
	zassert_equal(m_read(RING, &gap), 6, "the samples after the first hole");
	zassert_equal(gap, 10);
	zassert_equal(m_read(RING, &gap), 6, "the samples after the second hole");
	zassert_equal(gap, 10);
	zassert_equal(m_read(RING, &gap), 0);
	zassert_equal(m.rd.queued, 1, "the first hole was skipped with the second queued");
	zassert_equal(m.rd.lost, 0);
}

ZTEST(ubertooth_core, test_sring_hole_recorded_after_w)
{
	uint32_t gap;
	uint32_t w0;

	m_init();
	m_fill(0, 10);
	/* The reader reads w and wmax (10), the sink records the hole [10, 30), writes 30..35. */
	w0 = m.w;
	m_put(30, 5);
	zassert_equal(m_read_with(w0, w0, m_count(), RING, NULL, &gap), 10);
	zassert_equal(gap, 0);
	zassert_equal(m_read(RING, &gap), 5);
	zassert_equal(gap, 20);

	/* The same with the reader at the hole's start already: it skips, takes nothing yet. */
	m_init();
	m_fill(0, 10);
	zassert_equal(m_read(RING, &gap), 10);
	w0 = m.w;
	m_put(30, 5);
	zassert_equal(m_read_with(w0, w0, m_count(), RING, NULL, &gap), 0);
	zassert_equal(gap, 20, "the hole skipped");
	zassert_equal(m.rd.r, 30);
	zassert_equal(m_read(RING, &gap), 5);
	zassert_equal(gap, 0);
}

ZTEST(ubertooth_core, test_sring_lap)
{
	uint32_t gap;

	m_init();
	m_fill(0, 8);
	zassert_equal(m_read(RING, &gap), 8);
	/* The writer runs 100 samples ahead: the reader keeps three quarters of the ring. */
	m_fill(8, 108);
	zassert_equal(m_read(RING, &gap), RING - RING / 4U);
	zassert_equal(gap, 100 - (RING - RING / 4U));
	zassert_equal(m.rd.overrun, gap);
}

static void mid_unpublished(void)
{
	/* The sink writes a call and has not published it yet. */
	m_write(m.next, INFLIGHT);
}

static void mid_published(void)
{
	/* The sink writes and publishes a call while the reader copies. */
	m_put(m.next, INFLIGHT);
}

static void mid_far(void)
{
	/* A hole of 60 samples, and a call after it over the copy's last slots, unpublished. */
	m_write(m.next + 60U, INFLIGHT);
}

ZTEST(ubertooth_core, test_sring_writer_during_the_copy)
{
	uint32_t gap;

	m_init();
	m_fill(0, RING - INFLIGHT);
	/* An unpublished call behind the copy's oldest slot: the copy stands. */
	zassert_equal(m_read_with(m.w, m.wmax, m_count(), RING, mid_unpublished, &gap),
		      RING - INFLIGHT);
	zassert_equal(gap, 0);
	m_publish();
	/* The whole ring ahead, and the sink publishes a call over its oldest slots: dropped. */
	m_fill(m.next, m.rd.r + RING);
	zassert_equal(m_read_with(m.w, m.wmax, m_count(), RING, mid_published, &gap), 0);
	zassert_equal(gap, RING + INFLIGHT);
	zassert_equal(m.rd.r, m.w, "the reader skipped to the writer");

	/*
	 * After a hole the sink writes far beyond w: over the copy's last slots, while w still says
	 * they are 24 samples away. A check against w took them (sample 36 holding 100).
	 */
	m_init();
	m_fill(0, 40);
	zassert_equal(m_read_with(m.w, m.wmax, m_count(), RING, mid_far, &gap), 0);
	zassert_equal(gap, 40);
	m_publish();
	zassert_equal(m_read(RING, &gap), INFLIGHT);
	zassert_equal(gap, 60, "a lap to 60, then the hole to 100");
}

ZTEST(ubertooth_core, test_sring_records_lost)
{
	uint32_t gap;

	m_init();
	m_fill(0, 4);
	/* More holes than records before the reader comes: it drops the stream up to the writer. */
	for (uint32_t k = 0; k < UBT_SRING_HOLES + 1U; k++) {
		m_put(m.next + 2U, 2);
	}
	zassert_equal(m_read(RING, &gap), 0);
	zassert_equal(gap, m.w);
	zassert_equal(m.rd.lost, 1);
	m_fill(m.next, m.next + 5);
	zassert_equal(m_read(RING, &gap), 5);
	zassert_equal(gap, 0);
}

/* xorshift32: the same sequence on every run. */
static uint32_t rnd_state;

static uint32_t rnd(uint32_t n)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state % n;
}

/* A step of the sink: half a call (write, publish) at a time, a hole before one call in six. */
static void sink_step(void)
{
	if (m.inflight) {
		m_publish();
	} else {
		uint32_t hole = rnd(6) == 0U ? 1U + (rnd(8) == 0U ? rnd(3U * RING) : rnd(40)) : 0U;

		m_write(m.next + hole, 1U + rnd(INFLIGHT));
	}
}

static void mid_random(void)
{
	for (uint32_t k = rnd(4); k != 0U; k--) {
		sink_step();
	}
}

ZTEST(ubertooth_core, test_sring_random_interleaving)
{
	m_init();
	rnd_state = 0x2545f491U;
	for (uint32_t step = 0; step < 400000U; step++) {
		/* Stretches where the reader keeps up, and where it falls behind (laps, holes). */
		uint32_t lazy = (step / 2000U) % 3U == 2U ? 64U : 2U;

		if (rnd(lazy) != 0U) {
			sink_step();
		} else {
			uint32_t w0 = m.w;
			uint32_t wmax0;

			mid_random();
			wmax0 = m.wmax;
			mid_random();
			m_read_with(w0, wmax0, m_count(), 1U + rnd(RING + 8U),
				    rnd(2) != 0U ? mid_random : NULL, NULL);
		}
	}
	zassert_equal(m.taken + m.skipped, m.expect, "every index taken or skipped once");
	/* Every path ran. */
	zassert_true(m.rd.queued > 0U);
	zassert_true(m.rd.lost > 0U);
	zassert_true(m.rd.overrun > 0U);
	zassert_true(m.dropped > 0U);
	TC_PRINT("taken %llu, skipped %llu; queued holes %u, records lost %u, dropped copies %u\n",
		 (unsigned long long)m.taken, (unsigned long long)m.skipped, m.rd.queued, m.rd.lost,
		 m.dropped);
}
