/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The CPU 1 thread of the transmit stream, after the loop of doc/dev/txs.md. The engine loops over
 * 32768 words (one group); the ownership register names the group it reads. The model is the CPU
 * cycle counter at the engine's start (txs_pos): pass p plays group 2 (p even) or group 1 (p odd),
 * stream indices p x 32768 up, and the window [next, next + 32768) is the group the CPU owns.
 * The first pass is zeros: the first window is filled by the packets of that pass.
 *
 * The thread is created stopped, pinned to CPU 1, then started. It runs with interrupts open,
 * except from the lock point before a flip on: the network lives on CPU 0. The deadman is a general
 * purpose timer whose interrupt is on CPU 0: no flip for 2.5 passes and it stops the engine from the
 * interrupt, the control thread then turns the chain off.
 *
 * Who does what: the shell thread starts a run, the flip thread (CPU 1) plays it, the control
 * thread (the main thread, which runs tx_stream_ctl_run() for good) turns the chain on at the start
 * of an RF run and off at its end, in that order on the same thread (esp_sdr_replay_start() leaves
 * esp_sdr_lock owned by the caller until esp_sdr_replay_stop()), and prints the report. The engine (DAC_TRIG) is turned off by whoever
 * ends the run first: the flip thread at its end, whatever the reason, or the deadman.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/init.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>

#include <soc/hp_system_reg.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_replay.h>

#include "tx_inject.h"
#include "tx_stream.h"
#include "tx_tone.h"
#include "txs_flip.h"
#include "txs_jit.h"
#include "txs_pos.h"

/* The library's engine calls (lib/esp_sdr/src/esp_sdr_priv.h) and the PIE's enable */
extern bool esp_sdr_ready;
int esp_sdr_s31_dac_start(unsigned int rate_code, uint32_t ctrl, uint32_t *t0);
void esp_sdr_s31_dac_stop(void);
#if defined(CONFIG_ESP_SDR_TXS_PIE)
uint32_t txs_pie_enable(void);
#endif

#define OWN_REG   HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG /* ownership: 7:0 group 0 ... 31:24 group 3 */
#define DAC_TRIG  0x2010900cU
#define OWN_G2    0x00ff0000U
#define OWN_G1    0x0000ff00U
#define TRIG_RUN  BIT(31)
#define TRIG_LOOP BIT(19)
#define RATE_40   1U
#define RATE_20   3U

#define US(x)       ((uint32_t)(x) * (ESP_SDR_CPU_HZ / 1000000U))
#define CHUNK       702U /* most words a fill step handles: a whole packet; the clock cuts it short near the flip */
#define CAL_N       128U /* words of the calibration calls (a per word share with the call overhead in it) */
#define VER_N       16U /* words of the read back a turn: it must not push the lock lead */
/* Interrupts off, on this core only (arch_irq_lock(): with SMP irq_lock() is the global lock, which CPU 0
 * would wait for with its own interrupts masked, the deadman's too), for this long before a flip. The
 * time to the lock must outlast an interrupt that arrives just before it. */
#define LOCK_US     10U
#define HANG_PASSES 64U
#define DEAD_HALVES 5U /* deadman ticks (half a pass each) without a flip: 2.5 passes */
#define DEAD_PRE_MS 200U /* before the first flip */
#define RING        16U
#define PEND        16U
#define CAL_REPS    16U
/* TX_SRC_NET: the engine starts when the first window is complete, or this long after the first
 * packet; with no packet at all the run ends after PRE_WAIT_MS (before the deadman's 200 ms). */
#define PRE_FILL_US 2000U
#define PRE_WAIT_MS 150U
#define PRE_ST_US   100U

/* the pool: the stream's hold, the MAC's descriptors and two for the stack (ND, ARP, control); the injector holds
 * TX_BUFFERS of its own and the injector.conf pool is two buffers smaller */
/* the hold is in the ring and the thread's list together */
BUILD_ASSERT(TX_HOLD <= RING + PEND, "the ring and the list cannot hold TX_HOLD buffers");
BUILD_ASSERT(IS_ENABLED(CONFIG_APP_TXS_INJECT) ||
	     TX_HOLD + CONFIG_DWMAC_NB_RX_DESCS + 2 == CONFIG_NET_BUF_RX_COUNT,
	     "the stream's hold, the MAC's descriptors and the stack's two are not the pool (src/tx_hold.h, prj.conf)");
BUILD_ASSERT(!IS_ENABLED(CONFIG_APP_TXS_INJECT) ||
	     TX_BUFFERS + CONFIG_DWMAC_NB_RX_DESCS + 2 <= CONFIG_NET_BUF_RX_COUNT,
	     "the injector's buffers, the MAC's descriptors and the stack's two do not fit the pool (injector.conf)");

enum { S_IDLE, S_RUN, S_ENDING, S_FIN };
/* the control thread's requests */
enum { CTL_START = BIT(0), CTL_OFF = BIT(1), CTL_FIN = BIT(2) };
#define STOP_JOIN_MS  100

struct pend {
	struct net_buf *buf;
	const uint8_t *src;
	uint64_t idx;
	uint32_t n;
	uint32_t t_taken;
	bool wait;
};

struct ring_item {
	struct net_buf *buf;
	uint16_t off, len;
};

static struct {
	atomic_t state, dead, flips_pub, hang, ready, inflight;
	/* producers inside tx_stream_put(); abort_pre: end a TX_SRC_NET run before its engine started;
	 * st_flips: statuses published after a flip */
	atomic_t putters, abort_pre, st_flips, snap_ok, st_flip_ts;
	struct tx_cfg cfg;
	tx_done_t done;
	uint32_t total_flips;
	uint64_t st_flips_seen;
	int64_t t_start_ms;
	uint32_t dead_idle, dead_seen, dead_pre, dead_pre_max;
	uint32_t last_kind, last_cyc, last_end, ms_start;
	bool first_seen;
	uint64_t pk_flushed;
	/* the flip thread's */
	struct txs_pos p;
	struct txs_flip f;
	struct txs_jit j;
	struct txs_wire_cfg wcfg;
	struct txs_snap snap;
	struct tx_stats ls;
	uint32_t depth;
	struct pend pend[PEND];
	uint32_t npend;
	/* the ring: CPU 0 puts, the flip thread takes */
	struct ring_item ring[RING];
	atomic_t head, tail;
	/* the stats copy for CPU 0 (a sequence lock) */
	atomic_t pub_seq;
} st ESP_SDR_DATA_F(STATE, st, NOINIT);

static struct k_thread tx_thread ESP_SDR_DATA_F(STATE, tx_thread, NOINIT);

