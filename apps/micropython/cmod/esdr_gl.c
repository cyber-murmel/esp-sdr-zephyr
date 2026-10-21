/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gapless stream's channelizer (esdr_gl.h). The parser hands each segment block to
 * gl_block(), which copies it into a pool (or drops it when the thread is behind); the thread
 * pushes its segments through esdr_pfbq and esdr_detq, adds every decided block's channel
 * powers to the running sums and turns the detector's openings and closings into bursts.
 * MicroPython takes the sums and the bursts (esdr_radio.c).
 *
 * The thread runs the P4's PIE kernels (esdr_pfbq_pie.S); no other thread here uses PIE, whose
 * state is in no thread context.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "esdr_gl.h"
#include "esdr_pfbq.h"
#include "esdr_proto.h"
#include "esdr_remote.h"
#include "esdr_seg.h"

#define GL_POOL  8U
#define GL_STACK 3072
/* Above MicroPython (10): the radio's link budget bounds the work (esdr_radio.c's default). */
#define GL_PRIO  9
/* No block for this long: the stream stopped, its last quad and block decide. */
#define GL_IDLE_MS 20
/* Detector blocks of 16 frames; output at 32 times the raw units (as the bridge's). */
#define GL_BLOCK 16U
#define GL_GAIN  32.0f
/* A full-scale carrier (512 raw units) at the output, dB: 20 log10(512 * 32). */
#define GL_FS_DB 84.2884f
/*
 * A hole in the stream longer than 0.5 ms ends every burst: the radio kept nothing there, so
 * nothing was loud (the detector itself sees no quiet blocks in a hole and would keep the
 * channel open across it). Shorter holes come from the radio's budget inside a long burst (its
 * threshold servo rising past it for a while), as do holes it dropped kept windows in (its
 * staging ring was full: the next segment says so; the link drains the ring within
 * milliseconds): a hole after drops ends bursts only past 5 ms.
 */
#define GL_HOLE_PAIRS      40000U
#define GL_DROP_HOLE_PAIRS 400000U

#define PSRAM __attribute__((section(".ext_ram.bss")))

static uint8_t pool[GL_POOL][ESDR_BLOCK_BYTES] __aligned(16) PSRAM;
K_MSGQ_DEFINE(gl_free, sizeof(uint8_t), GL_POOL, 1);
K_MSGQ_DEFINE(gl_full, sizeof(uint8_t), GL_POOL, 1);

/*
 * The bank and the detector, for up to 80 channels: in PSRAM, as the pool (the SRAM is LVGL's);
 * their working set of some 40 KiB stays in the cache.
 */
static uint8_t mem[48U * 1024U] __aligned(16) PSRAM;

/* Held by the thread while it works on a block, by start and stop while they change things. */
static K_MUTEX_DEFINE(gl_lock);

static struct {
	struct esdr_pfbq pfb;
	struct esdr_detq det;
	struct esdr_seg_track track;
	unsigned int m;
	bool run, have_ref;
	uint32_t decided;
	/* The first frame seen (bursts' start times count from it). */
	uint64_t m_ref;
	/* Per channel while in a burst: the first block's first frame, the end of its last loud
	 * block, its loudest block. */
	uint64_t start[ESDR_PFB_M_MAX], end[ESDR_PFB_M_MAX];
	float peak[ESDR_PFB_M_MAX];
	bool in_burst[ESDR_PFB_M_MAX];
	/* The frame after the last quad, to see holes; the segment being pushed follows drops. */
	uint64_t next_m;
	bool have_next, after_drops;
} gl;

/* What MicroPython takes, under acc_lock: running sums since its last look, the bursts. */
static struct k_spinlock acc_lock;
static struct {
	uint32_t n;
	float sum[ESDR_PFB_M_MAX], big[ESDR_PFB_M_MAX], floor[ESDR_PFB_M_MAX];
	uint32_t open[ESDR_PFB_M_MAX];
	struct esdr_gl_burst burst[ESDR_GL_BURSTS];
	unsigned int first, count;
	struct esdr_gl_stats st;
} acc;

