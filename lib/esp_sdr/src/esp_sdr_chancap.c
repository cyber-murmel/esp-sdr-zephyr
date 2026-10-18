/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Channelized burst capture (esp_sdr_chancap.h).
 *
 * CPU 1 runs the receive ring without decimation; its sink copies every chunk into a hand-over
 * ring in SRAM bank 2 (planar int16, the first HO_MIRROR pairs mirrored past the end so that a
 * filter window never wraps), sums the power of every 64-pair sub-block, and publishes the write
 * position. The channelizer thread on the other CPU runs two passes: the wideband gate on the
 * sub-block powers, a lookback ahead of the second pass, which channelizes a sub-block when the
 * gate is or becomes open within that lookback. Channel samples go to a lookback ring; a detector
 * per channel opens a capture from it and appends until the hang time ends or max_capture is
 * reached. Captures live in a PSRAM slab and reach the consumer on the callback thread.
 *
 * The fast path (8 channels at decimation 8, CONFIG_ESP_SDR_CHANCAP_SIMD) splits the filter bank
 * over both CPUs: the sink also computes every frame's branch sums into a sums ring
 * (esp_sdr_chancap_sums_n()), and the channelizer turns 8 frames at a time into channel samples
 * (esp_sdr_chancap_post_n()), frame-major in a frame ring that is the lookback of all channels
 * at once. Other grids run in C on the channelizer's CPU from the raw hand-over ring.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_attr.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_chancap.h>

#include "esp_sdr_chancap_dsp.h"