/* `st` is in group 3 (place.h), where nothing is zeroed at boot: before the kernel starts any thread */
static int st_zero(void)
{
	memset(&st, 0, sizeof(st));
	/* not created yet: tx_stream_stacks() and the thread listings read stack_info */
	memset(&tx_thread, 0, sizeof(tx_thread));
	return 0;
}
SYS_INIT(st_zero, POST_KERNEL, 0);
/* the copy for CPU 0's readers (about 600 B): in group 3, group 0 is full; start and the end of a run write it whole */
static struct tx_stats pub_stats ESP_SDR_DATA_F(SCRATCH, pub_stats, NOINIT);

static K_THREAD_STACK_DEFINE(tx_stack, 3072);
static struct k_sem ctl_wake ESP_SDR_DATA_F(SCRATCH, ctl_wake, NOINIT);
static struct k_sem ctl_ack ESP_SDR_DATA_F(SCRATCH, ctl_ack, NOINIT);
static k_tid_t ctl_tid;
static atomic_t ctl_ready;
static atomic_t ctl_req;
static const struct esp_sdr_replay_cfg *ctl_rc;
static int ctl_ret;
static const struct device *const dm = DEVICE_DT_GET(DT_CHILD(DT_NODELABEL(timer1), counter));
/* calibration source: 8+8 bit pattern, 2 mod 4 start used, and the reference output */
static uint8_t cal_src[4U * CAL_N + 64U] __aligned(4) ESP_SDR_DATA_F(SCRATCH, cal_src, NOINIT);

static ALWAYS_INLINE uint32_t cyc(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, cycle" : "=r"(v));
	return v;
}

static ALWAYS_INLINE uint32_t *word_ptr(uint64_t idx)
{
	return (uint32_t *)(txs_group_of(idx) != 0U ? TX_GROUP1 : TX_GROUP2) +
	       (uint32_t)(idx & (TXS_GROUP_WORDS - 1U));
}

static ALWAYS_INLINE uint32_t own_val(uint32_t group)
{
	return group != 0U ? OWN_G1 : OWN_G2;
}

/* The engine off: RUN cleared, then the register to zero. Idempotent, from any context. */
static ALWAYS_INLINE void tx_engine_off(void)
{
	sys_write32(sys_read32(DAC_TRIG) & ~TRIG_RUN, DAC_TRIG);
	sys_write32(0, DAC_TRIG);
	__asm__ volatile("fence" ::: "memory");
}

/* ---- producer side (CPU 0) ---- */

/* Per packet, from the receive interrupt: IRAM (place.h). The count of producers inside lets the end of
 * a run wait for the one that passed the state check before it flushes the ring (tx_fin()). */
ESP_SDR_CODE(EVENT, tx_stream_put) bool tx_stream_put(struct net_buf *buf, uint16_t off, uint16_t len)
{
	bool ok = false;

	atomic_inc(&st.putters);
	/* the stream holds TX_HOLD at most (the caller counted this one): the rest of the pool is the stack's */
	if (atomic_get(&st.state) == S_RUN && atomic_get(&st.inflight) <= (atomic_val_t)TX_HOLD) {
		const atomic_val_t h = atomic_get(&st.head), t = atomic_get(&st.tail);

		if (h - t < (atomic_val_t)RING) {
			st.ring[h % RING] = (struct ring_item){.buf = buf, .off = off, .len = len};
			atomic_set(&st.head, h + 1);
			ok = true;
		}
	}
	atomic_dec(&st.putters);
	return ok;
}

ESP_SDR_CODE(EVENT, tx_stream_inflight_add) void tx_stream_inflight_add(int d)
{
	atomic_add(&st.inflight, d);
}

uint32_t tx_stream_flips(void)
{
	return (uint32_t)atomic_get(&st.st_flips);
}

uint32_t tx_stream_flip_ts(void)
{
	return (uint32_t)atomic_get(&st.st_flip_ts);
}

bool tx_stream_ready(void)
{
	return atomic_get(&st.ready) != 0 && atomic_get(&st.state) == S_RUN;
}

bool tx_stream_status_raw(uint8_t out[TXS_STATUS_BYTES])
{
	/* nothing before the thread has published its first record: not an all zero one */
	if (atomic_get(&st.snap_ok) == 0) {
		return false;
	}
	/* the writer on CPU 1 is inside its 16 stores for a few hundred cycles each 90 us: wait it out (a few
	 * tries are not enough: 1 to 2 % of the reads failed that way) */
	const uint32_t t0 = cyc();

	do {
		if (txs_snap_read_raw(&st.snap, out)) {
			return true;
		}
	} while ((uint32_t)(cyc() - t0) < US(10));
	return false;
}

bool tx_stream_status(struct txs_status *s)
{
	for (unsigned int i = 0; i < 8U; i++) {
		if (txs_snap_read(&st.snap, s)) {
			return true;
		}
	}
	return false;
}

const struct txs_wire_cfg *tx_stream_wire(void)
{
	return &st.wcfg;
}

/* ---- the flip thread ---- */

static ESP_SDR_CODE(PACED_CHUNK, tx_release) void tx_release(struct net_buf *b)
{
	net_buf_unref(b);
	atomic_dec(&st.inflight);
}

/* One packet from the ring, parsed, to the pending list. */
static ESP_SDR_CODE(PACED_CHUNK, tx_pull) __noinline void tx_pull(void)
{
	struct ring_item it;
	struct txs_wire_pkt w;
	enum txs_wire_err err;
	const atomic_val_t t = atomic_get(&st.tail);

	if (st.npend >= PEND || t == atomic_get(&st.head)) {
		return;
	}
	it = st.ring[t % RING];
	err = txs_wire_parse(&st.wcfg, TX_PORT, (const uint8_t *)it.buf->data + it.off, it.len, &w);
	/* Abort safe (the deadman's abort can stop this thread at any instruction): a buffer is in the
	 * ring, or in both ring and list for the instructions between, or in neither only just before a
	 * release; tx_flush() releases each buffer once and a buffer in both only once.
	 */
	if (err != TXS_WIRE_OK) {
		atomic_set(&st.tail, t + 1);
		st.ls.pk_err[MIN((unsigned int)err, TX_ERR_N - 1U)]++;
		tx_release(it.buf);
		return;
	}
	st.pend[st.npend] = (struct pend){.buf = it.buf, .src = w.payload, .idx = w.index, .n = w.words,
					  .t_taken = cyc()};
	st.npend++;
	atomic_set(&st.tail, t + 1);
	st.ls.pk_taken++;
}

