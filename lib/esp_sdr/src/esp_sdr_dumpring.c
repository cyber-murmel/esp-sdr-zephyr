/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gapless receive on the ESP32-S31 through the dump engine alone (esp_sdr_dumpring.h).
 *
 * A turn: while group 2 is the engine's, the writer fills a unit of UNIT words there and the
 * CPU, interrupts on, gives the sink what it has: the words the writer put into group 3 during
 * the turn before last, read where they are, and the last unit, copied out of group 2. Once the
 * unit is written, interrupts off and the other core parked, it hands the writer group 3, finds
 * the unit's ends, copies it out, marks group 2 for the next unit and hands the writer group 2
 * again. The writer never stops: its index runs on in either group, so group 3's words of a turn
 * run from where one unit ended to where the next one starts, both found in group 2.
 *
 * Marks: a sentinel no sample word has (their upper bits are the receiver's state). Before a unit
 * is written its start guard and end guard hold marks; afterwards the first word that is no mark
 * is its first and the first mark after its end its end. Group 3 needs none, which matters: the
 * CPU has some 16 cycles a word at 20 MS/s, and marking or scanning there cost it near 4.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_attr.h>
#include <esp_cpu.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_dumpring.h>

#include "esp_sdr_priv.h"

/* The writer's ring, in both groups: well below group 3's ROM data, inside the capture bank. */
#define RING        8192U
#define RING_MASK   (RING - 1U)
/* A unit in group 2, and how far past it the writer may get before the turn. */
#define UNIT        1536U
#define LATE_MAX    256U
/* The most words group 3 takes in a turn. */
#define TURN_MAX    768U
/* Group 2: marks from where the writer is in group 3 on, then around the next unit's end. */
#define START_GUARD 128U
#define END_LEAD    64U
#define END_GUARD   (END_LEAD + LATE_MAX + 64U)
/* The writer's first words are the engine settling. */
#define FIRST_WORD  256U
/* A unit copied out of group 2, at most. */
#define COPY_MAX    (UNIT + LATE_MAX + 64U)
/* Words a turn waits for those in flight to land before reading the group the writer left. */
#define SETTLE      16U
/* The writer silent this long: the run ends. */
#define STALL_US    2000U
#define MARK        0xa5c33c5aU

BUILD_ASSERT(RING * 4U <= 0x1A010U, "the ring fits the capture bank");
BUILD_ASSERT(2U * (UNIT + LATE_MAX + TURN_MAX) + START_GUARD + END_GUARD < RING,
	     "group 3's words wait a turn: the writer comes back to them a turn after that");

#define G2 ((volatile uint32_t *)__esp_sdr_group2_start)
#define G3 ((volatile uint32_t *)__esp_sdr_bank_start)

/* A unit lands at copy_buf + (its start & 3), so that both sides share their alignment. */
static uint32_t copy_buf[COPY_MAX + 3U] __aligned(16);
static atomic_t stop_req;
static bool running;
/* The run is on CPU 1, the S31's only core with PIE: copies and marks 4 words a load or store. */
static bool use_pie;
/* In DRAM, not flash: the turn reads it. */
static uint32_t marks4[4] __aligned(16) = {MARK, MARK, MARK, MARK};

static const struct {
	enum esp_sdr_rate rate;
	uint8_t code;
	uint32_t hz;
} rates[] = {
	{ESP_SDR_RATE_20MSPS, 3U, 20000000U},
	{ESP_SDR_RATE_10MSPS, 5U, 10000000U},
	{ESP_SDR_RATE_8MSPS, 7U, 8000000U},
	{ESP_SDR_RATE_4MSPS, 9U, 4000000U},
};

static ALWAYS_INLINE void own(uint32_t usage)
{
	REG_WRITE(DUMP_USAGE_REG, usage);
	__asm__ volatile("fence" ::: "memory");
}

static ALWAYS_INLINE uint32_t writer_at(void)
{
	return REG_READ(DUMP_MODE_REG) & RING_MASK;
}

static ALWAYS_INLINE uint32_t cycles(void)
{
	return (uint32_t)esp_cpu_get_cycle_count();
}

/*
 * Plain loads and stores, unrolled: about 2.4 CPU cycles a word copied, 3 a word marked, against 7
 * through a volatile pointer with the index masked (measured). The turn's fences keep them inside.
 */
static ALWAYS_INLINE void fill(uint32_t *d, uint32_t n)
{
	uint32_t k = 0U;

	for (; k + 8U <= n; k += 8U) {
		d[k] = MARK;
		d[k + 1U] = MARK;
		d[k + 2U] = MARK;
		d[k + 3U] = MARK;
		d[k + 4U] = MARK;
		d[k + 5U] = MARK;
		d[k + 6U] = MARK;
		d[k + 7U] = MARK;
	}
	for (; k < n; k++) {
		d[k] = MARK;
	}
}

static ALWAYS_INLINE void copy(uint32_t *d, const uint32_t *s, uint32_t n)
{
	uint32_t k = 0U;

	for (; k + 8U <= n; k += 8U) {
		uint32_t w0 = s[k], w1 = s[k + 1U], w2 = s[k + 2U], w3 = s[k + 3U];
		uint32_t w4 = s[k + 4U], w5 = s[k + 5U], w6 = s[k + 6U], w7 = s[k + 7U];

		d[k] = w0;
		d[k + 1U] = w1;
		d[k + 2U] = w2;
		d[k + 3U] = w3;
		d[k + 4U] = w4;
		d[k + 5U] = w5;
		d[k + 6U] = w6;
		d[k + 7U] = w7;
	}
	for (; k < n; k++) {
		d[k] = s[k];
	}
}

/*
 * PIE (CPU 1 only), as words: the SDK's assembler knows no esp.* (encodings from Espressif's,
 * -march=rv32imafc_xespv2p2). esp.vld.128.ip qN, a0, 16 and esp.vst.128.ip qN, a1, 16, q0 to q3;
 * esp.vld.128.ip q4, a2, 0 and esp.vst.128.ip q4, a1, 16. Both pointers 16-byte aligned. The turn
 * has interrupts off and nothing else on this core uses the q registers.
 */
static ALWAYS_INLINE void pie_copy16(uint32_t *d, const uint32_t *s, uint32_t blocks)
{
	register const uint32_t *src __asm__("a0") = s;
	register uint32_t *dst __asm__("a1") = d;

	for (uint32_t b = 0; b < blocks; b++) {
		__asm__ volatile(".word 0x0311009f\n.word 0x0311049f\n.word 0x0311089f\n"
				 ".word 0x03110c9f\n.word 0x8311809f\n.word 0x8311849f\n"
				 ".word 0x8311889f\n.word 0x83118c9f"
				 : "+r"(src), "+r"(dst)
				 :
				 : "memory");
	}
}

static ALWAYS_INLINE void pie_fill16(uint32_t *d, uint32_t blocks)
{
	register uint32_t *dst __asm__("a1") = d;
	register const uint32_t *m4 __asm__("a2") = marks4;

	__asm__ volatile(".word 0x0312101f" : : "r"(m4) : "memory");
	for (uint32_t b = 0; b < blocks; b++) {
		__asm__ volatile(".word 0x8311909f\n.word 0x8311909f\n.word 0x8311909f\n"
				 ".word 0x8311909f"
				 : "+r"(dst)
				 :
				 : "memory");
	}
}

/* Words to the next 16-byte boundary of p, at most n. */
static ALWAYS_INLINE uint32_t to_align(const void *p, uint32_t n)
{
	return MIN(n, ((16U - ((uintptr_t)p & 15U)) & 15U) / 4U);
}

/* As copy(), d and s with the same alignment: the middle 16 words at a time. */
static IRAM_ATTR __noinline void copy_fast(uint32_t *d, const uint32_t *s, uint32_t n)
{
	uint32_t head = to_align(s, n), blocks;

	if (!use_pie) {
		copy(d, s, n);
		return;
	}
	copy(d, s, head);
	d += head;
	s += head;
	n -= head;
	blocks = n / 16U;
	pie_copy16(d, s, blocks);
	copy(d + 16U * blocks, s + 16U * blocks, n - 16U * blocks);
}

static IRAM_ATTR __noinline void fill_fast(uint32_t *d, uint32_t n)
{
	uint32_t head = to_align(d, n), blocks;

	if (!use_pie) {
		fill(d, n);
		return;
	}
	fill(d, head);
	d += head;
	n -= head;
	blocks = n / 16U;
	pie_fill16(d, blocks);
	fill(d + 16U * blocks, n - 16U * blocks);
}

/* Marks over n words of a group's ring from at, around its end. */
static ALWAYS_INLINE void mark(volatile uint32_t *g, uint32_t at, uint32_t n)
{
	uint32_t m = MIN(n, RING - at);

	fill_fast((uint32_t *)&g[at], m);
	fill_fast((uint32_t *)g, n - m);
}

/*
 * The first word of [at, at + n) that is a mark (or not), for a range that turns exactly once:
 * UINT32_MAX when it starts turned or never turns.
 */
static ALWAYS_INLINE uint32_t turn_point(volatile uint32_t *g, uint32_t at, uint32_t n,
					 bool to_mark)
{
	uint32_t lo = 0U, hi = n - 1U;

	if ((g[at & RING_MASK] == MARK) == to_mark ||
	    (g[(at + hi) & RING_MASK] == MARK) != to_mark) {
		return UINT32_MAX;
	}
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2U;

		if ((g[(at + mid) & RING_MASK] == MARK) == to_mark) {
			hi = mid;
		} else {
			lo = mid + 1U;
		}
	}
	return (at + lo) & RING_MASK;
}