LOG_MODULE_DECLARE(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

#define HOT IRAM_ATTR __attribute__((optimize("O3")))

#define FS_HZ        ESP_SDR_RING_RATE_HZ
#define CHANNELS_MAX ESP_SDR_CHANCAP_CHANNELS_MAX

/* Hand-over span in pairs (the raw ring of the C paths, the sums ring of the fast path). */
#define HO_PAIRS    8192U
#define HO_MASK     (HO_PAIRS - 1U)
#define HO_MIRROR   320U
/* Fast path: the sums ring holds HO_PAIRS of frames. */
#define SUMS_FRAMES (HO_PAIRS / 8U)
#define SUMS_MASK   (SUMS_FRAMES - 1U)
/* A fast frame's window starts this many pairs before the frame's first new pair. */
#define FAST_HIST   56U
BUILD_ASSERT(FAST_HIST <= ESP_SDR_RING_RAW_HISTORY, "chancap: the ring keeps the window's history");
#define SUB_PAIRS   64U
#define SUB_SHIFT   6U
#define HO_SUBS     (HO_PAIRS / SUB_PAIRS)
#define BLOCK_PAIRS 512U
/* Lookback: per-channel rings of the C paths (pairs in all), or frames of the fast path. */
#define LB_TOTAL    4096U
#define LB_FRAMES   (LB_TOTAL / 8U)
/*
 * Decimator bank: channels at most (a 16 MHz span holds three 5 MHz channels; the tables cost
 * about 1.5 KiB of internal RAM per channel), and the mixed history (a filter window and a
 * sub-block).
 */
#define DDC_MAX     4U
#define DDC_BUF     (CHANCAP_DDC_L + SUB_PAIRS)
/*
 * Detector block per channel: output samples (16 us at 2 MS/s, inside any lookback); its runs
 * of 8 add up their power halved, so 4 of them stay in 32 bits.
 */
#define DET_SAMPLES 32U
#define DET_RUNS    (DET_SAMPLES / 8U)
#define DET_SHIFT   1U
/*
 * Fast path: the block's power in one esp_sdr_chancap_power() call over its 32 frames,
 * fine << 5 or coarse << 15 (the sum >> 5, as the C paths' runs add up).
 */
#define DET_SF      10U
#define DET_SC      10U
/*
 * The gate pass: a sub-block's 8 frames of sums over all lanes, >> 7 (about the eighth of the
 * lanes' chancap_run8_pack() units it was).
 */
#define GATE_SHIFT  7U
/*
 * How far the channelizer pass may fall behind the newest pair: the rest of the hand-over ring
 * is the sink's slack while a block runs (it writes up to a block ahead meanwhile), and the C
 * filters' history in front of the block.
 */
#define LAG_MAX     (HO_PAIRS - 3U * BLOCK_PAIRS)
/*
 * The pass follows the gate by the lookback: with a block and a sleep of the thread (up to two
 * ticks, 3200 pairs at 10000 ticks a second) it stays inside LAG_MAX. 160 us, more than either
 * path's lookback of samples holds.
 */
#define LB_SUBS_MAX 40U
BUILD_ASSERT(LB_SUBS_MAX * SUB_PAIRS + BLOCK_PAIRS + 3200U <= LAG_MAX,
	     "chancap: lookback, block and sleep exceed the hand-over ring");
/* Ring units: short, so that a stop or a gap shows quickly and two banks suffice. */
#define UNIT_PAIRS  2048U

/* A frame-major run element: 8 lanes' I, then their Q. */
typedef int16_t frame_t[16];

/* The vector kernels' tables (esp_sdr_chancap_pie.S): 8 lanes each, 16-byte aligned. */
struct kern {
	/* Taps in window order, then a padding vector the kernel's look-ahead load reads. */
	int16_t hk[(CHANCAP_K_MAX + 1U) * 8U];
	/* esp_sdr_chancap_post_n()'s table: c1..c4, m_even, m_lo, m_hi. */
	int16_t post[7][8];
	/* A run's lane powers. */
	int16_t fine[8], coarse[8];
	/* A run transposed for the captures (lane-major, I and Q interleaved), and its scratch. */
	int16_t lanes[8][16], si[64], sq[64];
} __aligned(16);

struct capbuf;

/* A channel's state: internal RAM (bank 2), touched every run. */
struct chan {
	/* Lookback ring position of the C paths (samples written) and the capture in progress. */
	uint32_t lb_w;
	struct chancap_det det;
	struct capbuf *cap;
	uint32_t cap_n, cap_max;
	/* Output samples produced on this channel; the detector block's power so far, its runs. */
	uint64_t samples;
	uint32_t pw_run, pw_runs;
};

struct bank2 {
	union {
		struct {
			int16_t hi[HO_PAIRS + HO_MIRROR];
			int16_t hq[HO_PAIRS + HO_MIRROR];
		} raw;
		struct {
			frame_t sums[SUMS_FRAMES];
		} fast;
	} ho;
	union {
		/* Interleaved I/Q per channel, lb_samples each. */
		int16_t lb[2U * LB_TOTAL];
		frame_t frames[LB_FRAMES];
	} lb;
	/* Mean power of each sub-block (chancap_power() units), by sub-block index mod HO_SUBS. */
	uint32_t sub_power[HO_SUBS];
	/* Decimator bank: per channel the mixed pairs [pair - L, pair + SUB_PAIRS). */
	int16_t dmi[DDC_MAX][DDC_BUF], dmq[DDC_MAX][DDC_BUF];
	struct kern kern;
	struct chan ch[CHANNELS_MAX];
};
BUILD_ASSERT(sizeof(struct bank2) <= 65536U, "chancap: bank 2 overflow");
BUILD_ASSERT((HO_PAIRS & HO_MASK) == 0U && HO_MIRROR >= CHANCAP_L_MAX + 8U, "chancap: geometry");
BUILD_ASSERT(FAST_HIST + 8U == 8U * 8U, "chancap: the fast path is 8 channels, 8 taps a branch");

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
struct chancap_sums_job {
	const int16_t *xi, *xq, *hk;
	uint32_t k, n;
	frame_t *out;
	uint32_t shift, stride;
};

struct chancap_post_job {
	const frame_t *in;
	frame_t *out;
	uint32_t n;
	const int16_t *tab;
};

struct chancap_power_job {
	const frame_t *frames;
	int16_t *fine, *coarse;
	uint32_t n, sf, sc;
};

struct chancap_transpose_job {
	const frame_t *frames;
	int16_t (*out)[16];
	int16_t *si, *sq;
};

struct chancap_mix_job {
	const int16_t *xi, *xq, *tab;
	uint32_t n;
	int16_t *mi, *mq;
};

struct chancap_fir_job {
	const int16_t *mi, *mq, *h, *hs;
	int16_t *out;
	uint32_t n, shift, d;
};

/* A decimator channel's tables for the kernels: the mixer over 128 pairs, the taps twice. */
struct ddc_kern {
	int16_t mix[CHANCAP_DDC_PMAX / 8U][24];
	int16_t h[CHANCAP_DDC_L];
	int16_t hs[CHANCAP_DDC_L + 8U];
} __aligned(16);
BUILD_ASSERT(CHANCAP_TW_SHIFT == 14, "chancap: the mix kernel shifts by 14");

void esp_sdr_chancap_sums_n(const struct chancap_sums_job *job);
void esp_sdr_chancap_post_n(const struct chancap_post_job *job);
void esp_sdr_chancap_power(const struct chancap_power_job *job);
uint32_t esp_sdr_chancap_energy(const frame_t *frames, uint32_t n, uint32_t shift);
void esp_sdr_chancap_copy(int16_t *dst, const int16_t *src, uint32_t n);
uint32_t esp_sdr_chancap_energy2(const int16_t *i, const int16_t *q, uint32_t n, uint32_t shift);
void esp_sdr_chancap_transpose8(const struct chancap_transpose_job *job);
void esp_sdr_chancap_mix(const struct chancap_mix_job *job);
void esp_sdr_chancap_ddc_fir(const struct chancap_fir_job *job);
/* The hand-over copy: n int16, 16-byte aligned ends (memcpy() would call the ROM's). */
#define HO_COPY(d, s, n) esp_sdr_chancap_copy((d), (s), (n))
#else
#define HO_COPY(d, s, n) memcpy((d), (s), 2U * (n))
#endif

/* One capture buffer in the PSRAM slab. */
struct capbuf {
	struct esp_sdr_chancap cap;
	/* The burst's peak and the channel's floor as powers: dB on the callback thread. */
	uint32_t peak_pw, noise_pw;
	uint16_t slot;
	bool used;
	int16_t iq[];
};

static EXT_RAM_BSS_ATTR uint8_t slab[CONFIG_ESP_SDR_CHANCAP_SLAB_KB * 1024U] __aligned(16);

static inline uint32_t ccount(void)
{
	uint32_t c;

	__asm__ volatile("rsr.ccount %0" : "=r"(c));
	return c;
}

/* The vector unit is coprocessor 3; Zephyr neither enables nor saves it. */
static inline void pie_enable(void)
{
	uint32_t cp;

	__asm__ volatile("rsr.cpenable %0" : "=r"(cp));
	cp |= BIT(3);
	__asm__ volatile("wsr.cpenable %0\n\trsync" : : "r"(cp));
}

/* ---- sink side (CPU 1, ring loop, interrupts masked) ---- */

static struct {
	struct bank2 *b2;
	/* The raw ring: its pairs (a power of two) and planes. */
	uint32_t ho_pairs, ho_mask;
	int16_t *hi, *hq;
	/* Fast path: the branch sums per frame, from these taps. */
	bool fast;
	frame_t *sums;
	const int16_t *hk;
	uint32_t k;
	/* Pairs written, published with a sequence (odd while changing). */
	atomic_t seq;
	uint64_t written;
	/* Hand-over epochs: a ring gap starts a new one. */
	atomic_t epoch;
	uint64_t expect;
	/* Power of the sub-block in progress: sum of every 4th pair's I^2 + Q^2, and pairs seen. */
	uint32_t acc, acc_n;
	uint32_t gaps;
} snk;

/*
 * No wake-up from the sink: a k_sem_give() from the ring loop cost the ring thousands of
 * abandoned units a second (the scheduler lock against CPU 0, an IPI per give), and both
 * software interrupts the ring's CPU could raise instead belong to the flash stall. The
 * channelizer polls at the tick.
 */
static inline void ho_publish(uint64_t written)
{
	atomic_val_t q = atomic_get(&snk.seq);

	atomic_set(&snk.seq, q + 1);
	snk.written = written;
	atomic_set(&snk.seq, q + 2);
}

static bool ho_written(uint64_t *written)
{
	for (int tries = 0; tries < 8; tries++) {
		atomic_val_t q = atomic_get(&snk.seq);
		uint64_t w = snk.written;

		if ((q & 1) == 0 && atomic_get(&snk.seq) == q) {
			*written = w;
			return true;
		}
	}
	return false;
}

/* n pairs into a ring of @p size pairs at pos, wrapping; [0, HO_MIRROR) mirrored past the end. */
HOT static void ho_copy(const int16_t *src, int16_t *dst, uint32_t pos, uint32_t n, uint32_t size)
{
	/* All multiples of 8 pairs: the chunks, their positions and the mirror. */
	uint32_t first = MIN(n, size - pos);

	HO_COPY(dst + pos, src, first);
	if (first < n) {
		HO_COPY(dst, src + first, n - first);
	}
	if (pos < HO_MIRROR) {
		uint32_t m = MIN(n, HO_MIRROR - pos);

		HO_COPY(dst + size + pos, src, m);
	} else if (first < n) {
		HO_COPY(dst + size, src + first, MIN(n - first, HO_MIRROR));
	}
}

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
/*
 * The branch sums of the frames whose newest pair lies in [index, index + n), in place: frame
 * j's window starts FAST_HIST pairs before its first new pair, in the ring's history in front
 * of i and q (ESP_SDR_RING_RAW_HISTORY).
 */
HOT static void sink_sums(const int16_t *i, const int16_t *q, uint64_t index, uint32_t n)
{
	uint32_t f = (uint32_t)(index >> 3), left = n / 8U;
	const int16_t *xi = i - FAST_HIST, *xq = q - FAST_HIST;

	while (left != 0U) {
		uint32_t fs = f & SUMS_MASK;
		/* Up to the sums ring's end. */
		uint32_t nf = MIN(left, SUMS_FRAMES - fs);
		struct chancap_sums_job job = {
			.xi = xi, .xq = xq, .hk = snk.hk, .k = snk.k, .n = nf, .out = &snk.sums[fs],
			.shift = CHANCAP_H_SHIFT, .stride = 16U,
		};

		esp_sdr_chancap_sums_n(&job);
		xi += 8U * nf;
		xq += 8U * nf;
		f += nf;
		left -= nf;
	}
}
#endif

HOT static void sink(void *user, const int16_t *i, const int16_t *q, size_t n, uint64_t index)
{
	struct bank2 *b2 = snk.b2;
	uint32_t pos = (uint32_t)index & snk.ho_mask;

	ARG_UNUSED(user);
	if (index != snk.expect) {
		/*
		 * A ring gap: the consumer restarts from here. The sub-block in progress keeps the
		 * grid of 64 pairs (its first power is low, which never opens the gate).
		 */
		snk.gaps++;
		snk.acc = 0;
		snk.acc_n = (uint32_t)index & (SUB_PAIRS - 1U);
		atomic_inc(&snk.epoch);
	}
#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
	if (snk.fast) {
		/* No copy and no power here: the channelizer takes the gate's power from the sums. */
		sink_sums(i, q, index, (uint32_t)n);
		snk.expect = index + n;
		ho_publish(snk.expect);
		return;
	}
#endif
	ho_copy(i, snk.hi, pos, (uint32_t)n, snk.ho_pairs);
	ho_copy(q, snk.hq, pos, (uint32_t)n, snk.ho_pairs);
#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
	/* Sub-block power of every pair: the pieces' sums >> 6 add up to the mean. */
	for (uint32_t k = 0; k < n;) {
		uint32_t m = MIN((uint32_t)n - k, SUB_PAIRS - snk.acc_n);

		snk.acc += esp_sdr_chancap_energy2(i + k, q + k, m, SUB_SHIFT);
		snk.acc_n += m;
		k += m;
		if (snk.acc_n == SUB_PAIRS) {
			uint32_t sub = (uint32_t)((index + k - 1U) >> SUB_SHIFT) & (HO_SUBS - 1U);

			b2->sub_power[sub] = snk.acc;
			snk.acc = 0;
			snk.acc_n = 0;
		}
	}
#else
	/* Sub-block power on every 4th pair: 16 per sub-block, about a cycle per pair. */
	for (uint32_t k = 0; k < n; k += 4U) {
		int32_t a = i[k], b = q[k];

		snk.acc += (uint32_t)(a * a) + (uint32_t)(b * b);
		snk.acc_n += 4U;
		if (snk.acc_n == SUB_PAIRS) {
			uint32_t sub = (uint32_t)((index + k) >> SUB_SHIFT) & (HO_SUBS - 1U);

			b2->sub_power[sub] = snk.acc / (SUB_PAIRS / 4U);
			snk.acc = 0;
			snk.acc_n = 0;
		}
	}
#endif
	snk.expect = index + n;
	ho_publish(snk.expect);
}

/* ---- run state (CPU 0 threads) ---- */

enum engine { ENGINE_PFB, ENGINE_DDC };

static struct {
	struct esp_sdr_chancap_cfg cfg;
	struct esp_sdr_chancap_plan plan;
	enum engine engine;
	/* Both CPUs and the vector kernels (8 channels, decimation 8). */
	bool fast;
	/* The decimator bank on the vector kernels (decimation 4 or 8). */
	bool ddc_fast;
	unsigned int n;
	/* Output samples per input pair: 1 / d. */
	unsigned int d;
	uint32_t rate_hz;
	/* Filter bank channel c is bin b0 + c. */
	unsigned int b0;
	struct chancap_pfb pfb;
	struct chancap_ddc ddc[DDC_MAX];
	/* C paths: a lookback ring per channel, samples (a power of two); the lookback in samples. */
	uint32_t lb_samples, lb_mask, lookback;
	/* Fast path: frames written to the frame ring since the run started. */
	uint32_t lbf_w;
	/*
	 * Fast path: the channels advance together, so one count of output samples per channel
	 * and of runs in the detector's block serves them all.
	 */
	uint64_t fast_samples;
	uint32_t fast_runs;
	/* Captures open now. */
	uint32_t caps_open;
	uint32_t hang_det_blocks, max_samples;
	/* Wideband gate: sub-blocks of lookback, hang, and the detector. */
	uint32_t lb_subs, wb_hang_subs;
	struct chancap_det wb;
	/* Gate pass: next sub-block to judge; the newest open one (none: ~0). */
	uint64_t gate_sub, open_sub;
	/* Channelizer pass: next pair; whether its filters follow the stream. */
	uint64_t run_pair;
	bool running;
	/* Decimator bank: the sub-block its mixed history leads into (~0: none). */
	uint64_t ddc_next;
	/* Epoch the passes follow. */
	atomic_val_t epoch;
	/* Capture buffers. */
	struct capbuf **bufs;
	uint8_t *buf_area;
	size_t buf_size;
	uint16_t nbufs;
	uint16_t *free_list;
	uint16_t free_n;
	struct esp_sdr_chancap_stats st;
	uint64_t cyc, cyc_pairs;
} rs ESP_SDR_HIGH_RAM; /* Internal RAM above the capture bank: the PSRAM copies of the captures
			* stream through the data cache and evicted it from there. Not zeroed
			* at boot (rs_init() does); no atomics in it. */

/* The region is not zeroed at boot: the readers of rs (stats, plan) may run before a start. */
static int rs_init(void)
{
	memset(&rs, 0, sizeof(rs));
	return 0;
}
SYS_INIT(rs_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/* The channels' state in bank 2 (setup()). */
static struct chan *chs;
#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
/* The decimator channels' kernel tables: internal RAM above the capture bank. */
static struct ddc_kern ddck[DDC_MAX] ESP_SDR_HIGH_RAM;
#endif
/* The bench's split of the fast run: kernels and per-channel work, in cycles. */
static bool prof_on;
static uint32_t prof_kern, prof_chan;
/* The capture buffers' free list lock (internal RAM: atomics). */
static struct k_spinlock buf_lock;
/* The ring's counters of the current or last run, written by the ring loop: internal RAM. */
static struct esp_sdr_ring_stats ring_st;

/* Gates and thresholds, read at block boundaries. */
static atomic_t gates;
static atomic_t thresholds;
static atomic_t stop_req;
static atomic_t active;
static struct k_mutex lock;
static bool lock_ready;

#define GATE_WB BIT(0)
#define GATE_CH BIT(1)

/* The callback queue and threads. */
K_MSGQ_DEFINE(out_q, sizeof(struct capbuf *), CONFIG_ESP_SDR_CHANCAP_QUEUE, 4);
ESP_SDR_HIGH_STACK_DEFINE(ring_stack, CONFIG_ESP_SDR_CHANCAP_RING_STACK_SIZE);
ESP_SDR_HIGH_STACK_DEFINE(dsp_stack, CONFIG_ESP_SDR_CHANCAP_DSP_STACK_SIZE);
ESP_SDR_HIGH_STACK_DEFINE(out_stack, CONFIG_ESP_SDR_CHANCAP_OUT_STACK_SIZE);
static struct k_thread ring_thread, dsp_thread, out_thread;

/* ---- capture buffers ---- */

static struct capbuf *buf_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&buf_lock);
	struct capbuf *b = NULL;

	if (rs.free_n != 0U) {
		b = rs.bufs[rs.free_list[--rs.free_n]];
		b->used = true;
		rs.st.bufs_used++;
		rs.st.bufs_peak = MAX(rs.st.bufs_peak, rs.st.bufs_used);
	}
	k_spin_unlock(&buf_lock, key);
	return b;
}