/* Out of the list first, then released: an abort in between leaks one buffer, never releases one
 * that is still listed (tx_flush() would release it again).
 */
static ESP_SDR_CODE(PACED_CHUNK, tx_drop) void tx_drop(uint32_t i)
{
	struct net_buf *b = st.pend[i].buf;

	for (uint32_t k = i; k + 1U < st.npend; k++) {
		st.pend[k] = st.pend[k + 1U];
	}
	st.npend--;
	st.ls.pk_done++;
	tx_release(b);
}

/* The first packet that is not waiting gets a step: up to a chunk, as far as the clock allows (timed;
 * before the engine starts there is no clock: up to a chunk). One copy, called from the loop and from
 * the start (tx_prestart()). */
static ESP_SDR_CODE(PACED_CHUNK, tx_fill_step) __noinline void tx_fill_step(const struct txs_kern *k,
									    bool timed)
{
	const uint32_t bps = st.cfg.fmt == TXS_FMT_IQ8 ? 2U : 4U;
	struct txs_place pl;
	uint32_t i = 0U, used, step, t_a, t_b;
	struct pend *pk;
	uint64_t end;

	while (i < st.npend && st.pend[i].wait) {
		i++;
	}
	if (i == st.npend) {
		return;
	}
	pk = &st.pend[i];
	t_a = cyc();
	end = timed ? txs_flip_fit(&st.f, &st.p, st.j.cursor, pk->idx, pk->idx + MIN(pk->n, CHUNK))
		    : pk->idx + MIN(pk->n, CHUNK);
	t_b = cyc();
	st.ls.c_fit += t_b - t_a;
	if (end <= pk->idx) {
		return;
	}
	step = (uint32_t)(end - pk->idx);
	txs_jit_packet(&st.j, pk->idx, step, &pl);
	st.ls.c_jit += cyc() - t_b;
	used = pl.used;
	switch (pl.verdict) {
	case TXS_PK_PLACE:
		if (!st.first_seen) {
			/* the stream starts at a multiple of 351: the words before it are not a loss */
			st.first_seen = true;
			st.ls.align_gap = pl.zero.hi - pl.zero.lo;
		}
		if (pl.zero.hi > pl.zero.lo) {
			const uint32_t tz = cyc();

			k->zero(word_ptr(pl.zero.lo), (uint32_t)(pl.zero.hi - pl.zero.lo));
			st.ls.zero_words += pl.zero.hi - pl.zero.lo;
			st.ls.c_zero += cyc() - tz;
		}
		if (pl.put.hi > pl.put.lo) {
			const uint32_t words = (uint32_t)(pl.put.hi - pl.put.lo);
			const uint8_t *src = pk->src + (size_t)(pl.put.lo - pk->idx) * bps;
			const uint32_t t0 = cyc();
			uint32_t c;

			if (st.cfg.fmt == TXS_FMT_IQ8) {
				k->iq8(word_ptr(pl.put.lo), src, words);
			} else {
				k->word32(word_ptr(pl.put.lo), src, words);
			}
			c = cyc() - t0;
			st.ls.kern_cyc += c;
			st.ls.kern_words += words;
			st.ls.worst_kern_call = MAX(st.ls.worst_kern_call, c);
		}
		break;
	case TXS_PK_WAIT:
		pk->wait = true;
		return;
	case TXS_PK_DUP:
	case TXS_PK_LATE:
		if (used == 0U) {
			used = step;
		}
		break;
	default:
		/* FAR or BAD: the whole packet goes */
		used = pk->n;
		break;
	}
	pk->idx += used;
	pk->src += (size_t)used * bps;
	pk->n -= MIN(used, pk->n);
	if (pk->n == 0U) {
		tx_drop(i);
	} else if (used < step) {
		/* the rest of the step lies in the next window */
		pk->wait = true;
	}
}

/* The credit record for the sender: small, every depth / rate / 4. */
static ESP_SDR_CODE(PACED_CHUNK, tx_status) void tx_status(void)
{
	txs_status_make(&st.ls.status, st.wcfg.stream_id, st.p.idx, &st.j, &st.f, st.depth);
	txs_snap_write(&st.snap, &st.ls.status);
	atomic_set(&st.snap_ok, 1);
	/* the record that follows a flip: the sender's credit rose with it */
	if (st.f.flips != st.st_flips_seen) {
		st.st_flips_seen = st.f.flips;
		atomic_set(&st.st_flip_ts, (atomic_val_t)k_cycle_get_32());
		atomic_inc(&st.st_flips);
	}
}

/* The whole statistics for the shell, about once a millisecond. */
static ESP_SDR_CODE(PACED_CHUNK, tx_publish) void tx_publish(void)
{
	{
		struct tx_stats *l = &st.ls;

		l->flips = st.f.flips;
		l->late_flips = st.f.late_flips;
		l->slack_flips = st.f.slack_flips;
		l->early_flips = st.f.early_flips;
		l->missed_flips = st.f.missed_flips;
		l->stale_words = st.f.stale_words;
		l->late_worst = st.f.late_worst;
		l->early_worst = st.f.early_worst;
		l->reason = (int)st.f.reason;
		l->placed = st.j.placed;
		l->lost = st.j.lost;
		l->recovered = st.j.recovered;
		l->dup = st.j.dup;
		l->late = st.j.late;
		l->far = st.j.far;
		l->zeroed = st.j.zeroed;
		l->unfilled = st.j.unfilled;
		l->underruns = st.j.underruns;
	}
	{
		const atomic_val_t q = atomic_get(&st.pub_seq);

		atomic_set(&st.pub_seq, q + 1);
		memcpy(&pub_stats, &st.ls, sizeof(pub_stats));
		atomic_set(&st.pub_seq, q + 2);
	}
}

/* A wrong word (flash: it should not happen): what it looks like, and where. */
static __noinline void tx_verify_note(uint64_t idx, uint32_t got, uint32_t want)
{
	const uint64_t off = idx - st.j.lo;

	st.ls.ver_bad++;
	st.ls.ver_pos[MIN((uint32_t)(off >> 9), 7U)]++;
	if (got == tx_tone_word(idx - 2U * TXS_GROUP_WORDS, st.cfg.fmt)) {
		st.ls.ver_stale++;
	}
	if (st.ls.ver_nex < ARRAY_SIZE(st.ls.ver_ex)) {
		uint32_t *e = st.ls.ver_ex[st.ls.ver_nex++];

		e[0] = (uint32_t)idx;
		e[1] = (uint32_t)off;
		e[2] = got;
		e[3] = want;
	}
}

/* The window words just written, read back against the tone: whatever is not zero must be right. Only in
 * the turns the poll has nothing else for (the window is full) and the ring is empty: a packet waits for one
 * step of VER_N words at most. */
