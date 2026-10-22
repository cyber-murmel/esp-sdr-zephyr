/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gapless stream's channelizer (esdr_gl.h). The parser hands each segment block to
 * gl_block(), which copies it into a pool (or drops it when the thread is behind); the thread
 * pushes its segments through esdr_pfbq and esdr_detq, adds every decided block's channel
 * powers to the running sums and turns the detector's openings and closings into bursts.
 * MicroPython takes the sums and the bursts (esdr_radio.c), and rows of the bursts' spectrum
 * (esdr_gl_spectrum()): loud blocks of a channel in a burst (up to GL_SPEC_PER_BLOCK channels a
 * block, taking turns) are transformed over their frames (a Hann window, GL_BLOCK points), the
 * central GL_SPEC_BINS bins (the channel's passband) summed over the burst; when the burst
 * ends, and when the row is taken, its mean goes into the row's peak hold at the channel's
 * place in the band. The rest of the row reads each channel's floor with a little noise: the
 * gaps in time and frequency look quiet, the bursts stand out.
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

/* The radio empties its staging ring at the link's rate (up to 16 blocks at once). */
#define GL_POOL  24U
/* Segment, bank, detector, burst spectrum: some 1.4 KiB at most (gapless_stats()). */
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
/* Spectrum bins a channel: the passband, half of a block's transform (2x oversampled). */
#define GL_SPEC_BINS (GL_BLOCK / 2U)
/* Transforms a block at most (some 400 cycles each); more loud channels take turns. */
#define GL_SPEC_PER_BLOCK 8U
/* Without a row taken for this long, the blocks are not transformed. */
#define GL_SPEC_IDLE_MS 1000U
/* A Hann bin over a channel's floor (noise over half the transform's bins): 1.5 / 8, dB. */
#define GL_BIN_DB (-7.27f)
/* The fill's noise: up to this many dB either side of the floor. */
#define GL_FILL_DB 1.5f
#define GL_TAU     6.28318531f

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

/*
 * The spectrum, under gl_lock: the transform's window and twiddles (W16^e, e = 0 .. 9); the
 * block whose loud channels take turns (its first frame, loud ones so far, the turn's stride
 * and phase); per channel the current burst's bin sums (output units squared, before the
 * window's normalization) and its blocks; the row's peak hold (GL_SPEC_BINS per channel,
 * channel order, the lowest bin first) and the channels that have one. In PSRAM, as the bank.
 */
static struct {
	float win[GL_BLOCK], cos[10], sin[10], norm;
	uint64_t blk;
	uint32_t idx, stride, phase;
	float sum[ESDR_PFB_M_MAX][GL_SPEC_BINS];
	uint32_t n[ESDR_PFB_M_MAX];
	float row[ESDR_PFB_M_MAX][GL_SPEC_BINS];
	bool has[ESDR_PFB_M_MAX];
	/* A reader took a row lately (k_uptime_get_32() of the last one); the fill's noise. */
	bool live, want;
	uint32_t seen, seed;
} spec PSRAM;

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

BUILD_ASSERT(GL_BLOCK == 16U && GL_SPEC_BINS == ESDR_GL_SPEC_BINS, "spec_dft() and the bins");

static void spec_init(void)
{
	float sw = 0.0f;

	for (unsigned int t = 0; t < GL_BLOCK; t++) {
		spec.win[t] = 0.5f - 0.5f * cosf(GL_TAU * ((float)t + 0.5f) / GL_BLOCK);
		sw += spec.win[t];
	}
	for (unsigned int e = 0; e < ARRAY_SIZE(spec.cos); e++) {
		spec.cos[e] = cosf(GL_TAU * (float)e / GL_BLOCK);
		spec.sin[e] = sinf(GL_TAU * (float)e / GL_BLOCK);
	}
	/* A carrier of amplitude a at a bin's centre reads a^2. */
	spec.norm = 1.0f / (sw * sw);
	memset(spec.sum, 0, sizeof(spec.sum));
	memset(spec.n, 0, sizeof(spec.n));
	memset(spec.row, 0, sizeof(spec.row));
	memset(spec.has, 0, sizeof(spec.has));
	spec.blk = UINT64_MAX;
	spec.live = false;
	spec.want = false;
	spec.seed = 0x2545f491U;
}

/*
 * The passband of one block of channel @p k (GL_BLOCK frames, I then Q): windowed, the bins
 * -4 .. 3 of its 16-point transform, X[b] = sum x[t] e^(-j tau b t / 16), their powers added
 * to the burst's sums. Row-column, 4 x 4 (t = 4 t1 + t2, b = b1 + 4 b2): four 4-point
 * transforms over t1, the twiddles W16^(t2 b1), then of the second pass only b2 = 0 (bins
 * 0 .. 3) and b2 = 3 (bins 12 .. 15, that is -4 .. -1).
 */
static void spec_dft(unsigned int k, const int16_t *iq)
{
	float xr[GL_BLOCK], xi[GL_BLOCK], ar[4][4], ai[4][4];
	float *const sum = spec.sum[k];

	for (unsigned int t = 0; t < GL_BLOCK; t++) {
		xr[t] = (float)iq[2U * t] * spec.win[t];
		xi[t] = (float)iq[2U * t + 1U] * spec.win[t];
	}
	for (unsigned int t2 = 0; t2 < 4U; t2++) {
		const float s02r = xr[t2] + xr[t2 + 8U], s02i = xi[t2] + xi[t2 + 8U];
		const float d02r = xr[t2] - xr[t2 + 8U], d02i = xi[t2] - xi[t2 + 8U];
		const float s13r = xr[t2 + 4U] + xr[t2 + 12U], s13i = xi[t2 + 4U] + xi[t2 + 12U];
		const float d13r = xr[t2 + 4U] - xr[t2 + 12U], d13i = xi[t2 + 4U] - xi[t2 + 12U];

		ar[t2][0] = s02r + s13r;
		ai[t2][0] = s02i + s13i;
		/* d02 - j d13, d02 + j d13 */
		ar[t2][1] = d02r + d13i;
		ai[t2][1] = d02i - d13r;
		ar[t2][2] = s02r - s13r;
		ai[t2][2] = s02i - s13i;
		ar[t2][3] = d02r - d13i;
		ai[t2][3] = d02i + d13r;
	}
	for (unsigned int t2 = 1; t2 < 4U; t2++) {
		for (unsigned int b1 = 1; b1 < 4U; b1++) {
			const float c = spec.cos[t2 * b1], sn = spec.sin[t2 * b1];
			const float r = ar[t2][b1], i = ai[t2][b1];

			/* (r + j i)(c - j sn) */
			ar[t2][b1] = r * c + i * sn;
			ai[t2][b1] = i * c - r * sn;
		}
	}
	for (unsigned int b1 = 0; b1 < 4U; b1++) {
		const float s02r = ar[0][b1] + ar[2][b1], s02i = ai[0][b1] + ai[2][b1];
		const float d02r = ar[0][b1] - ar[2][b1], d02i = ai[0][b1] - ai[2][b1];
		const float s13r = ar[1][b1] + ar[3][b1], s13i = ai[1][b1] + ai[3][b1];
		const float d13r = ar[1][b1] - ar[3][b1], d13i = ai[1][b1] - ai[3][b1];
		/* X[b1] = s02 + s13; X[b1 + 12] = d02 + j d13 */
		const float pr = s02r + s13r, pi = s02i + s13i;
		const float nr = d02r - d13i, ni = d02i + d13r;

		sum[b1] += nr * nr + ni * ni;
		sum[b1 + 4U] += pr * pr + pi * pi;
	}
	spec.n[k]++;
}

/*
 * A loud block of channel @p k in a burst, from the detector's callback: transformed if it is
 * the channel's turn. The first one of a block counts the block's loud channels; one in
 * stride of them in callback order, from a phase that moves every block, is transformed.
 */
static void spec_block(const struct esdr_detq *d, unsigned int k)
{
	int16_t iq[2U * GL_BLOCK] __aligned(4);

	if (d->cur_start != spec.blk) {
		const float *pw = esdr_detq_power(d), *fl = esdr_detq_floor(d);
		unsigned int loud = 0;

		for (unsigned int c = 0; c < gl.m; c++) {
			loud += pw[c] > fl[c] * d->ratio;
		}
		spec.blk = d->cur_start;
		spec.idx = 0;
		spec.stride = MAX(1U, DIV_ROUND_UP(loud, GL_SPEC_PER_BLOCK));
		spec.phase++;
	}
	if ((spec.idx++ + spec.phase) % spec.stride == 0U && esdr_detq_block(d, k, iq)) {
		spec_dft(k, iq);
	}
}

/* Channel @p k's burst so far, its mean, into the row's peak hold. */
static void spec_fold(unsigned int k)
{
	float s;

	if (spec.n[k] == 0U) {
		return;
	}
	s = spec.norm / (float)spec.n[k];
	for (unsigned int i = 0; i < GL_SPEC_BINS; i++) {
		spec.row[k][i] = MAX(spec.row[k][i], spec.sum[k][i] * s);
		spec.sum[k][i] = 0.0f;
	}
	spec.n[k] = 0;
	spec.has[k] = true;
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
	if (loud && spec.live) {
		spec_block(d, k);
	}
	if ((ev->flags & ESDR_DET_CLOSE) != 0U && gl.in_burst[k]) {
		push_burst(k, gl.start[k], gl.end[k], gl.peak[k]);
		spec_fold(k);
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
				spec_fold(k);
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
			uint64_t t0 = esdr_pfbq_cycles();

			spec.live = spec.want && k_uptime_get_32() - spec.seen < GL_SPEC_IDLE_MS;

			(void)esdr_seg_parse(pool[i], ESDR_BLOCK_BYTES, seg_cb, NULL);
			acc.st.blocks++;
			acc.st.cycles += esdr_pfbq_cycles() - t0;
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
		spec_init();
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
	size_t unused = 0;

	*st = acc.st;
	k_spin_unlock(&acc_lock, key);
	if (started && k_thread_stack_space_get(&gl_thread, &unused) == 0) {
		st->stack_free = (uint32_t)unused;
	}
}

/* Uniform in [-1, 1) (xorshift32). */
static float jitter(void)
{
	uint32_t x = spec.seed;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	spec.seed = x;
	return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

int esdr_gl_spectrum(float *out, unsigned int max)
{
	float fl[ESDR_PFB_M_MAX], sorted[ESDR_PFB_M_MAX], fallback = -200.0f;
	unsigned int m, n, nf = 0;

	k_mutex_lock(&gl_lock, K_FOREVER);
	m = gl.m;
	n = m * GL_SPEC_BINS;
	if (!gl.run || max < n) {
		k_mutex_unlock(&gl_lock);
		return !gl.run ? -ENODEV : -EINVAL;
	}
	spec.want = true;
	spec.seen = k_uptime_get_32();
	memcpy(fl, esdr_detq_floor(&gl.det), m * sizeof(float));
	for (unsigned int k = 0; k < m; k++) {
		spec_fold(k);
	}
	/*
	 * Bin j sits at (j - n / 2) fs / n: channel i in display order (channel (i + M / 2) mod M,
	 * at (i - M / 2) fs / M) holds bins 8 i - 4 .. 8 i + 3, the lowest channel's lower half at
	 * the top (fs / 2 and -fs / 2 are one frequency).
	 */
	for (unsigned int j = 0; j < n; j++) {
		const unsigned int i = ((j + GL_SPEC_BINS / 2U) / GL_SPEC_BINS) % m;
		const unsigned int k = (i + m / 2U) % m;

		out[j] = spec.has[k] ? spec.row[k][(j + GL_SPEC_BINS / 2U) % GL_SPEC_BINS] : 0.0f;
	}
	memset(spec.row, 0, sizeof(spec.row));
	memset(spec.has, 0, sizeof(spec.has));
	k_mutex_unlock(&gl_lock);

	/* Channels still learning take the median floor of the others. */
	for (unsigned int k = 0; k < m; k++) {
		if (fl[k] > 0.0f) {
			unsigned int at = nf++;

			while (at > 0U && sorted[at - 1U] > fl[k]) {
				sorted[at] = sorted[at - 1U];
				at--;
			}
			sorted[at] = fl[k];
		}
	}
	if (nf > 0U) {
		fallback = db(sorted[nf / 2U]) + GL_BIN_DB;
	}
	for (unsigned int k = 0; k < m; k++) {
		fl[k] = fl[k] > 0.0f ? db(fl[k]) + GL_BIN_DB : fallback;
	}
	for (unsigned int j = 0; j < n; j++) {
		const unsigned int k = (((j + GL_SPEC_BINS / 2U) / GL_SPEC_BINS) % m + m / 2U) % m;
		const float fill = fl[k] + GL_FILL_DB * 0.5f * (jitter() + jitter());

		out[j] = out[j] > 0.0f ? MAX(db(out[j]), fill) : fill;
	}
	return (int)n;
}