static void buf_put(struct capbuf *b)
{
	k_spinlock_key_t key = k_spin_lock(&buf_lock);

	if (b->used) {
		b->used = false;
		rs.free_list[rs.free_n++] = b->slot;
		rs.st.bufs_used--;
	}
	k_spin_unlock(&buf_lock, key);
}

void esp_sdr_chancap_release(const struct esp_sdr_chancap *cap)
{
	struct capbuf *b = CONTAINER_OF(cap, struct capbuf, cap);

	buf_put(b);
}

static void bufs_init(size_t samples)
{
	size_t one = ROUND_UP(sizeof(struct capbuf) + 4U * samples, 16U);
	size_t list;

	rs.nbufs = (uint16_t)MIN(sizeof(slab) / one, 1024U);
	rs.buf_size = one;
	/* The pointer table and the free list at the slab's start, buffers after them. */
	list = ROUND_UP(rs.nbufs * (sizeof(struct capbuf *) + sizeof(uint16_t)), 16U);
	while (rs.nbufs != 0U && list + (size_t)rs.nbufs * one > sizeof(slab)) {
		rs.nbufs--;
	}
	rs.bufs = (struct capbuf **)slab;
	rs.free_list = (uint16_t *)(slab + rs.nbufs * sizeof(struct capbuf *));
	rs.buf_area = slab + list;
	rs.free_n = 0;
	for (uint16_t k = 0; k < rs.nbufs; k++) {
		struct capbuf *b = (struct capbuf *)(rs.buf_area + (size_t)k * one);

		b->slot = k;
		b->used = false;
		rs.bufs[k] = b;
		rs.free_list[rs.free_n++] = k;
	}
}

/* ---- the plan: engine, LO, filters ---- */

static bool is_pow2(unsigned int v)
{
	return v != 0U && (v & (v - 1U)) == 0U;
}