K_THREAD_STACK_DEFINE(gl_stack, GL_STACK);
static struct k_thread gl_thread;
static bool started;

static inline uint32_t mcycle(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, mcycle" : "=r"(v));
	return v;
}

static inline float db(float p)
{
	return 10.0f * log10f(p + 1e-12f) - GL_FS_DB;
}

/* Words, four at a time: the P4's memcpy() is the ROM's, some 5.5 cycles a byte. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void copy_block(uint32_t *dst, const uint32_t *src)
{
	for (size_t i = 0; i < ESDR_BLOCK_BYTES / 4U; i += 4U) {
		uint32_t a = src[i], b = src[i + 1U], c = src[i + 2U], d = src[i + 3U];

		dst[i] = a;
		dst[i + 1U] = b;
		dst[i + 2U] = c;
		dst[i + 3U] = d;
	}
}

/* The parser's hook (esdr_remote_gl_hook()): into a free pool slot, else dropped. */
static void gl_block(const uint8_t *blk)
{
	uint8_t i;

	if (k_msgq_get(&gl_free, &i, K_NO_WAIT) != 0) {
		acc.st.dropped++;
		return;
	}
	copy_block((uint32_t *)(void *)pool[i], (const uint32_t *)(const void *)blk);
	(void)k_msgq_put(&gl_full, &i, K_NO_WAIT);
}

static void push_burst(unsigned int k, uint64_t from, uint64_t to, float peak)
{
	const float frame_s = (float)(gl.m / 2U) / (float)ESDR_GAPLESS_RATE_HZ;
	const int off = (int)k - (k > gl.m / 2U ? (int)gl.m : 0);
	struct esdr_gl_burst b = {
		.offset_hz = (float)off * (float)ESDR_GAPLESS_RATE_HZ / (float)gl.m,
		.start_s = (float)(int64_t)(from - gl.m_ref) * frame_s,
		.length_s = (float)(to - from) * frame_s,
		.peak_db = db(peak),
	};
	k_spinlock_key_t key = k_spin_lock(&acc_lock);

	if (acc.count == ESDR_GL_BURSTS) {
		acc.first = (acc.first + 1U) % ESDR_GL_BURSTS;
		acc.count--;
		acc.st.bursts_lost++;
	}
	acc.burst[(acc.first + acc.count++) % ESDR_GL_BURSTS] = b;
	acc.st.bursts++;
	k_spin_unlock(&acc_lock, key);
}

/*
 * A channel with frames to send, at the end of the block that starts at d->cur_start: a burst
 * runs from the block that opened it to the end of its last loud block (not the hang after it,
 * nor a stretch the radio kept nothing of).
 */
static void det_cb(const struct esdr_detq *d, const struct esdr_detq_ev *ev, void *user)
{
	const unsigned int k = ev->chan;
	const bool loud = ev->power > ev->floor * d->ratio;

	ARG_UNUSED(user);
	if ((ev->flags & ESDR_DET_OPEN) != 0U || (!gl.in_burst[k] && loud)) {
		/* Opened, or loud again after a hole ended the burst while it stayed open. */
		gl.start[k] = d->cur_start;
		gl.end[k] = d->cur_start + d->b;
		gl.peak[k] = ev->power;
		gl.in_burst[k] = true;
	} else if (loud) {
		gl.end[k] = d->cur_start + d->b;
		gl.peak[k] = MAX(gl.peak[k], ev->power);
	}
	if ((ev->flags & ESDR_DET_CLOSE) != 0U && gl.in_burst[k]) {
		push_burst(k, gl.start[k], gl.end[k], gl.peak[k]);
		gl.in_burst[k] = false;
	}
}