static ESP_SDR_CODE(PACED_CHUNK, tx_verify_step) void tx_verify_step(uint64_t *vpos)
{
	/* only what the cursor has written or zeroed */
	const uint64_t hi = MIN(st.j.hi, st.j.cursor);
	uint32_t n = (uint32_t)MIN((uint64_t)VER_N, hi - *vpos);
	const uint32_t *w = word_ptr(*vpos);

	for (uint32_t i = 0; i < n; i++) {
		const uint32_t got = w[i];

		if (got == 0U) {
			st.ls.ver_zero++;
		} else {
			const uint32_t want = tx_tone_word(*vpos + i, st.cfg.fmt);

			if (got != want) {
				tx_verify_note(*vpos + i, got, want);
			}
		}
	}
	st.ls.ver_words += n;
	*vpos += n;
}

/* Lock, spin to the due time less the write latency, write the ownership, account. */
static ESP_SDR_CODE(PACED_CHUNK, tx_flip) void tx_flip(void)
{
	const uint32_t group = txs_group_of(st.f.next);
	const uint32_t due = txs_pos_counter_at(&st.p, st.f.next);
	const uint32_t aim = txs_flip_aim(&st.f, &st.p);
	unsigned int key;
	uint32_t tw, te, play_ctr, wr;
	uint64_t play;

	const uint32_t t_entry = cyc();
	const uint32_t open_at = aim - US(LOCK_US);
	uint32_t t_open, t_lock;

	/* open until the last stretch, then locked to the write */
	while ((int32_t)(cyc() - open_at) < 0) {
	}
	t_open = cyc();
	key = arch_irq_lock();
	t_lock = cyc();
	while ((int32_t)(cyc() - aim) < 0) {
	}
	tw = cyc();
	if (atomic_get(&st.dead) == 0) {
		sys_write32(own_val(group), OWN_REG);
		__asm__ volatile("fence" ::: "memory");
	}
	te = cyc();
	play_ctr = tw + st.f.cfg.write_lat;
	txs_pos_update(&st.p, play_ctr);
	play = txs_pos_word_after(&st.p);
	txs_flip_done(&st.f, play, st.j.cursor, st.j.win_data);
	if (st.f.dac_group != group && atomic_get(&st.dead) == 0) {
		sys_write32(own_val(st.f.dac_group), OWN_REG);
		__asm__ volatile("fence" ::: "memory");
	}
	arch_irq_unlock(key);
	wr = te - tw;
	st.ls.write_min = st.ls.write_min == 0U ? wr : MIN(st.ls.write_min, wr);
	st.ls.write_max = MAX(st.ls.write_max, wr);
	st.ls.write_sum += wr;
	st.ls.write_n++;
	{
		const uint32_t over = (uint32_t)MAX((int32_t)(tw + st.f.cfg.write_lat - due), 0);

		st.ls.spin_over64 += over > 64U;
		if (over > st.ls.spin_worst) {
			st.ls.spin_worst = over;
			st.ls.ev = (struct tx_flip_ev){
				.flip = st.f.flips, .ms = (uint32_t)(k_uptime_get() - st.t_start_ms), .late = over,
				.entry = (int32_t)(t_entry - open_at), .open = (int32_t)(t_open - open_at),
				.lock = (int32_t)(t_lock - aim), .prev_kind = st.last_kind,
				.prev_cyc = st.last_cyc, .prev_ago = t_entry - st.last_end,
			};
		}
	}
	txs_jit_flip(&st.j, &st.f);
	for (uint32_t i = 0; i < st.npend; i++) {
		st.pend[i].wait = false;
	}
	atomic_set(&st.flips_pub, (atomic_val_t)st.f.flips);
}

/* Calibrate the kernel (golden check, cycles a word) before the engine starts. Scratch: group 1. */
static ESP_SDR_CODE(PACED_CHUNK, tx_calibrate) __noinline int tx_calibrate(const struct txs_kern *k,
									  uint32_t *cost, uint32_t *zcost)
{
	uint32_t *dst = (uint32_t *)TX_GROUP1, *ref = dst + 2U * CAL_N;
	uint32_t worst = 0U, zworst = 0U;
	static const uint32_t ns[] = {CAL_N, CAL_N + 7U, 1U, 2U, 15U, 16U, 17U};

	for (uint32_t i = 0; i < sizeof(cal_src); i++) {
		cal_src[i] = (uint8_t)(i * 37U + (i >> 3) + 11U);
	}
	for (uint32_t a = 0; a < 2U; a++) {
		const uint8_t *src = cal_src + 2U * a;

		for (uint32_t t = 0; t < ARRAY_SIZE(ns); t++) {
			const uint32_t n = ns[t];

			memset(dst, 0xa5, 4U * 2U * CAL_N);
			memset(ref, 0xa5, 4U * 2U * CAL_N);
			if (st.cfg.fmt == TXS_FMT_IQ8) {
				k->iq8(dst, src, n);
				txs_kern_ref.iq8(ref, src, n);
			} else {
				k->word32(dst, src, n);
				txs_kern_ref.word32(ref, src, n);
			}
			if (memcmp(dst, ref, 4U * 2U * CAL_N) != 0) {
				return -EILSEQ;
			}
		}
		for (uint32_t r = 0; r < CAL_REPS + 1U; r++) {
			const uint32_t t0 = cyc();
			uint32_t c;

			if (st.cfg.fmt == TXS_FMT_IQ8) {
				k->iq8(dst, src, CAL_N);
			} else {
				k->word32(dst, src, CAL_N);
			}
			c = cyc() - t0;
			if (r != 0U) {
				worst = MAX(worst, c);
			}
		}
	}
	for (uint32_t r = 0; r < CAL_REPS + 1U; r++) {
		const uint32_t t0 = cyc();
		uint32_t c;

		k->zero(dst, CAL_N);
		c = cyc() - t0;
		if (r != 0U) {
			zworst = MAX(zworst, c);
		}
	}
	/* one and a half times the worst call, a word's share rounded up */
	*cost = (worst * 3U / 2U + CAL_N - 1U) / CAL_N;
	*zcost = (zworst * 3U / 2U + CAL_N - 1U) / CAL_N;
	*zcost = MAX(*zcost, 1U);
	*cost = MAX(*cost, *zcost + 1U);
	memset(dst, 0, 4U * 4U * CAL_N);
	return 0;
}

/* The ownership write as this core sees it, before the engine runs: the value written is the one
 * there, so nothing changes. The mean is what the flip aims by (the effect comes about that long after
 * the write starts).
 */