static int plan(const struct esp_sdr_chancap_cfg *cfg)
{
	unsigned int n = cfg->n_channels;
	uint64_t span = (uint64_t)(n - 1U) * cfg->spacing_hz;
	int64_t lo;

	if (n == 0U || n > CHANNELS_MAX || cfg->spacing_hz == 0U || cfg->channel_rate_hz == 0U ||
	    cfg->cb == NULL || span + cfg->channel_rate_hz > FS_HZ) {
		return -EINVAL;
	}
	rs.n = n;
	rs.rate_hz = cfg->channel_rate_hz;
	if (FS_HZ % cfg->spacing_hz == 0U && is_pow2(FS_HZ / cfg->spacing_hz) &&
	    FS_HZ / cfg->spacing_hz <= CHANCAP_M_MAX &&
	    (cfg->channel_rate_hz == cfg->spacing_hz ||
	     cfg->channel_rate_hz == 2U * cfg->spacing_hz)) {
		unsigned int m = FS_HZ / cfg->spacing_hz, d = FS_HZ / cfg->channel_rate_hz;

		if (m < n || m < 2U) {
			return -EINVAL;
		}
		rs.engine = ENGINE_PFB;
		rs.d = d;
		rs.b0 = (m - n) / 2U;
		/* Channel c at bin b0 + c, bins at (b - m / 2 + 1 / 2) spacings above the LO. */
		lo = (int64_t)cfg->first_freq_hz -
		     ((int64_t)rs.b0 * 2 - (int64_t)m + 1) * (int64_t)cfg->spacing_hz / 2;
		if (!chancap_pfb_init(&rs.pfb, m, d, 8, true,
				      0.4f * (float)cfg->spacing_hz / (float)FS_HZ, 50.0f)) {
			return -EINVAL;
		}
		rs.plan = (struct esp_sdr_chancap_plan){.pfb = true, .m = m, .d = d,
						       .taps = rs.pfb.l,
						       .delay_pairs = (rs.pfb.l - 1U) / 2U};
		rs.fast = IS_ENABLED(CONFIG_ESP_SDR_CHANCAP_SIMD) && m == 8U && d == 8U;
	} else {
		unsigned int d = FS_HZ / cfg->channel_rate_hz;
		int64_t centre = (int64_t)cfg->first_freq_hz + (int64_t)span / 2;
		/*
		 * The LO 1.5 MHz off the span's centre keeps its leakage off a channel, if the
		 * outer channels' pass bands (the decimator's cutoff, 0.3 of the rate) still fit.
		 */
		int64_t shift = span / 2 + 1500000 + (int64_t)cfg->channel_rate_hz * 3 / 10 <=
						FS_HZ / 2U
					? 1500000
					: 0;

		if (FS_HZ % cfg->channel_rate_hz != 0U || d < 2U || n > DDC_MAX) {
			return -EINVAL;
		}
		rs.engine = ENGINE_DDC;
		rs.d = d;
		lo = centre - shift;
		for (unsigned int c = 0; c < n; c++) {
			int64_t off = (int64_t)cfg->first_freq_hz + (int64_t)c * cfg->spacing_hz - lo;

			/* The mixer period: off / fs as num / den with den at most 128 (125 kHz steps). */
			if (off % (int64_t)(FS_HZ / CHANCAP_DDC_PMAX) != 0) {
				return -EINVAL;
			}
			if (!chancap_ddc_init(&rs.ddc[c],
					      (int)(off / (int64_t)(FS_HZ / CHANCAP_DDC_PMAX)),
					      CHANCAP_DDC_PMAX, d,
					      0.3f * (float)cfg->channel_rate_hz / (float)FS_HZ,
					      40.0f)) {
				return -EINVAL;
			}
		}
		rs.plan = (struct esp_sdr_chancap_plan){.pfb = false, .m = n, .d = d,
						       .taps = CHANCAP_DDC_L,
						       .delay_pairs = (CHANCAP_DDC_L - 1U) / 2U};
		rs.ddc_fast = IS_ENABLED(CONFIG_ESP_SDR_CHANCAP_SIMD) && (d == 4U || d == 8U);
	}
	/* A sub-block must hold whole runs of 8 outputs: per-channel rates of 2 MS/s and up. */
	if (SUB_PAIRS % (8U * rs.d) != 0U) {
		return -EINVAL;
	}
	if (lo < (int64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000 || lo % 1000 != 0) {
		return -ERANGE;
	}
	rs.plan.lo_hz = (uint64_t)lo;
	return 0;
}

static int tune(void)
{
	uint64_t lo = rs.plan.lo_hz;
	uint32_t mhz = (uint32_t)((lo + 500000U) / 1000000U);
	int32_t khz = (int32_t)((int64_t)lo - (int64_t)mhz * 1000000) / 1000;
	int ret = esp_sdr_set_freq(mhz);

	if (ret == 0) {
		ret = esp_sdr_set_freq_offset(khz);
	}
	return ret;
}

/* The kernels' tables from the bank's design (the fast path). */
static void kern_init(struct kern *k, const struct chancap_pfb *p)
{
	memset(k, 0, sizeof(*k));
	for (unsigned int t = 0; t < p->k; t++) {
		/* Window vector t carries tap k - 1 - t. */
		memcpy(&k->hk[8U * t], p->hk[p->k - 1U - t], 16);
	}
	for (unsigned int i = 0; i < 8; i++) {
		int16_t r = p->t8r[i], im = p->t8i[i];
		bool lo = i < 4U;

		/*
		 * Stage 1 leaves [sI, eQ] and [sQ, eI]: lane i's I times c1 plus the other's times
		 * c2 is the twiddled I, with c3 and c4 the Q (esp_sdr_chancap_pie.S).
		 */
		k->post[0][i] = lo ? r : (int16_t)-im;
		k->post[1][i] = lo ? (int16_t)-im : r;
		k->post[2][i] = lo ? im : r;
		k->post[3][i] = lo ? r : im;
		k->post[4][i] = (i & 1U) == 0U ? 1 : 0;
		k->post[5][i] = (i == 1U || i == 3U) ? 1 : 0;
		k->post[6][i] = (i == 5U || i == 7U) ? 1 : 0;
	}
}

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
static void ddc_kern_init(struct ddc_kern *k, const struct chancap_ddc *c)
{
	unsigned int mask = c->period - 1U;

	memset(k, 0, sizeof(*k));
	/* Vector v covers pairs 8 v to 8 v + 7 of a 128-pair cycle: the period repeats in it. */
	for (unsigned int v = 0; v < CHANCAP_DDC_PMAX / 8U; v++) {
		for (unsigned int l = 0; l < 8; l++) {
			unsigned int ph = (8U * v + l) & mask;

			k->mix[v][l] = c->cr[ph];
			k->mix[v][8U + l] = c->ci[ph];
			k->mix[v][16U + l] = (int16_t)-c->ci[ph];
		}
	}
	memcpy(k->h, c->h, sizeof(k->h));
	memcpy(&k->hs[4], c->h, sizeof(c->h));
}
#endif

static inline uint32_t us_to_samples(uint32_t us, uint32_t rate_hz)
{
	return (uint32_t)(((uint64_t)us * rate_hz + 999999U) / 1000000U);
}

/*
 * Run geometry and the sink's view, shared by a start and the bench: the bank-2 layout of the
 * engine, the lookback, the detectors' blocks, the capture length.
 */
static void setup(const struct esp_sdr_chancap_cfg *cfg)
{
	size_t words;

	snk.b2 = (struct bank2 *)esp_sdr_ring_burst_buf(&words);
	chs = snk.b2->ch;
	memset(chs, 0, sizeof(snk.b2->ch));
	rs.caps_open = 0;
	if (rs.fast) {
		/* No raw ring: the sink reads the ring's own buffer. */
		snk.ho_pairs = 1U;
		snk.sums = snk.b2->ho.fast.sums;
		snk.hk = snk.b2->kern.hk;
		snk.k = rs.pfb.k;
		kern_init(&snk.b2->kern, &rs.pfb);
	} else {
		snk.ho_pairs = HO_PAIRS;
		snk.hi = snk.b2->ho.raw.hi;
		snk.hq = snk.b2->ho.raw.hq;
	}
	snk.ho_mask = snk.ho_pairs - 1U;
	snk.fast = rs.fast;
#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
	if (rs.ddc_fast) {
		for (unsigned int c = 0; c < rs.n; c++) {
			ddc_kern_init(&ddck[c], &rs.ddc[c]);
		}
	}
#endif
	rs.lb_samples = 1;
	while (rs.lb_samples * 2U * rs.n <= LB_TOTAL) {
		rs.lb_samples *= 2U;
	}
	rs.lb_mask = rs.lb_samples - 1U;
	/* The fast path's frame ring has the same depth per channel. */
	rs.lookback = MIN(us_to_samples(cfg->lookback_us, rs.rate_hz),
			  (rs.fast ? LB_FRAMES : rs.lb_samples) / 2U);
	rs.lb_subs = MIN(((uint64_t)cfg->lookback_us * FS_HZ / 1000000U + SUB_PAIRS - 1U) /
				 SUB_PAIRS + 1U,
			 LB_SUBS_MAX);
	rs.wb_hang_subs = (uint32_t)((uint64_t)cfg->hang_us * FS_HZ / 1000000U / SUB_PAIRS);
	rs.hang_det_blocks = us_to_samples(cfg->hang_us, rs.rate_hz) / DET_SAMPLES;
	/* Captures grow by runs of 8. */
	rs.max_samples = ROUND_UP(MAX(us_to_samples(cfg->max_capture_us, rs.rate_hz), DET_SAMPLES),
				  8U);
	bufs_init(rs.max_samples);
}

/* ---- channelizer pass ---- */

/* Pair index of output sample j (the filter window's centre). */
static inline uint64_t sample_index(uint64_t j)
{
	return j * rs.d + rs.d - 1U - rs.plan.delay_pairs;
}

static void capture_finish_(unsigned int c, bool truncated, bool chunk)
{
	struct chan *ch = &chs[c];
	struct capbuf *b = ch->cap;

	if (b == NULL) {
		return;
	}
	b->cap.n = ch->cap_n;
	b->cap.truncated = truncated;
	b->cap.chunk = chunk;
	/* The logarithms (about 3000 cycles each in software) wait for the callback thread. */
	b->peak_pw = ch->det.peak;
	b->noise_pw = ch->det.floor;
	ch->cap = NULL;
	rs.caps_open--;
	if (k_msgq_put(&out_q, &b, K_NO_WAIT) != 0) {
		buf_put(b);
		rs.st.dropped++;
		return;
	}
	rs.st.captures++;
	if (truncated) {
		rs.st.truncated++;
	}
}

/* Open a capture on channel c with the last @p back samples of its lookback. */
static void capture_open_(unsigned int c, uint32_t back)
{
	struct chan *ch = &chs[c];
	struct capbuf *b = buf_get();
	uint32_t have = rs.fast ? MIN(rs.lbf_w, LB_FRAMES - 8U) : ch->lb_w;

	if (b == NULL) {
		rs.st.dropped++;
		return;
	}
	/* The runs append 8 at a time: leave room for one. */
	back = MIN(back, MIN(have, rs.max_samples - 8U));
	b->cap.channel = (uint16_t)c;
	b->cap.index = sample_index((rs.fast ? rs.fast_samples : ch->samples) - back);
	b->cap.sample_rate = rs.rate_hz;
	b->cap.iq = b->iq;
	ch->cap = b;
	rs.caps_open++;
	ch->cap_max = rs.max_samples;
	/* One 32-bit store per sample into the PSRAM buffer (iq is 4-byte aligned). */
	if (rs.fast) {
		const frame_t *fr = snk.b2->lb.frames;
		unsigned int lane = rs.b0 + c;
		uint32_t *dst = (uint32_t *)b->iq;

		for (uint32_t k = 0; k < back; k++) {
			uint32_t f = (rs.lbf_w - back + k) & (LB_FRAMES - 1U);

			dst[k] = (uint16_t)fr[f][lane] | ((uint32_t)(uint16_t)fr[f][8U + lane] << 16);
		}
	} else {
		const uint32_t *src = (const uint32_t *)&snk.b2->lb.lb[2U * c * rs.lb_samples];
		uint32_t *dst = (uint32_t *)b->iq;

		for (uint32_t k = 0; k < back; k++) {
			dst[k] = src[(ch->lb_w - back + k) & rs.lb_mask];
		}
	}
	ch->cap_n = back;
}

/* The capture bookkeeping, timed for the stats (cycles, the longest of each). */
static void capture_finish(unsigned int c, bool truncated, bool chunk)
{
	uint32_t t = ccount();

	capture_finish_(c, truncated, chunk);
	rs.st.finish_max = MAX(rs.st.finish_max, ccount() - t);
}

static void capture_open(unsigned int c, uint32_t back)
{
	uint32_t t = ccount();

	capture_open_(c, back);
	rs.st.open_max = MAX(rs.st.open_max, ccount() - t);
}

/* Channel c's lookback ring slot for its next run of 8 samples (runs never wrap: 8 | size). */
static inline int16_t *lb_slot(unsigned int c)
{
	return &snk.b2->lb.lb[2U * (c * rs.lb_samples + (chs[c].lb_w & rs.lb_mask))];
}

/* After a run was appended: a capture at its length limit ends, or with the gate off, rolls over. */
static inline void channel_limit(unsigned int c, bool ch_gate)
{
	struct chan *ch = &chs[c];

	if (ch->cap != NULL && ch->cap_n >= ch->cap_max) {
		if (ch_gate) {
			capture_finish(c, true, false);
		} else {
			capture_finish(c, false, true);
			capture_open(c, 0);
		}
	}
}

/* A detector block of channel c is complete: its power to the detector, which opens or ends captures. */
static void channel_judge(unsigned int c, bool ch_gate)
{
	struct chan *ch = &chs[c];
	bool was = ch->det.open;
	bool open = chancap_det_update(&ch->det, ch->pw_run);

	ch->pw_run = 0;
	if (!ch_gate) {
		if (ch->cap == NULL) {
			capture_open(c, 0);
		}
		return;
	}
	if (open && !was && ch->cap == NULL) {
		capture_open(c, rs.lookback + DET_SAMPLES);
	} else if (!open && was) {
		capture_finish(c, false, false);
	}
}

/* After a run of 8 samples was appended: the length limit, then the detector every DET_RUNS runs. */
static inline void channel_detect(unsigned int c, uint32_t p8, bool ch_gate)
{
	struct chan *ch = &chs[c];

	channel_limit(c, ch_gate);
	/* A run's p8 is its sum of I^2 + Q^2 / 16: DET_RUNS of them halved give the mean power. */
	ch->pw_run += p8 >> DET_SHIFT;
	if (++ch->pw_runs < DET_RUNS) {
		return;
	}
	ch->pw_runs = 0;
	channel_judge(c, ch_gate);
}

/*
 * A run of 8 samples (32 bytes, both 4-byte aligned) into a capture: memcpy() would call the
 * ROM's.
 */
static inline void copy_run(int16_t *dst, const int16_t *src)
{
	uint32_t *d = (uint32_t *)dst;
	const uint32_t *s = (const uint32_t *)src;

	d[0] = s[0];
	d[1] = s[1];
	d[2] = s[2];
	d[3] = s[3];
	d[4] = s[4];
	d[5] = s[5];
	d[6] = s[6];
	d[7] = s[7];
}

/*
 * C paths: one run of 8 output samples of channel c, already in its lookback ring at @p iq8,
 * with their power (sum of I^2 + Q^2 over the 8, divided by 16).
 */
static inline void channel_run(unsigned int c, const int16_t *iq8, uint32_t p8, bool ch_gate)
{
	struct chan *ch = &chs[c];

	ch->lb_w += 8U;
	ch->samples += 8U;
	if (ch->cap != NULL) {
		copy_run(&ch->cap->iq[2U * ch->cap_n], iq8);
		ch->cap_n += 8U;
	}
	channel_detect(c, p8, ch_gate);
}

/* The filters restart at pair @p pair: histories come from the hand-over ring. */
static void run_restart(uint64_t pair)
{
	rs.run_pair = pair;
	rs.running = true;
	rs.ddc_next = ~0ULL;
	rs.lbf_w = 0;
	rs.fast_samples = pair / rs.d;
	rs.fast_runs = 0;
	if (rs.engine == ENGINE_PFB) {
		rs.pfb.frames = pair / rs.d;
	} else {
		for (unsigned int c = 0; c < rs.n; c++) {
			rs.ddc[c].outputs = pair / rs.d;
		}
	}
	for (unsigned int c = 0; c < rs.n; c++) {
		chs[c].samples = pair / rs.d;
		chs[c].pw_run = 0;
		chs[c].pw_runs = 0;
		/* An interrupted capture ends; the lookback ring is stale. */
		capture_finish(c, false, false);
		chs[c].lb_w = 0;
	}
}

/*
 * The channelizer stops: open captures end. cut: by an overrun, a resync or a stop, so a
 * capture whose detector still saw power is truncated; else the wideband gate closed after
 * its hang, the band quiet that long, and the captures end whole (their own hang has about
 * run out too).
 */
static void run_stop(bool cut)
{
	rs.running = false;
	for (unsigned int c = 0; c < rs.n; c++) {
		capture_finish(c, cut && chs[c].det.open,
			       !((unsigned int)atomic_get(&gates) & GATE_CH));
	}
}

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
/* Fast path: the sub-block at @p pair is one run of 8 frames, their sums from the sink. */
static void run_sub_fast(uint64_t pair, bool ch_gate)
{
	struct bank2 *b2 = snk.b2;
	struct kern *k = &b2->kern;
	frame_t *out = &b2->lb.frames[rs.lbf_w & (LB_FRAMES - 1U)];
	struct chancap_post_job pj = {
		.in = &b2->ho.fast.sums[(uint32_t)(pair >> 3) & SUMS_MASK],
		.out = out, .n = 8U, .tab = k->post[0],
	};
	const unsigned int n = rs.n, b0 = rs.b0;
	struct chan *ch = chs;
	uint32_t t = prof_on ? ccount() : 0U;

	/* The run starts on a multiple of 8 frames: an even frame, as the kernel expects. */
	esp_sdr_chancap_post_n(&pj);
	rs.lbf_w += 8U;
	rs.fast_samples += 8U;
	if (rs.caps_open != 0U) {
		/* Open captures take their lanes from one transpose of the run. */
		struct chancap_transpose_job tj = {.frames = out, .out = k->lanes, .si = k->si,
						   .sq = k->sq};

		esp_sdr_chancap_transpose8(&tj);
		for (unsigned int c = 0; c < n; c++) {
			if (ch[c].cap != NULL) {
				copy_run(&ch[c].cap->iq[2U * ch[c].cap_n], k->lanes[b0 + c]);
				ch[c].cap_n += 8U;
				channel_limit(c, ch_gate);
			}
		}
	}
	if (prof_on) {
		uint32_t t1 = ccount();

		prof_kern += t1 - t;
		t = t1;
	}
	if (++rs.fast_runs == DET_RUNS) {
		/*
		 * The detector block: the last DET_RUNS runs, contiguous in the frame ring (the
		 * blocks start on multiples of DET_SAMPLES frames since the restart).
		 */
		struct chancap_power_job wj = {
			.frames = &b2->lb.frames[(rs.lbf_w - DET_SAMPLES) & (LB_FRAMES - 1U)],
			.fine = k->fine, .coarse = k->coarse, .n = DET_SAMPLES, .sf = DET_SF,
			.sc = DET_SC,
		};

		rs.fast_runs = 0;
		esp_sdr_chancap_power(&wj);
		for (unsigned int c = 0; c < n; c++) {
			int16_t f = k->fine[b0 + c];

			ch[c].pw_run = f < 32767 ? (uint32_t)f << 5 : (uint32_t)k->coarse[b0 + c] << 15;
			channel_judge(c, ch_gate);
		}
	}
	if (prof_on) {
		prof_chan += ccount() - t;
	}
}

/* Fast path, the gate pass: the wideband power of the sub-block at @p sub from its sums. */
static uint32_t gate_power_fast(uint64_t sub)
{
	return esp_sdr_chancap_energy(&snk.b2->ho.fast.sums[(uint32_t)(sub << 3) & SUMS_MASK], 8U,
				      GATE_SHIFT);
}
#endif

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
/*
 * The decimator bank on the kernels: per channel the sub-block mixed once behind its 32 pairs
 * of history (the previous sub-block's, or mixed from the ring after a jump), the filter's
 * outputs straight into the channel's lookback ring, the detector on every 32 samples.
 */
static void run_sub_ddc_fast(uint64_t pair, bool ch_gate)
{
	struct bank2 *b2 = snk.b2;
	const unsigned int n = rs.n, d = rs.d, outs = SUB_PAIRS / d;
	/* The sub-block starts on a multiple of 64 pairs: neither range wraps the ring. */
	uint32_t at = (uint32_t)(pair & HO_MASK);
	uint32_t hist = (uint32_t)((pair - CHANCAP_DDC_L) & HO_MASK);
	bool jump = pair != rs.ddc_next;

	for (unsigned int c = 0; c < n; c++) {
		struct chan *ch = &chs[c];
		struct ddc_kern *k = &ddck[c];
		int16_t *mi = b2->dmi[c], *mq = b2->dmq[c];
		int16_t *iq = lb_slot(c);
		unsigned int pmask = rs.ddc[c].period - 1U;
		struct chancap_mix_job mj = {
			.xi = &b2->ho.raw.hi[at], .xq = &b2->ho.raw.hq[at],
			.tab = k->mix[((uint32_t)pair & pmask) >> 3], .n = SUB_PAIRS,
			.mi = &mi[CHANCAP_DDC_L], .mq = &mq[CHANCAP_DDC_L],
		};
		struct chancap_fir_job fj = {
			.mi = mi, .mq = mq, .h = k->h, .hs = k->hs, .out = iq, .n = outs,
			.shift = CHANCAP_H_SHIFT, .d = d,
		};

		if (jump) {
			struct chancap_mix_job hj = {
				.xi = &b2->ho.raw.hi[hist], .xq = &b2->ho.raw.hq[hist],
				.tab = k->mix[((uint32_t)(pair - CHANCAP_DDC_L) & pmask) >> 3],
				.n = CHANCAP_DDC_L, .mi = mi, .mq = mq,
			};

			esp_sdr_chancap_mix(&hj);
		} else {
			esp_sdr_chancap_copy(mi, &mi[SUB_PAIRS], CHANCAP_DDC_L);
			esp_sdr_chancap_copy(mq, &mq[SUB_PAIRS], CHANCAP_DDC_L);
		}
		esp_sdr_chancap_mix(&mj);
		esp_sdr_chancap_ddc_fir(&fj);
		/* The outputs are in the lookback ring (runs never wrap it); a capture takes them. */
		for (unsigned int r = 0; r < outs; r += 8U) {
			ch->lb_w += 8U;
			ch->samples += 8U;
			if (ch->cap != NULL) {
				copy_run(&ch->cap->iq[2U * ch->cap_n], &iq[2U * r]);
				ch->cap_n += 8U;
				channel_limit(c, ch_gate);
			}
		}
		if ((ch->lb_w & (DET_SAMPLES - 1U)) == 0U) {
			/* The block's power from the ring, sum >> 5 as the C paths' runs add up. */
			const int16_t *blk = &b2->lb.lb[2U * (c * rs.lb_samples +
							      ((ch->lb_w - DET_SAMPLES) & rs.lb_mask))];

			ch->pw_run = esp_sdr_chancap_energy((const frame_t *)blk, DET_SAMPLES / 8U,
							    5U);
			channel_judge(c, ch_gate);
		}
	}
	rs.ddc_next = pair + SUB_PAIRS;
}
#endif

/* Channelize the sub-block at pair @p pair (64 pairs). Flash code: a thread, cached. */
static void run_sub(uint64_t pair, bool ch_gate)
{
	struct bank2 *b2 = snk.b2;

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
	if (rs.fast) {
		run_sub_fast(pair, ch_gate);
		return;
	}
	if (rs.ddc_fast) {
		run_sub_ddc_fast(pair, ch_gate);
		return;
	}
#endif
	if (rs.engine == ENGINE_PFB) {
		const unsigned int m = rs.pfb.m, d = rs.d, l = rs.pfb.l;
		uint64_t j = pair / d;

		/* Runs of 8 frames, transposed into the channels' lookback rings. */
		for (unsigned int run = 0; run < SUB_PAIRS / d / 8U; run++) {
			int16_t fi[8][8], fq[8][8], dummy[16];
			int16_t *dst[8];
			uint32_t p[8];

			for (unsigned int f = 0; f < 8; f++, j++) {
				/* The window ends at pair j d + m - 1 and never wraps (the mirror). */
				uint32_t at = (uint32_t)((j * d + m - l) & HO_MASK);

				chancap_pfb_frame(&rs.pfb, &b2->ho.raw.hi[at], &b2->ho.raw.hq[at],
						  fi[f], fq[f]);
			}
			for (unsigned int lane = 0; lane < 8; lane++) {
				dst[lane] = (lane >= rs.b0 && lane < rs.b0 + rs.n) ? lb_slot(lane - rs.b0)
										  : dummy;
			}
			chancap_run8(fi, fq, dst, p);
			for (unsigned int c = 0; c < rs.n; c++) {
				channel_run(c, dst[rs.b0 + c], chancap_run8_pack(p[rs.b0 + c]), ch_gate);
			}
		}
	} else {
		const unsigned int d = rs.d;
		/* The sub-block starts on a multiple of 64 pairs: neither range wraps the ring. */
		uint32_t at = (uint32_t)(pair & HO_MASK);
		uint32_t hist = (uint32_t)((pair - CHANCAP_DDC_L) & HO_MASK);

		for (unsigned int c = 0; c < rs.n; c++) {
			struct chancap_ddc *ddc = &rs.ddc[c];
			int16_t *mi = b2->dmi[c], *mq = b2->dmq[c];

			/* Mix once per pair; after a jump the window's history first. */
			if (pair != rs.ddc_next) {
				chancap_ddc_mix(ddc, &b2->ho.raw.hi[hist], &b2->ho.raw.hq[hist],
						pair - CHANCAP_DDC_L, CHANCAP_DDC_L, mi, mq);
			}
			chancap_ddc_mix(ddc, &b2->ho.raw.hi[at], &b2->ho.raw.hq[at], pair, SUB_PAIRS,
					&mi[CHANCAP_DDC_L], &mq[CHANCAP_DDC_L]);
			/* Output j's window starts at pair j d + d - L, at mi[j d + d - pair]. */
			for (unsigned int run = 0; run < SUB_PAIRS / d / 8U; run++) {
				int16_t *iq8 = lb_slot(c);
				uint32_t s = 0;

				for (unsigned int f = 0; f < 8; f++) {
					unsigned int w = (run * 8U + f) * d + d;
					int16_t oi, oq;

					chancap_ddc_fir(ddc, &mi[w], &mq[w], &oi, &oq);
					iq8[2U * f] = oi;
					iq8[2U * f + 1U] = oq;
					/* 8 x 2^31 would not fit: sum >> 4 per sample, as the run's power. */
					s += ((uint32_t)((int32_t)oi * oi) + (uint32_t)((int32_t)oq * oq)) >> 4;
				}
				channel_run(c, iq8, chancap_run8_pack(s), ch_gate);
			}
			/* The last L mixed pairs are the next sub-block's history. */
			memcpy(mi, &mi[SUB_PAIRS], 2U * CHANCAP_DDC_L);
			memcpy(mq, &mq[SUB_PAIRS], 2U * CHANCAP_DDC_L);
		}
		rs.ddc_next = pair + SUB_PAIRS;
	}
}

/* ---- the channelizer thread ---- */

static void apply_thresholds(void)
{
	atomic_val_t t = atomic_get(&thresholds);
	int16_t wb = (int16_t)(t & 0xffff), ch = (int16_t)((t >> 16) & 0xffff);
	uint32_t hang_subs = rs.wb_hang_subs;

	if (rs.wb.open_x256 != 0U && wb == rs.cfg.wb_threshold_db && ch == rs.cfg.ch_threshold_db) {
		return;
	}
	rs.cfg.wb_threshold_db = wb;
	rs.cfg.ch_threshold_db = ch;
	/* The floors survive a threshold change. */
	{
		uint32_t f = rs.wb.floor;

		chancap_det_init(&rs.wb, wb, hang_subs);
		if (f != 0U) {
			rs.wb.floor = f;
			rs.wb.warmup = 0;
		}
	}
	for (unsigned int c = 0; c < rs.n; c++) {
		uint32_t f = chs[c].det.floor;

		chancap_det_init(&chs[c].det, ch, rs.hang_det_blocks);
		if (f != 0U) {
			chs[c].det.floor = f;
			chs[c].det.warmup = 0;
		}
	}
}

/*
 * One block: the gate pass judges the sub-blocks up to the written position; the channelizer
 * pass follows lb_subs behind and runs where the gate is open or opens within the lookback.
 */
static bool dsp_block(uint64_t written)
{
	unsigned int g = (unsigned int)atomic_get(&gates);
	bool wb_gate = (g & GATE_WB) != 0U, ch_gate = (g & GATE_CH) != 0U;
	struct bank2 *b2 = snk.b2;
	uint64_t sub_end = written >> SUB_SHIFT;
	uint64_t run_end;
	uint32_t t0 = ccount(), dt;
	uint64_t run0 = rs.st.pairs_run;
	bool did = false;

	apply_thresholds();
	/* Gate pass. */
	while (rs.gate_sub < sub_end) {
		uint32_t p;

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
		if (rs.fast) {
			/* Skipped with the gate off: its power is not needed then. */
			p = wb_gate ? gate_power_fast(rs.gate_sub) : 0U;
		} else
#endif
		{
			p = b2->sub_power[rs.gate_sub & (HO_SUBS - 1U)];
		}
		if (chancap_det_update(&rs.wb, p) || !wb_gate) {
			rs.open_sub = rs.gate_sub;
		}
		rs.gate_sub++;
	}
	/* Channelizer pass, lb_subs behind the gate, one block at most. */
	run_end = (rs.gate_sub >= rs.lb_subs ? rs.gate_sub - rs.lb_subs : 0U) << SUB_SHIFT;
	if (run_end > rs.run_pair + BLOCK_PAIRS) {
		run_end = rs.run_pair + BLOCK_PAIRS;
	}
	while (rs.run_pair < run_end) {
		bool run = rs.open_sub != ~0ULL && rs.open_sub >= (rs.run_pair >> SUB_SHIFT);

		if (run) {
			if (!rs.running) {
				run_restart(rs.run_pair);
			}
			uint32_t t1 = ccount();

			run_sub(rs.run_pair, ch_gate);
			rs.st.run_max = MAX(rs.st.run_max, ccount() - t1);
			rs.st.pairs_run += SUB_PAIRS;
			did = true;
		} else if (rs.running) {
			run_stop(false);
		}
		rs.run_pair += SUB_PAIRS;
	}
	dt = ccount() - t0;
	if (did) {
		/* Per pair channelized: the gate pass's share is included. */
		rs.cyc += dt;
		rs.cyc_pairs += rs.st.pairs_run - run0;
		rs.st.block_max = MAX(rs.st.block_max, dt);
	}
	return did;
}

static void dsp_resync(uint64_t written)
{
	/* Follow from the newest whole block; the passes start lb_subs apart. */
	uint64_t start = written & ~(uint64_t)(BLOCK_PAIRS - 1U);

	if (rs.running) {
		run_stop(true);
	}
	rs.gate_sub = start >> SUB_SHIFT;
	rs.run_pair = start;
	rs.open_sub = ~0ULL;
}

static void dsp_thread_fn(void *p1, void *p2, void *p3)
{
	uint64_t written = 0;
	bool synced = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	bool wmac;

	/* The kernels: this thread is the vector unit's only user on its CPU. */
	pie_enable();
	/*
	 * Wi-Fi started on this CPU, so its MAC interrupt is here: on busy air it took this CPU
	 * for 400 us at a time, longer than the hand-over ring's slack.
	 */
	wmac = esp_sdr_ring_wmac_hold();
	while (atomic_get(&stop_req) == 0) {
		atomic_val_t e = atomic_get(&snk.epoch);

		if (!ho_written(&written)) {
			k_sleep(K_USEC(100));
			continue;
		}
		if (e != rs.epoch || !synced) {
			rs.epoch = e;
			synced = true;
			rs.st.gaps = snk.gaps;
			dsp_resync(written);
		}
		rs.st.lag_max = (uint32_t)MAX((uint64_t)rs.st.lag_max, written - rs.run_pair);
		/*
		 * Fallen behind past the ring's slack: skip to the newest data, and let the lower
		 * threads run for a tick (the band is busier than the channelizer keeps up with).
		 */
		if (written - rs.run_pair > LAG_MAX) {
			rs.st.overruns++;
			dsp_resync(written);
			k_sleep(K_TICKS(1));
			continue;
		}
		rs.st.pairs = written;
		if (written < rs.run_pair + SUB_PAIRS + ((uint64_t)rs.lb_subs << SUB_SHIFT)) {
			/*
			 * Caught up: a sub-block of input is 4 us away; a tick's sleep (up to two
			 * ticks with the tickless kernel: the chancap images run 20000 ticks a
			 * second, so 100 us at most) lets the other threads run.
			 */
			k_sleep(K_TICKS(1));
			continue;
		}
		/* Up to a block; higher threads preempt, lower ones wait for the sleep. */
		(void)dsp_block(written);
	}
	if (rs.running) {
		run_stop(true);
	}
	esp_sdr_ring_wmac_release(wmac);
}

static void out_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		struct capbuf *b;

		if (k_msgq_get(&out_q, &b, K_MSEC(20)) == 0) {
			b->cap.peak_db = (int16_t)lroundf(chancap_power_db(b->peak_pw));
			b->cap.noise_db = (int16_t)lroundf(chancap_power_db(b->noise_pw));
			rs.cfg.cb(rs.cfg.user, &b->cap);
		} else if (atomic_get(&stop_req) != 0) {
			break;
		}
	}
}