struct turn {
	/* In: where group 2's start guard begins; out: the next one's. */
	uint32_t expected;
	uint32_t cycles_per_word;
	bool first;
	/* Out: the unit's ends, its words at copy_buf + off, and the turn's cycles. */
	uint32_t start, end, n, off, dt;
};

/*
 * Group 3 to the writer and back; meanwhile nothing may read it, the ROM's data included: this
 * core's interrupts are off, the other core waits, and this code, its data and its stack are below
 * group 2. -EIO when the unit's ends are not where the marks say.
 */
static IRAM_ATTR __noinline int turn(struct turn *t)
{
	unsigned int key = esp_sdr_s31_group_take();
	uint32_t t0 = cycles(), at;
	const uint32_t *src = (const uint32_t *)__esp_sdr_group2_start;
	int ret = 0;

	own(DUMP_USAGE_OWN);
	while (cycles() - t0 < SETTLE * t->cycles_per_word) {
	}
	t->start = t->first ? FIRST_WORD : turn_point(G2, t->expected, START_GUARD, false);
	t->end = turn_point(G2, (t->expected + UNIT - END_LEAD) & RING_MASK, END_GUARD, true);
	t->n = (t->end - t->start) & RING_MASK;
	if (t->start == UINT32_MAX || t->end == UINT32_MAX || t->n > COPY_MAX) {
		ret = -EIO;
	} else {
		/* The unit out of group 2, around the ring's end; no library call in here. */
		uint32_t m = MIN(t->n, RING - t->start);

		t->off = t->start & 3U;
		copy_fast(&copy_buf[t->off], &src[t->start], m);
		copy_fast(&copy_buf[t->off + m], src, t->n - m);
	}
	/* The writer is in group 3 from here on until the turn back: the next unit starts later. */
	at = writer_at();
	mark(G2, at, START_GUARD);
	mark(G2, (at + UNIT - END_LEAD) & RING_MASK, END_GUARD);
	t->expected = at;
	own(DUMP_USAGE_DAC);
	t->dt = cycles() - t0;
	esp_sdr_s31_group_give(key);
	return ret;
}