static ESP_SDR_CODE(PACED_CHUNK, tx_cal_write) __noinline void tx_cal_write(void)
{
	const uint32_t v = sys_read32(OWN_REG);
	const unsigned int key = arch_irq_lock();
	uint32_t sum = 0U, mn = UINT32_MAX, mx = 0U;

	for (uint32_t i = 0; i < 64U; i++) {
		const uint32_t t = cyc();
		uint32_t c;

		sys_write32(v, OWN_REG);
		__asm__ volatile("fence" ::: "memory");
		c = cyc() - t;
		sum += c;
		mn = MIN(mn, c);
		mx = MAX(mx, c);
	}
	arch_irq_unlock(key);
	st.ls.cal_min = mn;
	st.ls.cal_mean = (sum + 32U) / 64U;
	st.ls.cal_max = mx;
}

static ESP_SDR_CODE(PACED_CHUNK, tx_have_work) __noinline bool tx_have_work(void)
{
	for (uint32_t i = 0; i < st.npend; i++) {
		if (!st.pend[i].wait) {
			return true;
		}
	}
	return false;
}

static ESP_SDR_CODE(PACED_CHUNK, tx_ring_waiting) __noinline bool tx_ring_waiting(void)
{
	return st.npend < PEND && atomic_get(&st.tail) != atomic_get(&st.head);
}

/* A window closes short: what was there? Packet words waiting for it (the fill was too slow: the
 * oldest one's age in cycles), a packet in the ring not taken yet, or nothing (lost, or not sent).
 */
static ESP_SDR_CODE(PACED_CHUNK, tx_classify_close) void tx_classify_close(void)
{
	const uint64_t cur = st.j.cursor, hi = st.j.hi;
	uint32_t age = 0U;
	bool pend = false;

	if (st.f.mode != TXS_RUN || !st.j.started || st.j.closed || cur >= hi) {
		return;
	}
	for (uint32_t i = 0; i < st.npend; i++) {
		const struct pend *p = &st.pend[i];

		if (p->n != 0U && p->idx < hi && p->idx + p->n > cur) {
			pend = true;
			age = MAX(age, cyc() - p->t_taken);
		}
	}
	if (pend) {
		st.ls.ur_pend++;
		st.ls.ur_age = MAX(st.ls.ur_age, age);
	} else if (atomic_get(&st.tail) != atomic_get(&st.head)) {
		st.ls.ur_ring++;
	} else {
		st.ls.ur_none++;
	}
}

/* TX_SRC_NET, before the engine starts: the host fills the first window up to the credit (its end) first,
 * and the engine does not start until that window is complete or PRE_FILL_US after the first packet
 * was taken. No clock to meet yet: the steps are untimed. Statuses go out as in the loop, the play
 * index is the window's first word. In flash: nothing here is on a deadline. */
static __noinline int tx_prestart(const struct txs_kern *k)
{
	const uint32_t begin = cyc();
	uint32_t t_first = 0U, next_st = begin;
	bool first = false;

	for (;;) {
		const uint32_t now = cyc();

		if (atomic_get(&st.dead) != 0 || atomic_get(&st.abort_pre) != 0) {
			return -ECANCELED;
		}
		if ((int32_t)(now - next_st) >= 0) {
			txs_status_make(&st.ls.status, st.wcfg.stream_id, st.f.next, &st.j, &st.f, st.depth);
			txs_snap_write(&st.snap, &st.ls.status);
			atomic_set(&st.snap_ok, 1);
			next_st = now + US(PRE_ST_US);
		}
		if (st.j.cursor >= st.j.hi) {
			return 0;
		}
		if (first ? (int32_t)(now - t_first) >= (int32_t)US(PRE_FILL_US) :
			    (int32_t)(now - begin) >= (int32_t)US(PRE_WAIT_MS * 1000U)) {
			return first ? 0 : -ETIMEDOUT;
		}
		if (tx_have_work()) {
			tx_fill_step(k, false);
		} else if (tx_ring_waiting()) {
			tx_pull();
			if (!first && st.ls.pk_taken != 0U) {
				first = true;
				t_first = cyc();
			}
		}
	}
}