static void ring_thread_fn(void *p1, void *p2, void *p3)
{
	struct esp_sdr_ring_cfg cfg = {
		.decim = ESP_SDR_RING_DECIM_RAW,
		.sink = sink,
		.unit_pairs = UNIT_PAIRS,
		.spare_bank_app = true,
	};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (atomic_get(&stop_req) == 0) {
		int ret;

		snk.expect = ~0ULL;
		/* The ring fills ring_st as it runs: the status shows the live counters. */
		ret = esp_sdr_ring_run(&cfg, &ring_st);
		if (ret != 0 && atomic_get(&stop_req) == 0) {
			LOG_WRN("chancap ring stopped: %d, status %d detail 0x%08x", ret,
				ring_st.status, ring_st.detail);
			k_sleep(K_MSEC(10));
		}
	}
}

/* ---- API ---- */

static void lock_init(void)
{
	if (!lock_ready) {
		k_mutex_init(&lock);
		lock_ready = true;
	}
}

int esp_sdr_chancap_start(const struct esp_sdr_chancap_cfg *cfg)
{
	int ret;

	lock_init();
	k_mutex_lock(&lock, K_FOREVER);
	if (atomic_get(&active) != 0) {
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	memset(&rs, 0, sizeof(rs));
	memset(&snk, 0, sizeof(snk));
	/* Else the status shows the last run's ring counters until the new run starts. */
	memset(&ring_st, 0, sizeof(ring_st));
	rs.cfg = *cfg;
	ret = plan(cfg);
	if (ret != 0) {
		k_mutex_unlock(&lock);
		return ret;
	}
	ret = tune();
	if (ret != 0) {
		k_mutex_unlock(&lock);
		return ret;
	}
	setup(cfg);
	if (rs.nbufs == 0U) {
		k_mutex_unlock(&lock);
		return -ENOMEM;
	}
	atomic_set(&gates, (cfg->wb_gate ? GATE_WB : 0) | (cfg->ch_gate ? GATE_CH : 0));
	atomic_set(&thresholds, ((atomic_val_t)(uint16_t)cfg->ch_threshold_db << 16) |
					(uint16_t)cfg->wb_threshold_db);
	rs.open_sub = ~0ULL;
	atomic_set(&stop_req, 0);
	k_msgq_purge(&out_q);
	atomic_set(&active, 1);

	k_thread_create(&ring_thread, ring_stack, K_KERNEL_STACK_SIZEOF(ring_stack),
			ring_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(2), K_FP_REGS, K_FOREVER);
	k_thread_create(&dsp_thread, dsp_stack, K_KERNEL_STACK_SIZEOF(dsp_stack), dsp_thread_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_ESP_SDR_CHANCAP_DSP_PRIORITY),
			K_FP_REGS, K_FOREVER);
	k_thread_create(&out_thread, out_stack, K_KERNEL_STACK_SIZEOF(out_stack), out_thread_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_ESP_SDR_CHANCAP_DSP_PRIORITY + 1),
			K_FP_REGS, K_FOREVER);
	k_thread_name_set(&ring_thread, "chancap_ring");
	k_thread_name_set(&dsp_thread, "chancap_dsp");
	k_thread_name_set(&out_thread, "chancap_out");