/* A hole in the stream before frame @p m0: the bursts end at their last loud block. */
static void hole(uint64_t m0)
{
	const uint64_t pairs = m0 > gl.next_m ? (m0 - gl.next_m) * (gl.m / 2U) : 0U;

	if (gl.have_next &&
	    pairs > (gl.after_drops ? GL_DROP_HOLE_PAIRS : GL_HOLE_PAIRS)) {
		for (unsigned int k = 0; k < gl.m; k++) {
			if (gl.in_burst[k]) {
				push_burst(k, gl.start[k], gl.end[k], gl.peak[k]);
				gl.in_burst[k] = false;
			}
		}
	}
	gl.next_m = m0 + ESDR_PFBQ_FRAMES;
	gl.have_next = true;
}

/* A block decided since the last look: its powers, the floors, who is open, into the sums. */
static void account(void)
{
	uint32_t n = esdr_detq_decided(&gl.det, NULL);
	const float *pw, *fl;
	const uint8_t *op;
	k_spinlock_key_t key;

	if (n == gl.decided) {
		return;
	}
	gl.decided = n;
	pw = esdr_detq_power(&gl.det);
	fl = esdr_detq_floor(&gl.det);
	op = esdr_detq_open(&gl.det);
	key = k_spin_lock(&acc_lock);
	acc.n++;
	for (unsigned int k = 0; k < gl.m; k++) {
		acc.sum[k] += pw[k];
		acc.big[k] = MAX(acc.big[k], pw[k]);
		acc.floor[k] = fl[k];
		acc.open[k] += op[k];
	}
	k_spin_unlock(&acc_lock, key);
}

static void quad_cb(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	ARG_UNUSED(user);
	/* After the detector: a quad in a new block decides the block before it first. */
	esdr_detq_quad(&gl.det, m0, iq, valid, complete, det_cb, NULL);
	account();
	hole(m0);
}

static int seg_cb(const struct esdr_seg *seg, void *user)
{
	ARG_UNUSED(user);
	/* A segment that goes back (the radio started again) starts the bank anew. */
	if (esdr_seg_track_gap(&gl.track, seg) == ESDR_SEG_GAP_BACK) {
		esdr_pfbq_reset(&gl.pfb);
	}
	if (!gl.have_ref) {
		gl.m_ref = seg->index / (gl.m / 2U);
		gl.have_ref = true;
	}
	acc.st.pairs += seg->pairs;
	gl.after_drops = (seg->flags & ESDR_SEG_DROPPED) != 0U;
	(void)esdr_pfbq_push(&gl.pfb, seg->index, seg->iq, seg->pairs, seg->shift, seg->dc_i,
			     seg->dc_q, quad_cb, NULL);
	return 0;
}

static void gl_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		uint8_t i;

		if (k_msgq_get(&gl_full, &i, K_MSEC(GL_IDLE_MS)) != 0) {
			k_mutex_lock(&gl_lock, K_FOREVER);
			if (gl.run) {
				(void)esdr_pfbq_flush(&gl.pfb, quad_cb, NULL);
				esdr_detq_flush(&gl.det, det_cb, NULL);
				account();
			}
			k_mutex_unlock(&gl_lock);
			continue;
		}
		k_mutex_lock(&gl_lock, K_FOREVER);
		if (gl.run) {
			uint32_t t0 = mcycle();

			(void)esdr_seg_parse(pool[i], ESDR_BLOCK_BYTES, seg_cb, NULL);
			acc.st.blocks++;
			acc.st.cycles += mcycle() - t0;
		}
		k_mutex_unlock(&gl_lock);
		(void)k_msgq_put(&gl_free, &i, K_NO_WAIT);
	}
}