static ESP_SDR_CODE(PACED_CHUNK, tx_loop) __noinline void tx_loop(void)
{
	const uint32_t cpw = ESP_SDR_CPU_HZ / (st.cfg.msps * 1000000U);
	const uint32_t ctrl = TRIG_RUN | TRIG_LOOP | TXS_GROUP_WORDS;
	const uint32_t st_cyc = st.depth * cpw / 4U;
	const struct txs_kern *k = st.cfg.kern;
	struct txs_flip_cfg fc = {.chunk = CHUNK, .overhead = st.cfg.overhead, .write_lat = st.cfg.write_lat,
				  .tol = st.cfg.tol, .hang_passes = HANG_PASSES};
	uint32_t t0, cost, zcost, sys_prev, cpu_prev, next_pub, next_st;
	uint64_t vpos = 0U;
	bool stopping = false;
	int ret;

	st.ls.cpu_start = _current_cpu->id;
	st.ls.pass_cyc = TXS_GROUP_WORDS * cpw;
#if defined(CONFIG_ESP_SDR_TXS_PIE)
	if (k == &txs_kern_pie) {
		(void)txs_pie_enable();
	}
#endif
	ret = tx_calibrate(k, &cost, &zcost);
	if (ret != 0 && st.cfg.fallback != NULL && st.cfg.fallback != k) {
		/* the first kernel did not match the reference: the fallback runs instead (tx stat says which) */
		k = st.cfg.fallback;
		st.ls.kern = k->name;
		st.ls.kern_fb = true;
		ret = tx_calibrate(k, &cost, &zcost);
	}
	if (ret != 0) {
		st.ls.ret = ret;
		return;
	}
	if (fc.write_lat == 0U) {
		tx_cal_write();
		fc.write_lat = st.ls.cal_mean;
		st.ls.lat_auto = true;
	}
	fc.cost = cost;
	fc.zcost = zcost;
	fc.lock_lead = CHUNK * cost + fc.overhead + US(LOCK_US);
	fc.margin = fc.lock_lead + CHUNK * zcost + fc.overhead + US(1);
	st.ls.cost = cost;
	st.ls.zcost = zcost;
	st.ls.margin = fc.margin;
	st.ls.lock_lead = fc.lock_lead;
	st.ls.chunk = CHUNK;
	st.ls.overhead = fc.overhead;
	st.ls.write_lat = fc.write_lat;
	st.ls.tol = fc.tol;
	st.ls.depth = st.depth;
	if (txs_flip_init(&st.f, &fc, TXS_GROUP_WORDS) != 0 ||
	    txs_jit_init(&st.j, &st.f, st.depth, st.depth) != 0) {
		st.ls.ret = -EINVAL;
		return;
	}
	if (st.cfg.src == TX_SRC_NET) {
		ret = tx_prestart(k);
		if (ret != 0) {
			st.ls.ret = ret;
			return;
		}
	}
	if (st.cfg.rf) {
		/* The library's replay start has run (chain on, engine playing group 2 from that moment):
		 * restart the index here, on this core, to know its time.
		 */
		const unsigned int key = arch_irq_lock();

		sys_write32(0, DAC_TRIG);
		t0 = cyc();
		sys_write32(ctrl, DAC_TRIG);
		arch_irq_unlock(key);
		ret = 0;
	} else {
		ret = esp_sdr_s31_dac_start(st.cfg.msps == 40U ? RATE_40 : RATE_20, ctrl, &t0);
	}
	if (ret != 0) {
		st.ls.ret = ret;
		return;
	}
	txs_pos_init(&st.p, t0, cpw, 0U);
	sys_prev = k_cycle_get_32();
	cpu_prev = t0;
	next_pub = next_st = t0;
	atomic_set(&st.ready, 1);
	for (;;) {
		const uint32_t now = cyc();
		enum txs_op op;
		uint32_t kind;
		bool published = false;

		if (atomic_get(&st.hang) != 0) {
			if (atomic_get(&st.hang) == 2) {
				/* test: spin with this core's interrupts off, as a stuck flip would (no RF) */
				(void)arch_irq_lock();
			}
			for (;;) {
				k_busy_wait(100);
			}
		}
		if (atomic_get(&st.dead) != 0) {
			return;
		}
		txs_pos_update(&st.p, now);
		if ((int32_t)(now - next_st) >= 0) {
			tx_status();
			next_st = now + st_cyc;
			st.ls.c_pub += cyc() - now;
			published = true;
		}
		if ((int32_t)(now - next_pub) >= 0) {
			const uint32_t sys = k_cycle_get_32();

			st.ls.cpu_cyc += now - cpu_prev;
			st.ls.sys_cyc += sys - sys_prev;
			cpu_prev = now;
			sys_prev = sys;
			st.ls.elapsed_ms = (uint32_t)(k_uptime_get() - st.t_start_ms);
			tx_publish();
			next_pub = now + US(1000);
			published = true;
		}
		if (published) {
			/* the publishes took time: the poll must not run on the position from before them */
			txs_pos_update(&st.p, cyc());
		}
		if (!stopping && (st.f.flips >= st.total_flips || atomic_get(&st.abort_pre) != 0)) {
			txs_flip_stop(&st.f, TXS_STOP_DRAIN);
			stopping = true;
		}
		{
			const uint32_t tp = cyc();

			op = txs_flip_poll(&st.f, &st.p, st.j.cursor);
			st.ls.c_poll += cyc() - tp;
		}
		st.ls.loops++;
		kind = 4U;
		/* One thing a turn: a step or a pull, so that the longest of them bounds the lock lead */
		switch (op) {
		case TXS_OP_FILL: {
			const uint32_t t = cyc();

			if (tx_have_work()) {
				uint32_t c;

				tx_fill_step(k, true);
				c = cyc() - t;
				st.ls.fill_cyc += c;
				st.ls.steps++;
				st.ls.worst_step = MAX(st.ls.worst_step, c);
				kind = 1U;
			} else {
				tx_pull();
				st.ls.c_pull += cyc() - t;
				kind = 5U;
			}
			break;
		}
		case TXS_OP_ZERO: {
			struct txs_span z;
			const uint32_t tz = cyc();

			tx_classify_close();
			txs_jit_close(&st.j, st.f.mode == TXS_DRAIN, &z);
			if (z.hi > z.lo) {
				k->zero(word_ptr(z.lo), (uint32_t)(z.hi - z.lo));
				st.ls.zero_words += z.hi - z.lo;
			}
			st.ls.c_zero += cyc() - tz;
			kind = 2U;
			break;
		}
		case TXS_OP_IDLE: {
			const uint32_t tq = cyc();

			if (tx_ring_waiting()) {
				tx_pull();
				st.ls.c_pull += cyc() - tq;
				kind = 5U;
			} else if (st.cfg.verify && !(st.f.mode == TXS_DRAIN && st.f.drain_left == 0U)) {
				/* not the final drain window: the poll leaves it unfilled (it is never played) */
				if (vpos < st.j.lo || vpos > st.j.hi) {
					vpos = st.j.lo;
				}
				if (vpos < MIN(st.j.hi, st.j.cursor)) {
					tx_verify_step(&vpos);
					kind = 3U;
				}
			}
			break;
		}
		case TXS_OP_FLIP:
			tx_flip();
			vpos = st.j.lo;
			kind = 6U;
			/* the record that follows is the sender's new credit: publish it at once */
			next_st = now;
			break;
		case TXS_OP_OFF:
			goto out;
		}
		st.last_kind = kind | (published ? 0x100U : 0U);
		st.last_end = cyc();
		st.last_cyc = st.last_end - now;
	}
out:
	/* the engine off here, for every reason, before anything else */
	tx_engine_off();
	st.ls.reason = (int)st.f.reason;
	while (st.npend != 0U) {
		tx_drop(st.npend - 1U);
	}
	{
		const uint32_t sys = k_cycle_get_32();

		st.ls.cpu_cyc += cyc() - cpu_prev;
		st.ls.sys_cyc += sys - sys_prev;
	}
	tx_publish();
}

static void ctl_request(atomic_val_t bits)
{
	atomic_or(&ctl_req, bits);
	k_sem_give(&ctl_wake);
}

static void tx_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	tx_loop();
	/* the engine off for every way out, also an early failure */
	tx_engine_off();
	/* whoever takes the run from S_RUN ends it: the deadman may have been first */
	if (atomic_cas(&st.state, S_RUN, S_ENDING)) {
		st.ls.cpu_end = _current_cpu->id;
		st.ls.thread_ended = true;
		tx_publish();
		atomic_set(&st.state, S_FIN);
		ctl_request(CTL_FIN);
	}
}