#if defined(CONFIG_SCHED_CPU_MASK)
	if (arch_num_cpus() > 1) {
		k_thread_cpu_pin(&ring_thread, CONFIG_ESP_SDR_CHANCAP_RING_CPU);
		k_thread_cpu_pin(&dsp_thread, 1 - CONFIG_ESP_SDR_CHANCAP_RING_CPU);
		k_thread_cpu_pin(&out_thread, 1 - CONFIG_ESP_SDR_CHANCAP_RING_CPU);
	}
#endif
	k_thread_start(&out_thread);
	k_thread_start(&dsp_thread);
	k_thread_start(&ring_thread);
	k_mutex_unlock(&lock);
	LOG_INF("chancap: %u channels, %s m %u d %u%s, LO %llu Hz, %u buffers of %u samples",
		rs.n, rs.plan.pfb ? "pfb" : "ddc", rs.plan.m, rs.plan.d,
		rs.fast ? " (both CPUs)" : "", (unsigned long long)rs.plan.lo_hz, rs.nbufs,
		rs.max_samples);
	return 0;
}

void esp_sdr_chancap_stop(void)
{
	lock_init();
	k_mutex_lock(&lock, K_FOREVER);
	if (atomic_get(&active) != 0) {
		atomic_set(&stop_req, 1);
		esp_sdr_ring_stop();
		/*
		 * The ring thread polls the stop request; one that does not end in 2 s is wedged
		 * on its CPU. Waiting forever would hold the lock and the caller's thread (the
		 * shell): leave chancap active (start answers -EBUSY) and let a later stop retry.
		 */
		if (k_thread_join(&ring_thread, K_SECONDS(2)) != 0) {
			LOG_ERR("chancap stop: the ring thread did not end (units %u, pairs %llu)",
				ring_st.units, (unsigned long long)ring_st.pairs);
			k_mutex_unlock(&lock);
			return;
		}
		k_thread_join(&dsp_thread, K_FOREVER);
		k_thread_join(&out_thread, K_FOREVER);
		/* Captures still queued never reach the consumer. */
		{
			struct capbuf *b;

			while (k_msgq_get(&out_q, &b, K_NO_WAIT) == 0) {
				buf_put(b);
			}
		}
		rs.st.cycles_x100 =
			rs.cyc_pairs != 0U ? (uint32_t)(rs.cyc * 100U / rs.cyc_pairs) : 0U;
		atomic_set(&active, 0);
	}
	k_mutex_unlock(&lock);
}