/* Words at g[from], n of them around the ring's end, to the sink. */
static IRAM_ATTR void give(const struct esp_sdr_dumpring_cfg *cfg, volatile uint32_t *g,
			   uint32_t from, uint32_t n)
{
	while (n > 0U) {
		uint32_t m = MIN(n, RING - from);

		cfg->sink(cfg->user, (const uint32_t *)&g[from], m);
		from = (from + m) & RING_MASK;
		n -= m;
	}
}

/* In IRAM like its sink should be: the other core's work through the shared cache slows code in
 * flash (the turns would not wait for it, the units would). */
static IRAM_ATTR int run_locked(const struct esp_sdr_dumpring_cfg *cfg, uint32_t hz,
				unsigned int code, struct esp_sdr_dumpring_stats *st)
{
	struct turn t = {.expected = 0U, .cycles_per_word = ESP_SDR_CPU_HZ / hz, .first = true};
	const uint32_t stall = STALL_US * (ESP_SDR_CPU_HZ / 1000000U);
	/* Group 3's words of the turn before last, and where the last unit ended. */
	uint32_t g3_from = 0U, g3_n = 0U, last_end = 0U, copied = 0U;
	uint32_t prev, unit0;
	int ret;

	/* Every word a mark: group 2 has nothing of the writer's yet. */
	use_pie = arch_curr_cpu()->id == 1U;
	if (use_pie) {
		/* PIE on, on this core: Zephyr leaves its state CSR off. */
		__asm__ volatile("csrw 0x7f2, %0" : : "r"(1));
	}
	mark(G2, 0U, RING);
	ret = esp_sdr_s31_ring_start(code, RING);
	if (ret != 0) {
		return ret;
	}
	prev = cycles();
	unit0 = prev;
	for (;;) {
		uint32_t written, last = writer_at(), since = cycles();

		/* Group 2 is the writer's: group 3's words, then the last unit, to the sink. */
		if (g3_n > 0U) {
			give(cfg, G3, g3_from, g3_n);
			st->words += g3_n;
			g3_n = 0U;
		}
		if (copied > 0U) {
			cfg->sink(cfg->user, &copy_buf[t.off], copied);
			st->words += copied;
			copied = 0U;
		}
		/* By the clock, not the index: an index that went round the ring looks early. */
		if ((cycles() - unit0) / t.cycles_per_word > UNIT + LATE_MAX) {
			ret = -ENOBUFS;
			break;
		}
		do {
			uint32_t now = writer_at();

			written = (now - t.expected) & RING_MASK;
			if (now != last) {
				last = now;
				since = cycles();
			} else if (cycles() - since > stall) {
				ret = -ETIMEDOUT;
				break;
			}
		} while (written < UNIT && atomic_get(&stop_req) == 0);
		if (ret != 0 || atomic_get(&stop_req) != 0) {
			break;
		}
		if (written > UNIT + LATE_MAX) {
			ret = -ENOBUFS;
			break;
		}
		st->late_max = MAX(st->late_max, written - UNIT);
		ret = turn(&t);
		unit0 = cycles();
		st->cycles += unit0 - prev;
		prev = unit0;
		st->turns++;
		st->turn_cycles += t.dt;
		st->turn_cycles_max = MAX(st->turn_cycles_max, t.dt);
		if (ret != 0) {
			break;
		}
		if (!t.first) {
			/* The last turn's words: from the last unit's end to this one's start. */
			g3_from = last_end;
			g3_n = (t.start - last_end) & RING_MASK;
			if (g3_n == 0U || g3_n > TURN_MAX) {
				ret = -EIO;
				break;
			}
		}
		t.first = false;
		last_end = t.end;
		copied = t.n;
	}
	esp_sdr_s31_ring_stop();
	st->cycles += cycles() - prev;
	return ret;
}

int esp_sdr_dumpring_run(const struct esp_sdr_dumpring_cfg *cfg,
			 struct esp_sdr_dumpring_stats *stats)
{
	struct esp_sdr_dumpring_stats st = {0};
	int ret = -EINVAL;

	for (size_t k = 0; k < ARRAY_SIZE(rates) && cfg->sink != NULL; k++) {
		if (rates[k].rate != cfg->rate) {
			continue;
		}
		if (!esp_sdr_ready) {
			return -EAGAIN;
		}
		k_mutex_lock(&esp_sdr_lock, K_FOREVER);
		if (running) {
			k_mutex_unlock(&esp_sdr_lock);
			return -EBUSY;
		}
		running = true;
		atomic_set(&stop_req, 0);
		ret = run_locked(cfg, rates[k].hz, rates[k].code, &st);
		running = false;
		k_mutex_unlock(&esp_sdr_lock);
		break;
	}
	if (stats != NULL) {
		*stats = st;
	}
	return ret;
}

void esp_sdr_dumpring_stop(void)
{
	atomic_set(&stop_req, 1);
}