/* Engine off from the interrupt: the groups go back to the CPU. */
static ESP_SDR_CODE(PACED_EVENT, tx_deadman) void tx_deadman(const struct device *dev, void *user)
{
	const uint32_t n = (uint32_t)atomic_get(&st.flips_pub);

	ARG_UNUSED(dev);
	ARG_UNUSED(user);
	if (atomic_get(&st.state) != S_RUN) {
		return;
	}
	if (n != st.dead_seen) {
		st.dead_seen = n;
		st.dead_idle = 0U;
		return;
	}
	if (atomic_get(&st.ready) == 0) {
		if (++st.dead_pre < st.dead_pre_max) {
			return;
		}
	} else if (++st.dead_idle < DEAD_HALVES) {
		return;
	}
	/* a thread that just ended on its own has the run (S_FIN): nothing to do */
	if (!atomic_cas(&st.state, S_RUN, S_ENDING)) {
		return;
	}
	atomic_set(&st.dead, 1);
	tx_engine_off();
	sys_write32(0, OWN_REG);
	__asm__ volatile("fence" ::: "memory");
	atomic_set(&st.state, S_FIN);
	ctl_request(CTL_FIN);
}

/* static, in group 3: the 600 B are not group 0's */
static struct tx_stats fin ESP_SDR_DATA_F(SCRATCH, fin, NOINIT);

static void tx_chain_off(void)
{
	if (st.cfg.rf) {
		esp_sdr_replay_stop();
	} else {
		esp_sdr_s31_dac_stop();
	}
}

/* The statistics for CPU 0's readers, under the sequence lock (the writer is this thread, or the flip
 * thread while it runs): an abort of the flip thread in a publish leaves the count odd, so it is
 * taken over here.
 */
static void pub_store(const struct tx_stats *s)
{
	const atomic_val_t q = atomic_get(&st.pub_seq) & ~(atomic_val_t)1;

	atomic_set(&st.pub_seq, q + 1);
	memcpy(&pub_stats, s, sizeof(pub_stats));
	atomic_set(&st.pub_seq, q + 2);
}

/* Every buffer the stream still holds goes back, each once. A buffer may be on the ring and the list
 * both (the flip thread was stopped between the two steps of a hand over). */
static void tx_flush(void)
{
	struct net_buf *seen[RING + PEND];
	const atomic_val_t h = atomic_get(&st.head);
	uint32_t n = 0U;

	for (atomic_val_t t = atomic_get(&st.tail); t != h && n < RING; t++) {
		seen[n++] = st.ring[t % RING].buf;
	}
	for (uint32_t i = 0; i < st.npend; i++) {
		bool dup = false;

		for (uint32_t k = 0; k < n; k++) {
			dup = dup || seen[k] == st.pend[i].buf;
		}
		if (!dup && n < ARRAY_SIZE(seen)) {
			seen[n++] = st.pend[i].buf;
		}
	}
	for (uint32_t k = 0; k < n; k++) {
		tx_release(seen[k]);
		st.pk_flushed++;
	}
	st.npend = 0U;
	atomic_set(&st.tail, h);
}

/* The final statistics: into `fin`, the shared copy, and to the report callback. */
static void tx_report(bool dead, bool call)
{
	tx_stream_stats(&fin);
	fin.dead = dead;
	fin.pk_flushed = st.pk_flushed;
	fin.inflight_end = (uint32_t)atomic_get(&st.inflight);
	fin.active = false;
	fin.elapsed_ms = (uint32_t)(k_uptime_get() - st.t_start_ms);
	fin.sys_hz = sys_clock_hw_cycles_per_sec();
	pub_store(&fin);
	if (call && st.done != NULL) {
		st.done(&fin);
	}
}

/* The end of a run, on the control thread. Order: engine and chain off first (the deadman stays armed
 * until then), then the deadman's timer, the injector, the flip thread. */
static void tx_fin(void)
{
	const bool dead = atomic_get(&st.dead) != 0;

	if (atomic_get(&st.state) != S_FIN) {
		return;
	}
	tx_engine_off();
	tx_chain_off();
	(void)counter_stop(dm);
	tx_inject_stop();
	/* a producer that passed the state check inside tx_stream_put() is done before the ring is flushed */
	for (unsigned int i = 0; i < 1000U && atomic_get(&st.putters) != 0; i++) {
		k_busy_wait(1);
	}
	if (dead) {
		/* The flip thread may spin with interrupts locked: its abort waits for it and may not
		 * return. The report comes first; the state stays FIN (no new start) until it did.
		 */
		tx_report(true, true);
		k_thread_abort(&tx_thread);
		tx_flush();
		tx_report(true, false);
	} else {
		(void)k_thread_join(&tx_thread, K_MSEC(STOP_JOIN_MS));
		tx_flush();
		tx_report(false, true);
	}
	atomic_set(&st.state, S_IDLE);
}

/* NOINIT: before the first run the shell would print what a reset left there (a kern pointer) */
static int tx_stream_early_init(void)
{
	memset(&pub_stats, 0, sizeof(pub_stats));
	memset(&fin, 0, sizeof(fin));
	return 0;
}

SYS_INIT(tx_stream_early_init, POST_KERNEL, 50);

void tx_stream_ctl_run(void)
{
	k_sem_init(&ctl_wake, 0, K_SEM_MAX_LIMIT);
	k_sem_init(&ctl_ack, 0, K_SEM_MAX_LIMIT);
	/* NOINIT: before the first run the shell would print what a reset left there (a kern pointer) */
	memset(&pub_stats, 0, sizeof(pub_stats));
	ctl_tid = k_current_get();
	atomic_set(&ctl_ready, 1);
	for (;;) {
		atomic_val_t r;

		k_sem_take(&ctl_wake, K_FOREVER);
		r = atomic_set(&ctl_req, 0);
		if ((r & CTL_START) != 0) {
			ctl_ret = esp_sdr_replay_start(ctl_rc);
			k_sem_give(&ctl_ack);
		}
		if ((r & CTL_OFF) != 0) {
			tx_chain_off();
			k_sem_give(&ctl_ack);
		}
		if ((r & CTL_FIN) != 0) {
			tx_fin();
		}
	}
}

/* A request the shell thread waits for. */
static int ctl_call(atomic_val_t bit)
{
	ctl_request(bit);
	k_sem_take(&ctl_ack, K_FOREVER);
	return bit == CTL_START ? ctl_ret : 0;
}