bool esp_sdr_chancap_active(void)
{
	return atomic_get(&active) != 0;
}

int esp_sdr_chancap_set_gates(bool wideband, bool per_channel)
{
	if (atomic_get(&active) == 0) {
		return -EINVAL;
	}
	atomic_set(&gates, (wideband ? GATE_WB : 0) | (per_channel ? GATE_CH : 0));
	return 0;
}

int esp_sdr_chancap_set_thresholds(int16_t wb_db, int16_t ch_db)
{
	if (atomic_get(&active) == 0) {
		return -EINVAL;
	}
	atomic_set(&thresholds, ((atomic_val_t)(uint16_t)ch_db << 16) | (uint16_t)wb_db);
	return 0;
}

void esp_sdr_chancap_get_stats(struct esp_sdr_chancap_stats *stats)
{
	*stats = rs.st;
	stats->ring = ring_st;
	stats->gaps = snk.gaps;
	if (atomic_get(&active) != 0 && rs.cyc_pairs != 0U) {
		stats->cycles_x100 = (uint32_t)(rs.cyc * 100U / rs.cyc_pairs);
	}
}

void esp_sdr_chancap_get_plan(struct esp_sdr_chancap_plan *plan_out)
{
	*plan_out = rs.plan;
}

/* ---- bench ---- */

static void bench_cb(void *user, const struct esp_sdr_chancap *cap)
{
	ARG_UNUSED(user);
	esp_sdr_chancap_release(cap);
}

/*
 * The sink's input as the ring hands it over: internal RAM, the stream's previous
 * ESP_SDR_RING_RAW_HISTORY pairs in front of each block.
 */
#define BENCH_H ESP_SDR_RING_RAW_HISTORY
static int16_t bench_i[BENCH_H + BLOCK_PAIRS] __aligned(16) ESP_SDR_HIGH_RAM;
static int16_t bench_q[BENCH_H + BLOCK_PAIRS] __aligned(16) ESP_SDR_HIGH_RAM;

/*
 * Noise-like input through the sink: the hand-over ring (and the fast path's sums) filled.
 * Returns the sink's cycles, interrupts masked around each call.
 */
static uint32_t bench_fill(uint32_t pairs)
{
	uint32_t x = 12345, cyc = 0;

	memset(bench_i, 0, sizeof(bench_i));
	memset(bench_q, 0, sizeof(bench_q));
	for (uint32_t p = 0; p < pairs; p += BLOCK_PAIRS) {
		unsigned int key;
		uint32_t t;

		memmove(bench_i, &bench_i[BLOCK_PAIRS], 2U * BENCH_H);
		memmove(bench_q, &bench_q[BLOCK_PAIRS], 2U * BENCH_H);
		for (uint32_t k = 0; k < BLOCK_PAIRS; k++) {
			x = x * 1664525U + 1013904223U;
			bench_i[BENCH_H + k] = (int16_t)((x >> 16) & 0x3fffU) - 8192;
			x = x * 1664525U + 1013904223U;
			bench_q[BENCH_H + k] = (int16_t)((x >> 16) & 0x3fffU) - 8192;
		}
		key = arch_irq_lock();
		t = ccount();
		sink(NULL, &bench_i[BENCH_H], &bench_q[BENCH_H], BLOCK_PAIRS, p);
		cyc += ccount() - t;
		arch_irq_unlock(key);
	}
	return cyc;
}

#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
/*
 * The fast path's kernels against the C reference on what bench_fill() left: the branch sums of
 * the last block's frames (their windows still in the bench buffer), then post, power and the
 * transpose on 8 of them.
 */
static void bench_check_fast(uint64_t written, struct esp_sdr_chancap_bench_result *res)
{
	struct bank2 *b2 = snk.b2;
	static frame_t got[8] __aligned(16) ESP_SDR_HIGH_RAM;
	static frame_t sums8[8] __aligned(16) ESP_SDR_HIGH_RAM;
	uint64_t jend = written >> 3, j0 = jend - BLOCK_PAIRS / 8U;
	/* About 2 KB: never on the caller's stack. */
	static struct chancap_pfb ref EXT_RAM_BSS_ATTR;
	ref = rs.pfb;
	for (uint64_t j = j0; j < jend; j++) {
		/* Frame j's window starts FAST_HIST pairs before its first new pair. */
		uint32_t start = BENCH_H + (uint32_t)((j - j0) << 3) - FAST_HIST;
		const frame_t *s = &b2->ho.fast.sums[(uint32_t)j & SUMS_MASK];
		int16_t ui[8], uq[8];

		chancap_pfb_sums(&ref, &bench_i[start], &bench_q[start], ui, uq);
		for (unsigned int i = 0; i < 8; i++) {
			if ((*s)[i] != ui[i] || (*s)[8U + i] != uq[i]) {
				if (res->mismatches < 4U) {
					LOG_WRN("sums frame %u lane %u: kernel %d %d, C %d %d",
						(unsigned int)j, i, (*s)[i], (*s)[8U + i], ui[i], uq[i]);
				}
				res->mismatches++;
			}
		}
	}
	/* Post on 8 frames from an even one, then their power. */
	memcpy(sums8, &b2->ho.fast.sums[(uint32_t)j0 & SUMS_MASK], sizeof(sums8));
	{
		struct chancap_post_job pj = {.in = sums8, .out = got, .n = 8U,
					      .tab = b2->kern.post[0]};

		esp_sdr_chancap_post_n(&pj);
	}
	ref.frames = j0;
	for (unsigned int f = 0; f < 8; f++) {
		int16_t ui[8], uq[8], oi[8], oq[8];

		memcpy(ui, &sums8[f][0], 16);
		memcpy(uq, &sums8[f][8], 16);
		chancap_pfb_post(&ref, ui, uq, oi, oq);
		for (unsigned int c = 0; c < 8; c++) {
			if (got[f][c] != oi[c] || got[f][8U + c] != oq[c]) {
				if (res->mismatches < 4U) {
					LOG_WRN("post frame %u lane %u: kernel %d %d, C %d %d", f, c,
						got[f][c], got[f][8U + c], oi[c], oq[c]);
				}
				res->mismatches++;
			}
		}
	}
	/* The detector's power of 32 frames (of sums here), and the gate's energy. */
	{
		const frame_t *fr = &b2->ho.fast.sums[(uint32_t)j0 & SUMS_MASK];
		struct chancap_power_job wj = {
			.frames = fr, .fine = b2->kern.fine, .coarse = b2->kern.coarse,
			.n = DET_SAMPLES, .sf = DET_SF, .sc = DET_SC,
		};
		int16_t rf[8], rc[8];
		uint32_t e, er;

		esp_sdr_chancap_power(&wj);
		chancap_power_n((const int16_t(*)[16])fr, wj.n, wj.sf, wj.sc, rf, rc);
		for (unsigned int c = 0; c < 8; c++) {
			if (b2->kern.fine[c] != rf[c] || b2->kern.coarse[c] != rc[c]) {
				if (res->mismatches < 4U) {
					LOG_WRN("power lane %u: kernel %d %d, C %d %d", c,
						b2->kern.fine[c], b2->kern.coarse[c], rf[c], rc[c]);
				}
				res->mismatches++;
			}
		}
		for (unsigned int t = 0; t < 2; t++) {
			const frame_t *g = t == 0U ? got : fr;

			e = esp_sdr_chancap_energy(g, 8U, GATE_SHIFT);
			er = chancap_energy((const int16_t(*)[16])g, 8U, GATE_SHIFT);
			if (e != er) {
				if (res->mismatches < 4U) {
					LOG_WRN("energy: kernel %u, C %u", e, er);
				}
				res->mismatches++;
			}
		}
	}
	/* The transpose: lane c's sample f is frame f's lane c. */
	{
		struct kern *k = &b2->kern;
		struct chancap_transpose_job tj = {.frames = got, .out = k->lanes, .si = k->si,
						   .sq = k->sq};

		esp_sdr_chancap_transpose8(&tj);
		for (unsigned int c = 0; c < 8; c++) {
			for (unsigned int f = 0; f < 8; f++) {
				if (k->lanes[c][2U * f] != got[f][c] ||
				    k->lanes[c][2U * f + 1U] != got[f][8U + c]) {
					if (res->mismatches < 4U) {
						LOG_WRN("transpose lane %u frame %u", c, f);
					}
					res->mismatches++;
				}
			}
		}
	}
}

/*
 * The decimator kernels against the C reference on what bench_fill() left in the raw ring:
 * channel 0's history and sub-block mixed, then its outputs.
 */
static void bench_check_ddc(uint64_t written, struct esp_sdr_chancap_bench_result *res)
{
	struct bank2 *b2 = snk.b2;
	static int16_t ref_mi[DDC_BUF] ESP_SDR_HIGH_RAM, ref_mq[DDC_BUF] ESP_SDR_HIGH_RAM;
	static int16_t out[2U * SUB_PAIRS / 4U] ESP_SDR_HIGH_RAM;
	const struct chancap_ddc *ddc = &rs.ddc[0];
	struct ddc_kern *k = &ddck[0];
	int16_t *mi = b2->dmi[0], *mq = b2->dmq[0];
	uint64_t pair = written - BLOCK_PAIRS;
	uint32_t at = (uint32_t)(pair & HO_MASK), hist = (uint32_t)((pair - CHANCAP_DDC_L) & HO_MASK);
	unsigned int pmask = ddc->period - 1U, outs = SUB_PAIRS / rs.d;
	struct chancap_mix_job hj = {
		.xi = &b2->ho.raw.hi[hist], .xq = &b2->ho.raw.hq[hist],
		.tab = k->mix[((uint32_t)(pair - CHANCAP_DDC_L) & pmask) >> 3], .n = CHANCAP_DDC_L,
		.mi = mi, .mq = mq,
	};
	struct chancap_mix_job mj = {
		.xi = &b2->ho.raw.hi[at], .xq = &b2->ho.raw.hq[at],
		.tab = k->mix[((uint32_t)pair & pmask) >> 3], .n = SUB_PAIRS,
		.mi = &mi[CHANCAP_DDC_L], .mq = &mq[CHANCAP_DDC_L],
	};
	struct chancap_fir_job fj = {.mi = mi, .mq = mq, .h = k->h, .hs = k->hs, .out = out,
				     .n = outs, .shift = CHANCAP_H_SHIFT, .d = rs.d};

	esp_sdr_chancap_mix(&hj);
	esp_sdr_chancap_mix(&mj);
	chancap_ddc_mix(ddc, &b2->ho.raw.hi[hist], &b2->ho.raw.hq[hist], pair - CHANCAP_DDC_L,
			DDC_BUF, ref_mi, ref_mq);
	for (unsigned int t = 0; t < DDC_BUF; t++) {
		if (mi[t] != ref_mi[t] || mq[t] != ref_mq[t]) {
			if (res->mismatches < 4U) {
				LOG_WRN("mix pair %u: kernel %d %d, C %d %d", t, mi[t], mq[t],
					ref_mi[t], ref_mq[t]);
			}
			res->mismatches++;
		}
	}
	esp_sdr_chancap_ddc_fir(&fj);
	for (unsigned int j = 0; j < outs; j++) {
		int16_t oi, oq;

		chancap_ddc_fir(ddc, &ref_mi[rs.d * j + rs.d], &ref_mq[rs.d * j + rs.d], &oi, &oq);
		if (out[2U * j] != oi || out[2U * j + 1U] != oq) {
			if (res->mismatches < 4U) {
				LOG_WRN("fir output %u: kernel %d %d, C %d %d", j, out[2U * j],
					out[2U * j + 1U], oi, oq);
			}
			res->mismatches++;
		}
	}
}