int esdr_gl_start(unsigned int channels, float open_db)
{
	const struct esdr_pfbq_cfg pc = {
		.channels = (uint16_t)channels,
		.quads = ESDR_DETQ_QUADS(GL_BLOCK),
		.gain = GL_GAIN,
	};
	const struct esdr_detq_cfg dc = {
		.channels = (uint16_t)channels,
		.block = GL_BLOCK,
		.open_ratio = powf(10.0f, open_db / 10.0f),
	};
	size_t pn = esdr_pfbq_mem_size(&pc), dn = esdr_detq_mem_size(&dc);
	k_spinlock_key_t key;
	int ret;

	if (pn == 0U || dn == 0U || ROUND_UP(pn, 16U) + dn > sizeof(mem) || !(open_db > 0.0f)) {
		return -EINVAL;
	}
	esdr_gl_stop();
	if (!started) {
		for (uint8_t i = 0; i < GL_POOL; i++) {
			(void)k_msgq_put(&gl_free, &i, K_NO_WAIT);
		}
		k_thread_create(&gl_thread, gl_stack, K_THREAD_STACK_SIZEOF(gl_stack), gl_fn, NULL,
				NULL, NULL, GL_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&gl_thread, "channelizer");
		started = true;
	}
	k_mutex_lock(&gl_lock, K_FOREVER);
	ret = esdr_pfbq_init(&gl.pfb, &pc, mem, pn);
	if (ret == 0) {
		ret = esdr_detq_init(&gl.det, &dc, mem + ROUND_UP(pn, 16U), dn);
	}
	if (ret == 0) {
		gl.m = channels;
		gl.decided = 0;
		gl.have_ref = false;
		gl.have_next = false;
		memset(gl.in_burst, 0, sizeof(gl.in_burst));
		esdr_seg_track_reset(&gl.track);
		key = k_spin_lock(&acc_lock);
		memset(&acc, 0, sizeof(acc));
		k_spin_unlock(&acc_lock, key);
		gl.run = true;
	}
	k_mutex_unlock(&gl_lock);
	if (ret == 0) {
		esdr_remote_gl_hook(gl_block);
	}
	return ret;
}

void esdr_gl_stop(void)
{
	esdr_remote_gl_hook(NULL);
	k_mutex_lock(&gl_lock, K_FOREVER);
	gl.run = false;
	k_mutex_unlock(&gl_lock);
}

unsigned int esdr_gl_channels(void)
{
	return gl.m;
}

uint32_t esdr_gl_power(float *power, float *floor, float *peak, float *open)
{
	const unsigned int m = gl.m;
	float sum[ESDR_PFB_M_MAX], big[ESDR_PFB_M_MAX], fl[ESDR_PFB_M_MAX];
	uint32_t op[ESDR_PFB_M_MAX], n;
	/* Copied and cleared under the lock, turned into dB outside it. */
	k_spinlock_key_t key = k_spin_lock(&acc_lock);

	n = acc.n;
	memcpy(sum, acc.sum, sizeof(sum));
	memcpy(big, acc.big, sizeof(big));
	memcpy(fl, acc.floor, sizeof(fl));
	memcpy(op, acc.open, sizeof(op));
	memset(acc.sum, 0, sizeof(acc.sum));
	memset(acc.big, 0, sizeof(acc.big));
	memset(acc.open, 0, sizeof(acc.open));
	acc.n = 0;
	k_spin_unlock(&acc_lock, key);
	for (unsigned int i = 0; i < m; i++) {
		unsigned int k = (i + m / 2U) % m;

		if (power != NULL) {
			power[i] = n != 0U ? db(sum[k] / (float)n) : -200.0f;
		}
		if (floor != NULL) {
			floor[i] = db(fl[k]);
		}
		if (peak != NULL) {
			peak[i] = n != 0U ? db(big[k]) : -200.0f;
		}
		if (open != NULL) {
			open[i] = n != 0U ? (float)op[k] / (float)n : 0.0f;
		}
	}
	return n;
}

unsigned int esdr_gl_bursts(struct esdr_gl_burst *out, unsigned int max)
{
	k_spinlock_key_t key = k_spin_lock(&acc_lock);
	unsigned int n = MIN(max, acc.count);

	for (unsigned int i = 0; i < n; i++) {
		out[i] = acc.burst[(acc.first + i) % ESDR_GL_BURSTS];
	}
	acc.first = (acc.first + n) % ESDR_GL_BURSTS;
	acc.count -= n;
	k_spin_unlock(&acc_lock, key);
	return n;
}

void esdr_gl_stats(struct esdr_gl_stats *st)
{
	k_spinlock_key_t key = k_spin_lock(&acc_lock);

	*st = acc.st;
	k_spin_unlock(&acc_lock, key);
}