int tx_stream_start(const struct tx_cfg *cfg, tx_done_t done)
{
	struct counter_top_cfg top = {.callback = tx_deadman};
	const struct esp_sdr_replay_cfg rc = {
		.rate = cfg->msps == 40U ? ESP_SDR_RATE_40MSPS : ESP_SDR_RATE_20MSPS,
		.count = TXS_GROUP_WORDS, .passes = 0, .power = 0,
	};
	k_tid_t t;
	int ret;
	uint32_t words_pkt, half_us;

	if ((cfg->msps != 20U && cfg->msps != 40U) || cfg->seconds == 0U || cfg->kern == NULL) {
		return -EINVAL;
	}
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	if (!device_is_ready(dm) || arch_num_cpus() < 2U) {
		return -ENODEV;
	}
	if (atomic_get(&ctl_ready) == 0) {
		return -EAGAIN;
	}
	if (!atomic_cas(&st.state, S_IDLE, S_RUN)) {
		return -EBUSY;
	}
	memset(&st.ls, 0, sizeof(st.ls));
	memset(&st.pend, 0, sizeof(st.pend));
	st.npend = 0U;
	st.cfg = *cfg;
	st.done = done;
	atomic_set(&st.dead, 0);
	atomic_set(&st.hang, 0);
	atomic_set(&st.ready, 0);
	atomic_set(&st.flips_pub, 0);
	atomic_set(&st.inflight, 0);
	atomic_set(&st.head, 0);
	atomic_set(&st.tail, 0);
	atomic_set(&st.pub_seq, 0);
	st.dead_idle = st.dead_seen = st.dead_pre = 0U;
	st.ls.msps = cfg->msps;
	st.first_seen = false;
	st.pk_flushed = 0U;
	st.ls.cpu_end = -1;
	st.last_kind = st.last_cyc = st.last_end = 0U;
	st.ls.rf = cfg->rf;
	st.ls.verify = cfg->verify;
	st.ls.fmt = cfg->fmt;
	st.ls.kern = cfg->kern->name;
	st.ls.active = true;
	atomic_set(&st.putters, 0);
	atomic_set(&st.abort_pre, 0);
	atomic_set(&st.st_flips, 0);
	atomic_set(&st.snap_ok, 0);
	st.st_flips_seen = 0U;
	st.wcfg = (struct txs_wire_cfg){.stream_id = cfg->src == TX_SRC_NET ? cfg->stream_id : TX_STREAM_ID,
					.port = (uint16_t)TX_PORT, .fmt = cfg->fmt,
					.max_words = cfg->fmt == TXS_FMT_IQ8 ? 702U : 351U};
	/* depth: the buffers' capacity in words of a packet, less two packets (txs_credit.h) */
	words_pkt = st.wcfg.max_words;
	st.depth = (TX_BUFFERS - 2U) * words_pkt;
	st.total_flips = (uint32_t)((uint64_t)cfg->seconds * cfg->msps * 1000000U / TXS_GROUP_WORDS);
	txs_snap_init(&st.snap);
	memcpy(&pub_stats, &st.ls, sizeof(pub_stats));

	/* both groups zero: the engine drops CPU stores to the one it owns; pass 0 plays zeros */
	memset((void *)TX_GROUP1, 0, 4U * TXS_GROUP_WORDS);
	memset((void *)TX_GROUP2, 0, 4U * TXS_GROUP_WORDS);
	barrier_dmem_fence_full();
	if (cfg->rf) {
		/* on the control thread, which turns the chain off again (the library's lock is its) */
		ctl_rc = &rc;
		ret = ctl_call(CTL_START);
		if (ret != 0) {
			goto fail;
		}
	}
	t = k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack), tx_entry, NULL, NULL,
			    NULL, K_PRIO_COOP(1), 0, K_FOREVER);
	k_thread_name_set(t, "tx_flip");
	ret = k_thread_cpu_pin(t, 1);
	if (ret != 0) {
		k_thread_abort(t);
		goto fail_chain;
	}
	/* a timer tick is half a pass */
	half_us = TXS_GROUP_WORDS / cfg->msps / 2U;
	top.ticks = counter_us_to_ticks(dm, half_us);
	st.dead_pre_max = DEAD_PRE_MS * 1000U / half_us;
	ret = counter_set_top_value(dm, &top);
	if (ret != 0) {
		k_thread_abort(t);
		goto fail_chain;
	}
	/* the injector first: it only waits for the status until the flip thread publishes one, and an
	 * early end of the flip thread then finds it running, not half started */
	ret = cfg->src == TX_SRC_INJECT ? tx_inject_start() : 0;
	if (ret != 0) {
		k_thread_abort(t);
		goto fail_chain;
	}
	ret = counter_start(dm);
	if (ret != 0) {
		tx_inject_stop();
		k_thread_abort(t);
		goto fail_chain;
	}
	st.t_start_ms = k_uptime_get();
	k_thread_start(t);
	return 0;
fail_chain:
	if (cfg->rf) {
		(void)ctl_call(CTL_OFF);
	}
fail:
	atomic_set(&st.state, S_IDLE);
	return ret;
}

int tx_stream_stop(bool now)
{
	if (atomic_get(&st.state) != S_RUN) {
		return -EALREADY;
	}
	if (atomic_get(&st.ready) == 0) {
		if (st.cfg.src == TX_SRC_NET) {
			/* waiting for the first window: nothing plays yet, end the start */
			atomic_set(&st.abort_pre, 1);
			return 0;
		}
		return -EAGAIN;
	}
	txs_flip_request(&st.f, now ? TXS_STOP_NOW : TXS_STOP_DRAIN);
	return 0;
}

int tx_stream_hang(bool lock)
{
	if (atomic_get(&st.state) != S_RUN) {
		return -EALREADY;
	}
	if (lock && st.cfg.rf) {
		return -EPERM;
	}
	atomic_set(&st.hang, lock ? 2 : 1);
	return 0;
}

void tx_stream_stats(struct tx_stats *s)
{
	/* bounded: a flip thread stopped inside a publish leaves the count odd */
	for (unsigned int i = 0; i < 1000U; i++) {
		const atomic_val_t q = atomic_get(&st.pub_seq);

		memcpy(s, &pub_stats, sizeof(*s));
		if ((q & 1) == 0 && q == atomic_get(&st.pub_seq)) {
			break;
		}
	}
	s->active = atomic_get(&st.state) != S_IDLE;
	if (s->active) {
		s->sys_hz = sys_clock_hw_cycles_per_sec();
	}
}

bool tx_stream_active(void)
{
	return atomic_get(&st.state) != S_IDLE;
}

void tx_stream_stacks(size_t *flip_unused, size_t *ctl_unused)
{
	*flip_unused = *ctl_unused = 0U;
	if (ctl_tid != NULL) {
		(void)k_thread_stack_space_get(ctl_tid, ctl_unused);
		if (tx_thread.stack_info.size != 0U) {
			(void)k_thread_stack_space_get(&tx_thread, flip_unused);
		}
	}
}

void *tx_stream_stack(size_t *size)
{
	*size = K_THREAD_STACK_SIZEOF(tx_stack);
	return tx_stack;
}