/* Each fast kernel alone, interrupts masked: cycles per call. */
static void bench_kernels(struct esp_sdr_chancap_bench_result *res)
{
	struct bank2 *b2 = snk.b2;
	static frame_t out8[8] __aligned(16) ESP_SDR_HIGH_RAM;
	/* The last block's 64 frames, from the bench buffer as the sink computes them. */
	struct chancap_sums_job sj = {
		.xi = &bench_i[BENCH_H - FAST_HIST], .xq = &bench_q[BENCH_H - FAST_HIST],
		.hk = b2->kern.hk, .k = rs.pfb.k, .n = BLOCK_PAIRS / 8U, .out = b2->ho.fast.sums,
		.shift = CHANCAP_H_SHIFT, .stride = 16U,
	};
	struct chancap_post_job pj = {.in = b2->ho.fast.sums, .out = out8, .n = 8U,
				      .tab = b2->kern.post[0]};
	/* The detector's call: 32 frames (the sums ring's as data). */
	struct chancap_power_job wj = {.frames = b2->ho.fast.sums, .fine = b2->kern.fine,
				       .coarse = b2->kern.coarse, .n = DET_SAMPLES, .sf = DET_SF,
				       .sc = DET_SC};
	unsigned int key = arch_irq_lock();
	uint32_t t;

	t = ccount();
	for (unsigned int r = 0; r < 4; r++) {
		esp_sdr_chancap_sums_n(&sj);
	}
	res->sums_cyc = (ccount() - t) / (4U * 64U);
	t = ccount();
	for (unsigned int r = 0; r < 64; r++) {
		esp_sdr_chancap_post_n(&pj);
	}
	res->post_cyc = (ccount() - t) / 64U;
	t = ccount();
	for (unsigned int r = 0; r < 64; r++) {
		esp_sdr_chancap_power(&wj);
	}
	res->power_cyc = (ccount() - t) / 64U;
	arch_irq_unlock(key);
}
#endif

int esp_sdr_chancap_bench(const struct esp_sdr_chancap_cfg *cfg, uint32_t pairs,
			  struct esp_sdr_chancap_bench_result *res)
{
	struct esp_sdr_chancap_cfg c = *cfg;
	int ret;

	lock_init();
	k_mutex_lock(&lock, K_FOREVER);
	if (atomic_get(&active) != 0) {
		k_mutex_unlock(&lock);
		return -EBUSY;
	}
	if (pairs == 0U || pairs % BLOCK_PAIRS != 0U) {
		k_mutex_unlock(&lock);
		return -EINVAL;
	}
	memset(&rs, 0, sizeof(rs));
	memset(&snk, 0, sizeof(snk));
	memset(res, 0, sizeof(*res));
	c.cb = bench_cb;
	c.user = NULL;
	rs.cfg = c;
	ret = plan(&c);
	if (ret != 0) {
		k_mutex_unlock(&lock);
		return ret;
	}
	setup(&c);
	res->fast = rs.fast;
	pie_enable();
	atomic_set(&gates, GATE_CH);
	atomic_set(&thresholds, ((atomic_val_t)(uint16_t)c.ch_threshold_db << 16) |
					(uint16_t)c.wb_threshold_db);
	/* The ring's CPU: unpack and sink, as a run has them (the fast path's sums included). */
	{
		struct esp_sdr_ring_cfg rcfg = {.decim = ESP_SDR_RING_DECIM_RAW, .sink = sink};
		struct esp_sdr_ring_bench_result rres;

		snk.expect = ~0ULL;
		if (esp_sdr_ring_bench(&rcfg, 4, &rres) == 0) {
			res->ring_x100 = rres.cycles_x100;
			res->ring_unpack_x100 = rres.stage_x100[0];
			res->ring_sink_x100 = rres.stage_x100[1];
			res->ring_hist_x100 = rres.stage_x100[3];
		}
	}
	/* The hand-over ring full of known input, then the channelizer over it, every sub-block. */
	snk.expect = ~0ULL;
	res->sink_x100 = (uint32_t)((uint64_t)bench_fill(HO_PAIRS) * 100U / HO_PAIRS);
#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
	if (rs.fast) {
		bench_check_fast(HO_PAIRS, res);
		bench_kernels(res);
		snk.expect = ~0ULL;
		bench_fill(HO_PAIRS);
	}
#endif
	{
		uint32_t t_run = 0;

		rs.lb_subs = 2;
		rs.run_pair = 0;
		apply_thresholds();
		prof_kern = 0;
		prof_chan = 0;
		prof_on = true;
		for (uint32_t p = 0; p < pairs; p += SUB_PAIRS) {
			unsigned int key = arch_irq_lock();
			uint32_t t1 = ccount();

			if (!rs.running) {
				run_restart(p);
			}
			/* The data wraps in the hand-over ring: every sub-block is a valid one. */
			run_sub((uint64_t)p, true);
			t_run += ccount() - t1;
			arch_irq_unlock(key);
		}
		prof_on = false;
		res->block_x100 = (uint32_t)((uint64_t)t_run * 100U / pairs);
		res->kern_x100 = (uint32_t)((uint64_t)prof_kern * 100U / pairs);
		res->chan_x100 = (uint32_t)((uint64_t)prof_chan * 100U / pairs);
	}
	/* The engines alone, on the C paths. */
	if (!rs.fast && rs.engine == ENGINE_PFB) {
		int16_t oi[CHANNELS_MAX], oq[CHANNELS_MAX];
		uint32_t t1 = ccount();
		unsigned int frames = pairs / rs.d;

		for (unsigned int j = 0; j < frames; j++) {
			uint32_t at = (uint32_t)(((uint64_t)j * rs.d + rs.pfb.m - rs.pfb.l) & HO_MASK);

			chancap_pfb_frame(&rs.pfb, &snk.b2->ho.raw.hi[at], &snk.b2->ho.raw.hq[at], oi,
					  oq);
		}
		res->pfb_x100 = (uint32_t)((uint64_t)(ccount() - t1) * 100U / pairs);
#if defined(CONFIG_ESP_SDR_CHANCAP_SIMD)
	} else if (rs.ddc_fast) {
		/* One channel on the kernels, as run_sub() runs it. */
		struct ddc_kern *k = &ddck[0];
		int16_t *mi = snk.b2->dmi[0], *mq = snk.b2->dmq[0];
		struct chancap_mix_job mj = {
			.xi = snk.b2->ho.raw.hi, .xq = snk.b2->ho.raw.hq, .tab = k->mix[0],
			.n = SUB_PAIRS, .mi = &mi[CHANCAP_DDC_L], .mq = &mq[CHANCAP_DDC_L],
		};
		struct chancap_fir_job fj = {.mi = mi, .mq = mq, .h = k->h, .hs = k->hs,
					     .out = snk.b2->lb.lb, .n = SUB_PAIRS / rs.d,
					     .shift = CHANCAP_H_SHIFT, .d = rs.d};
		uint32_t t1;

		bench_check_ddc(HO_PAIRS, res);
		t1 = ccount();
		for (uint32_t p = 0; p < pairs; p += SUB_PAIRS) {
			esp_sdr_chancap_copy(mi, &mi[SUB_PAIRS], CHANCAP_DDC_L);
			esp_sdr_chancap_copy(mq, &mq[SUB_PAIRS], CHANCAP_DDC_L);
			esp_sdr_chancap_mix(&mj);
			esp_sdr_chancap_ddc_fir(&fj);
		}
		res->ddc_x100 = (uint32_t)((uint64_t)(ccount() - t1) * 100U / pairs);
#endif
	} else if (rs.engine == ENGINE_DDC) {
		/* One channel as run_sub() runs it: a mix per pair, a filter per output. */
		static volatile int32_t ddc_sink;
		struct chancap_ddc *ddc = &rs.ddc[0];
		int16_t *mi = snk.b2->dmi[0], *mq = snk.b2->dmq[0];
		int16_t oi, oq;
		uint32_t t1 = ccount();

		for (uint32_t p = 0; p < pairs; p += SUB_PAIRS) {
			uint32_t at = p & HO_MASK;

			chancap_ddc_mix(ddc, &snk.b2->ho.raw.hi[at], &snk.b2->ho.raw.hq[at], p,
					SUB_PAIRS, &mi[CHANCAP_DDC_L], &mq[CHANCAP_DDC_L]);
			for (unsigned int w = rs.d; w <= SUB_PAIRS; w += rs.d) {
				chancap_ddc_fir(ddc, &mi[w], &mq[w], &oi, &oq);
				/* A volatile sink, or the compiler drops the arithmetic. */
				ddc_sink += oi + oq;
			}
			memcpy(mi, &mi[SUB_PAIRS], 2U * CHANCAP_DDC_L);
			memcpy(mq, &mq[SUB_PAIRS], 2U * CHANCAP_DDC_L);
		}
		res->ddc_x100 = (uint32_t)((uint64_t)(ccount() - t1) * 100U / pairs);
	}
	res->kernels_match = res->mismatches == 0U;
	if (rs.running) {
		run_stop(true);
	}
	{
		struct capbuf *b;

		while (k_msgq_get(&out_q, &b, K_NO_WAIT) == 0) {
			buf_put(b);
		}
	}
	k_mutex_unlock(&lock);
	return 0;
}
