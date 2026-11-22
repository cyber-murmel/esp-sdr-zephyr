/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * End to end: a model DAC that plays words at the rate from two groups it owns in turn, a model
 * PC sender (a timeline of words, paced to the rate, credits, jitter, loss, duplicates, stalls),
 * and the module between them, driven the way the CPU1 thread will drive it: poll, fill chunks of
 * packets (wire parse, jitter cursor, expansion kernels), zero, flip. Time is in CPU cycles; the
 * counter the module sees is 32 bit and wraps in the run.
 *
 * Asserted for every run: every word the DAC plays is zero or the word the sender put at that
 * index (the index never shifts), the words played stale are exactly those the scheduler counted,
 * the CPU never writes into the DAC's group, the words are conserved (arrived = placed + dup +
 * late + far + still queued; window words = placed + lost + zeroed + unfilled).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "txs_credit.h"
#include "txs_expand.h"
#include "txs_wire.h"

#define G       TXS_GROUP_WORDS
#define QCAP    34U
/* the queue array: a run may ask for more buffers than the pool has */
#define QMAX    128U
#define NETCAP  600U
#define MINPKT  100U
#define MAXPKT  350U
/* the largest packet a test may use: 1404 B of 8+8 bit samples, 702 words */
#define PKTMAX  702U
#define FRAMEB  (2U + 16U + 4U * PKTMAX + 8U)
#define SESSION 0x5eed1234U
#define PORT    4994U
#define CHUNK   256U
/* the ownership write takes at least WLAT_MIN cycles, up to WLAT_VAR more; the spin exits late by up to SPIN_VAR */
#define WLAT_MIN 40U
#define WLAT_VAR 11U
#define SPIN_VAR 13U

struct frame {
	uint8_t b[FRAMEB] __aligned(4);
	size_t len;
	uint64_t arrive;
};

struct qent {
	struct frame fr;
	struct txs_wire_pkt pk;
	/* what is left: index of the first word, words, and where its bytes are (an offset in fr) */
	uint64_t idx;
	uint32_t n;
	uint32_t srcoff;
	bool wait;
};

struct sim_cfg {
	const char *name;
	uint32_t cpw;
	enum txs_fmt fmt;
	uint64_t idx0;
	uint32_t windows;
	uint32_t seed;
	bool fast_kernels;
	/* network: loss and duplicate per mille, jitter in cycles */
	uint32_t loss_pm, dup_pm, jitter;
	/* sender: lead beyond a group in words, stall chance per 100000 packets, longest in cycles */
	uint32_t lead_extra, stall_pp, stall_max;
	int32_t skew_ppm;
	/* packets of random size (else all MAXPKT words) */
	bool var_pkt;
	/* words of a packet (0: MAXPKT), buffers (0: QCAP), the step the module takes (0: CHUNK), its
	 * lock lead (0: 400), and one stall of the sender at a time of the run (cycles, 0: none) */
	uint32_t pkt, qcap, chunk, lock_lead, status_div;
	/* packets sent in batches of this many, last one first (the injector's reorder) */
	uint32_t reorder;
	uint64_t stall_at, stall_len;
	/* module: preemption chance per million loop turns, longest in cycles */
	uint32_t preempt_pm, preempt_max;
	/* stop: 0 none (a DRAIN after `windows`), else a random time in the run: 1 DRAIN, 2 NOW */
	uint32_t stop;
	/* the module hangs for hang_len cycles from hang_at (0: never) */
	uint64_t hang_at, hang_len;
	/* the sender's speed when it may send, in quarters of the sample rate (0: 8, twice), the data's extra
	 * delay (cycles), the status' delay to the sender (0: 6400) and its period besides the one at each
	 * flip (0: depth / rate / status_div), the module's cycles a word (0: cpw / 2) */
	uint32_t send_q;
	uint32_t net_delay, st_delay, st_period, cost;
	/* the credit depth in words (0: the buffers', (qcap - 2) x the smallest packet), and the first window
	 * sent and in the queue before the engine starts (the board waits for it: prestart) */
	uint32_t depth;
	bool prefill;
};

struct stats {
	uint64_t sent, arrived, flushed, net_lost, rx_drop, queued_left;
	uint64_t nz, zeros, stale, stale_placed, windows_unfilled;
	uint32_t resyncs, credit_blocked, bad_pkts, qmax;
};

static struct {
	const struct sim_cfg *c;
	const struct txs_kern *k;
	struct txs_pos pos;
	struct txs_flip f;
	struct txs_jit j;
	struct stats st;
	uint32_t ctr0;
	uint64_t T;
	/* DAC */
	uint32_t bank[TXS_BANK_WORDS];
	/* index + 1 of the word a slot was last written with: data, and zero */
	uint64_t wrote[TXS_BANK_WORDS], zwrote[TXS_BANK_WORDS];
	uint32_t owner;
	uint64_t dac_idx;
	/* network and queue */
	struct frame net[NETCAP];
	uint32_t nnet;
	struct qent q[QMAX];
	uint32_t nq;
	/* sender */
	uint64_t snext, st_free, stall_until, credit;
	bool stalled;
	uint32_t seen_u;
	uint32_t seq;
	struct { uint64_t arrive; struct txs_status s; } sp[128];
	uint32_t sp_n;
	uint64_t status_due;
	struct txs_wire_cfg wc;
	struct txs_snap snap;
} S;

ZTEST_SUITE(txs_sim, NULL, NULL, NULL, NULL, NULL);

static uint32_t pk_words(void)
{
	return S.c->pkt != 0U ? S.c->pkt : MAXPKT;
}

static uint32_t qcap(void)
{
	return S.c->qcap != 0U ? S.c->qcap : QCAP;
}

static uint32_t hash(uint64_t i)
{
	i ^= i >> 33;
	i *= 0xff51afd7ed558ccdULL;
	i ^= i >> 33;
	i *= 0xc4ceb9fe1a85ec53ULL;
	i ^= i >> 33;
	return (uint32_t)i;
}

/* The word of the DAC at index i, as the sender means it (never zero). */
static uint32_t sw(uint64_t i)
{
	return S.wc.fmt == TXS_FMT_IQ8 ? txs_iq8_word((hash(i) & 0xffffU) | 1U) : (hash(i) | 1U);
}

/* The counter the module reads at sim time T. */
static uint32_t ctr(uint64_t T)
{
	return S.ctr0 + (uint32_t)T;
}

/* DAC: check the words that start before time T. */
static void dac_advance(uint64_t T)
{
	uint64_t upto = S.c->idx0 + (T + S.c->cpw - 1U) / S.c->cpw;

	for (; S.dac_idx < upto; S.dac_idx++) {
		uint64_t i = S.dac_idx;
		uint32_t slot = txs_slot_of(i);

		if (txs_group_of(i) != S.owner) {
			S.st.stale++;
			if (S.wrote[slot] == i + 1U) {
				S.st.stale_placed++;
			}
			continue;
		}
		if (S.wrote[slot] == i + 1U) {
			zassert_equal(S.bank[slot], sw(i), "%s: word %llu is not the sender's", S.c->name, i);
			S.st.nz++;
		} else if (S.zwrote[slot] == i + 1U || i < S.c->idx0 + G) {
			zassert_equal(S.bank[slot], 0U, "%s: word %llu is not zero", S.c->name, i);
			if (i >= S.c->idx0 + G) {
				S.st.zeros++;
			}
		} else {
			/* not written for this index: the group's old content */
			S.st.stale++;
		}
	}
}

/* The agent's write to the bank: only into the CPU's group. */
static void bank_zero(uint64_t lo, uint64_t hi)
{
	if (hi <= lo) {
		return;
	}
	zassert_not_equal(txs_group_of(lo), S.owner, "%s: zero write into the DAC's group", S.c->name);
	zassert_equal(txs_group_of(lo), txs_group_of(hi - 1U));
	S.k->zero(S.bank + txs_slot_of(lo), (uint32_t)(hi - lo));
	for (uint64_t i = lo; i < hi; i++) {
		S.wrote[txs_slot_of(i)] = 0;
		S.zwrote[txs_slot_of(i)] = i + 1U;
	}
}

static void bank_put(uint64_t lo, uint64_t hi, const struct qent *e)
{
	uint32_t n = (uint32_t)(hi - lo);
	uint32_t off = (uint32_t)(lo - e->idx);

	if (hi <= lo) {
		return;
	}
	zassert_not_equal(txs_group_of(lo), S.owner, "%s: write into the DAC's group", S.c->name);
	zassert_equal(txs_group_of(lo), txs_group_of(hi - 1U));
	if (S.wc.fmt == TXS_FMT_IQ8) {
		S.k->iq8(S.bank + txs_slot_of(lo), e->fr.b + e->srcoff + 2U * off, n);
	} else {
		S.k->word32(S.bank + txs_slot_of(lo), e->fr.b + e->srcoff + 4U * off, n);
	}
	for (uint64_t i = lo; i < hi; i++) {
		S.wrote[txs_slot_of(i)] = i + 1U;
		S.zwrote[txs_slot_of(i)] = 0;
	}
}

/* The sender's packet at time t. */
static void emit(uint64_t t, uint64_t idx, uint32_t n, uint64_t extra)
{
	const struct sim_cfg *c = S.c;
	struct frame *fr;
	uint8_t *pkt;

	S.st.sent += n;
	if (test_rng_u32() % 1000U < c->loss_pm) {
		S.st.net_lost += n;
		return;
	}
	for (int copy = 0; copy < 2; copy++) {
		if (copy == 1 && test_rng_u32() % 1000U >= c->dup_pm) {
			break;
		}
		if (S.nnet == NETCAP) {
			break;
		}
		fr = &S.net[S.nnet++];
		pkt = fr->b + 2;
		txs_wire_header(pkt, &S.wc, (uint8_t)S.seq, idx, n);
		for (uint32_t k = 0; k < n; k++) {
			uint32_t w = sw(idx + k);

			if (S.wc.fmt == TXS_FMT_IQ8) {
				/* the sender's sample is the bytes I, Q: invert the word */
				uint32_t x = ((w >> 2) & 0xffU) | ((w >> 4) & 0xff00U);

				pkt[16 + 2 * k] = (uint8_t)x;
				pkt[17 + 2 * k] = (uint8_t)(x >> 8);
			} else {
				pkt[16 + 4 * k] = (uint8_t)w;
				pkt[17 + 4 * k] = (uint8_t)(w >> 8);
				pkt[18 + 4 * k] = (uint8_t)(w >> 16);
				pkt[19 + 4 * k] = (uint8_t)(w >> 24);
			}
		}
		fr->len = 16U + (S.wc.fmt == TXS_FMT_IQ8 ? 2U : 4U) * n;
		fr->arrive = t + extra + 2000U + c->net_delay + test_rng_u32() % (c->jitter + 1U) +
			     (copy ? 3000U + test_rng_u32() % (2U * c->jitter + 1000U) : 0U);
	}
	S.seq++;
}

static void sender_step(uint64_t T)
{
	const struct sim_cfg *c = S.c;
	uint32_t lead = G + c->lead_extra;

	if (c->stall_len != 0U && !S.stalled && T >= c->stall_at) {
		S.stalled = true;
		S.stall_until = T + c->stall_len;
	}
	/* statuses that reached the sender */
	for (uint32_t i = 0; i < S.sp_n;) {
		if (S.sp[i].arrive > T) {
			i++;
			continue;
		}
		S.credit = S.sp[i].s.credit;
		if (S.sp[i].s.underruns != S.seen_u) {
			S.seen_u = S.sp[i].s.underruns;
			if (S.sp[i].s.restart > S.snext) {
				S.snext = S.sp[i].s.restart;
				S.st.resyncs++;
			}
		}
		memmove(&S.sp[i], &S.sp[i + 1U], (S.sp_n - i - 1U) * sizeof(S.sp[0]));
		S.sp_n--;
	}
	for (;;) {
		uint64_t t = S.st_free, avail, need_t;
		/* even: a payload is whole 32 bit words */
		uint32_t n = c->var_pkt ? (MINPKT + test_rng_u32() % (MAXPKT - MINPKT + 1U)) & ~1U : pk_words();

		if (t > T) {
			break;
		}
		if (t < S.stall_until) {
			if (S.stall_until > T) {
				break;
			}
			S.st_free = S.stall_until;
			continue;
		}
		/* when the source has produced up to the end of this packet (the PC's clock: skewed) */
		const uint32_t batch = c->reorder > 1U ? c->reorder : 1U;
		const uint64_t total = (uint64_t)n * batch;

		need_t = S.snext + total > c->idx0 + lead ? S.snext + total - c->idx0 - lead : 0;
		need_t = need_t * c->cpw * 1000000ULL / (uint64_t)(1000000LL + c->skew_ppm);
		avail = need_t;
		if (avail > t) {
			S.st_free = avail;
			continue;
		}
		if (S.snext + total > S.credit) {
			S.st.credit_blocked++;
			/* the time spent waiting is gone: it is not sent later in the past */
			S.st_free = T;
			break;
		}
		for (uint32_t b = 0; b < batch; b++) {
			/* the batch arrives last packet first, 50 cycles apart */
			emit(t, S.snext + (uint64_t)b * n, n, (uint64_t)(batch - 1U - b) * 50U);
		}
		S.snext += total;
		S.st_free = t + total * c->cpw * 4U / (c->send_q != 0U ? c->send_q : 8U) + 1U;
		if (test_rng_u32() % 100000U < c->stall_pp) {
			S.stall_until = t + 1U + test_rng_u32() % c->stall_max;
		}
	}
}

static void deliver(uint64_t T)
{
	for (;;) {
		uint32_t best = S.nnet;

		for (uint32_t i = 0; i < S.nnet; i++) {
			if (S.net[i].arrive <= T && (best == S.nnet || S.net[i].arrive < S.net[best].arrive)) {
				best = i;
			}
		}
		if (best == S.nnet) {
			return;
		}
		if (S.f.mode != TXS_RUN) {
			/* a stop: nothing more is played, the buffers go straight back */
			struct txs_wire_pkt pk;

			if (txs_wire_parse(&S.wc, PORT, S.net[best].b + 2, S.net[best].len, &pk) ==
			    TXS_WIRE_OK) {
				S.st.arrived += pk.words;
				S.st.flushed += pk.words;
			}
		} else if (S.nq < qcap()) {
			struct qent *e = &S.q[S.nq];

			e->fr = S.net[best];
			if (txs_wire_parse(&S.wc, PORT, e->fr.b + 2, e->fr.len, &e->pk) != TXS_WIRE_OK) {
				S.st.bad_pkts++;
			} else {
				e->idx = e->pk.index;
				e->n = e->pk.words;
				e->srcoff = (uint32_t)(e->pk.payload - e->fr.b);
				e->wait = false;
				S.st.arrived += e->n;
				S.nq++;
				S.st.qmax = MAX(S.st.qmax, S.nq);
			}
		} else {
			S.st.rx_drop++;
		}
		S.net[best] = S.net[--S.nnet];
	}
}

/* The credit depth: the buffers' words in the smallest packet, less two packets (one split by
 * a window's end and one in progress).
 */
static uint32_t depth(void);

/* Words of the smallest packet: the buffer depth is counted in them. */
static uint32_t pkt_min(void)
{
	return S.c->var_pkt ? MINPKT : pk_words();
}

static uint32_t depth(void)
{
	return S.c->depth != 0U ? S.c->depth : (qcap() - 2U) * pkt_min();
}

static void send_status(uint64_t T)
{
	if (S.sp_n < ARRAY_SIZE(S.sp)) {
		struct txs_status st;

		/* CPU1 builds the record and publishes it; the other core reads the snapshot */
		txs_status_make(&st, SESSION, txs_pos_word_after(&S.pos), &S.j, &S.f, depth());
		txs_snap_write(&S.snap, &st);
		zassert_true(txs_snap_read(&S.snap, &S.sp[S.sp_n].s));
		S.sp[S.sp_n].arrive = T + (S.c->st_delay != 0U ? S.c->st_delay : 6400U);
		S.sp_n++;
	}
	/* depth / rate / 4: the credit moves on before the sender has used it up */
	S.status_due = T + (S.c->st_period != 0U ? S.c->st_period :
			    (uint64_t)depth() * S.c->cpw / (S.c->status_div != 0U ? S.c->status_div : 4U));
}

static void queue_drop(uint32_t i)
{
	memmove(&S.q[i], &S.q[i + 1U], (S.nq - i - 1U) * sizeof(S.q[0]));
	S.nq--;
}

/* One fill chunk: the first packet of the queue that is not waiting. */
static void fill(uint64_t *Tc)
{
	uint32_t i, cw;
	struct txs_place pl;
	struct qent *e;
	uint64_t end;

	for (i = 0; i < S.nq && S.q[i].wait; i++) {
	}
	if (i == S.nq) {
		*Tc += 80;
		return;
	}
	e = &S.q[i];
	/* what of the first CHUNK words of this packet can be written in time (holes cost too) */
	cw = MIN(e->n, S.f.cfg.chunk);
	end = txs_flip_fit(&S.f, &S.pos, S.j.cursor, e->idx, e->idx + cw);
	if (end <= e->idx) {
		/* no time: wait for the point of no return */
		*Tc += 40;
		return;
	}
	cw = (uint32_t)(end - e->idx);
	txs_jit_packet(&S.j, e->idx, cw, &pl);
	switch (pl.verdict) {
	case TXS_PK_PLACE:
		bank_zero(pl.zero.lo, pl.zero.hi);
		bank_put(pl.put.lo, pl.put.hi, e);
		*Tc += 60U + (pl.zero.hi - pl.zero.lo) * S.f.cfg.zcost +
		       (pl.put.hi - pl.put.lo) * S.f.cfg.cost;
		break;
	case TXS_PK_WAIT:
		e->wait = true;
		*Tc += 30;
		return;
	default:
		*Tc += 30;
		break;
	}
	e->idx += pl.used;
	e->srcoff += (S.wc.fmt == TXS_FMT_IQ8 ? 2U : 4U) * pl.used;
	e->n -= pl.used;
	if (e->n == 0U) {
		queue_drop(i);
	} else if (pl.used < cw) {
		e->wait = true;
	}
}

static void flip(uint64_t *Tc)
{
	uint64_t due = (S.f.next - S.c->idx0) * S.c->cpw;
	/* spin to the aim (the due time less the write's minimum latency), then the write */
	/* the aim comes as a 32 bit counter value: as a time it is Tc plus the signed difference */
	uint64_t aim = (uint64_t)((int64_t)*Tc + (int32_t)(txs_flip_aim(&S.f, &S.pos) - ctr(*Tc)));
	uint64_t Tf, play, fill_at = S.j.cursor;
	uint32_t groups;

	zassert_equal(aim, due - WLAT_MIN);
	Tf = MAX(*Tc + 20U, aim) + test_rng_u32() % (SPIN_VAR + 1U) + WLAT_MIN + test_rng_u32() % (WLAT_VAR + 1U);
	txs_pos_update(&S.pos, ctr(Tf));
	play = txs_pos_word_after(&S.pos);
	dac_advance(Tf);
	groups = txs_flip_done(&S.f, play, fill_at, S.j.win_data);
	S.owner = S.f.dac_group;
	zassert_true(groups >= 1U);
	if (fill_at < S.j.hi) {
		S.st.windows_unfilled += S.j.hi - fill_at;
	}
	txs_jit_flip(&S.j, &S.f);
	for (uint32_t i = 0; i < S.nq; i++) {
		S.q[i].wait = false;
	}
	*Tc = Tf;
	send_status(Tf);
}

static void sim_run(const struct sim_cfg *c)
{
	struct txs_flip_cfg fc = {
		.cost = c->cost != 0U ? c->cost : c->cpw / 2U, .zcost = c->cpw / 4U, .lock_lead = c->lock_lead != 0U ? c->lock_lead : 400U,
		.chunk = c->chunk != 0U ? c->chunk : CHUNK,
		.overhead = 80, .write_lat = WLAT_MIN,
		/* the write lands up to 11 cycles late and the spin exits up to 13 late */
		.tol = (WLAT_VAR + SPIN_VAR + c->cpw - 1U) / c->cpw,
	};
	uint64_t Tc = 0, stop_at = 0;
	/* the rule of txs_flip_init() and room for a preemption-free spin of the loop */

	uint64_t total = (uint64_t)(c->windows + 3U) * G * c->cpw;
	bool stopped = false;
	uint64_t iters = 0;

	fc.margin = fc.lock_lead + fc.chunk * fc.zcost + fc.overhead + 500U;
	memset(&S, 0, sizeof(S));
	test_rng_seed(0x9e3779b9ULL * c->seed + 1U);
	S.c = c;
	S.k = c->fast_kernels ? &txs_kern_fast : &txs_kern_ref;
	S.wc = (struct txs_wire_cfg){ .stream_id = SESSION, .port = PORT, .fmt = c->fmt,
				      .max_words = c->pkt != 0U ? c->pkt : MAXPKT };
	/* the counter wraps somewhere in the first half of the run */
	S.ctr0 = (uint32_t)(0x100000000ULL - 1U - test_rng_u32() % (uint32_t)(total / 2U));
	zassert_equal(txs_pos_init(&S.pos, S.ctr0, c->cpw, c->idx0), 0);
	zassert_equal(txs_flip_init(&S.f, &fc, c->idx0 + G), 0);
	zassert_equal(txs_jit_init(&S.j, &S.f, depth(), depth()), 0);
	txs_snap_init(&S.snap);
	S.owner = txs_group_of(c->idx0);
	S.dac_idx = c->idx0;
	S.snext = c->idx0 + G;
	/* before the first data word: the first window's end */
	S.credit = S.snext + G;
	if (c->prefill) {
		/* the host fills the first window up to the credit first, and the engine starts when it is in:
		 * the window's packets come at the speed of the module's fill, and the credit has opened
		 * (the first status said started) by the time the engine runs */
		const uint32_t n = pk_words();
		const uint64_t cw = fc.cost;
		uint64_t tp = 0;

		while (S.snext < c->idx0 + 2U * G) {
			const uint32_t first = S.nnet;

			emit(0, S.snext, n, 0);
			for (uint32_t i = first; i < S.nnet; i++) {
				S.net[i].arrive = tp;
			}
			S.snext += n;
			tp += n * cw;
		}
		S.credit = S.snext + depth();
	}
	if (c->stop != 0U) {
		stop_at = G * (uint64_t)c->cpw + test_rng_u32() % (total - 3U * G * c->cpw);
	}

	while (S.f.mode != TXS_OFF) {
		uint64_t idx;
		enum txs_op op;

		zassert_true(++iters < 20000000U, "%s: no end", c->name);
		if (c->preempt_pm != 0U && test_rng_u32() % 1000000U < c->preempt_pm) {
			Tc += test_rng_u32() % c->preempt_max;
		}
		if (c->hang_at != 0U && Tc >= c->hang_at && Tc < c->hang_at + c->hang_len) {
			Tc = c->hang_at + c->hang_len;
		}
		deliver(Tc);
		if (S.f.mode != TXS_RUN) {
			/* a stop: what is queued is not played, the buffers go back */
			for (uint32_t i = 0; i < S.nq; i++) {
				S.st.flushed += S.q[i].n;
			}
			S.nq = 0;
		}
		sender_step(Tc);
		if (Tc >= S.status_due) {
			send_status(Tc);
		}
		idx = txs_pos_update(&S.pos, ctr(Tc));
		zassert_equal(idx, c->idx0 + Tc / c->cpw, "%s: the position shifted at %llu", c->name, Tc);
		/* a DRAIN ends at the end of its zero window; later words are a late OFF's, not checked */
		dac_advance(S.f.mode == TXS_DRAIN && S.f.drain_left == 0U ?
			    MIN(Tc, (S.f.next - c->idx0) * c->cpw) : Tc);
		if (!stopped && ((c->stop == 0U && S.f.flips >= c->windows) ||
				 (c->stop != 0U && Tc >= stop_at))) {
			txs_flip_stop(&S.f, c->stop == 2U ? TXS_STOP_NOW : TXS_STOP_DRAIN);
			stopped = true;
		}
		op = txs_flip_poll(&S.f, &S.pos, S.j.cursor);
		switch (op) {
		case TXS_OP_FILL:
			fill(&Tc);
			break;
		case TXS_OP_ZERO: {
			struct txs_span z;

			txs_jit_close(&S.j, S.f.mode == TXS_DRAIN, &z);
			bank_zero(z.lo, z.hi);
			Tc += 60U + (z.hi - z.lo) * fc.zcost;
			break;
		}
		case TXS_OP_IDLE: {
			/* wait, but not past the point where the flip is taken up */
			uint64_t spin = (S.f.next - c->idx0) * c->cpw - fc.lock_lead;

			Tc = spin > Tc + 50U ? MIN(Tc + 2000U, spin) : Tc + 50U;
			break;
		}
		case TXS_OP_FLIP:
			flip(&Tc);
			break;
		case TXS_OP_OFF:
			break;
		}
	}
	S.T = Tc;
	/* the engine is off: the DAC plays on until the end of the window it was playing */
	if (S.f.reason == TXS_STOP_DRAINED) {
		dac_advance((S.f.next - c->idx0) * c->cpw);
	} else {
		dac_advance(Tc);
	}
	for (uint32_t i = 0; i < S.nq; i++) {
		S.st.queued_left += S.q[i].n;
	}
}

/* What every run must satisfy. */
static void check_common(const struct sim_cfg *c)
{
	const struct txs_jit *j = &S.j;
	const struct txs_flip *f = &S.f;

	zassert_equal(S.st.bad_pkts, 0U, "%s", c->name);
	if (c->stop == 2U || f->reason == TXS_STOP_MISSED) {
		/* The engine stopped inside a window: the unplayed rest of its stale words is not played.
		 * And a NOW that comes after a flip fell due left the words since then stale.
		 */
		uint64_t due = (f->next - c->idx0) * c->cpw;
		uint64_t since = S.T > due ? (S.T - due + c->cpw - 1U) / c->cpw : 0U;

		zassert_true(S.st.stale <= f->stale_words + since, "%s", c->name);
	} else {
		zassert_equal(S.st.stale, f->stale_words, "%s: stale played %llu, counted %llu",
			      c->name, S.st.stale, f->stale_words);
	}
	zassert_equal(S.st.arrived, j->placed + j->dup + j->late + j->far + S.st.queued_left + S.st.flushed,
		      "%s: arrived %llu placed %llu dup %llu late %llu far %llu left %llu flushed %llu",
		      c->name, S.st.arrived, j->placed, j->dup, j->late, j->far, S.st.queued_left,
		      S.st.flushed);
	/* NOW leaves the window being filled unplayed */
	if (c->stop == 2U) {
		/* the words of the window the engine was still playing are not all played */
		zassert_true(S.st.nz + S.st.stale_placed <= j->placed, "%s", c->name);
	} else {
		zassert_equal(S.st.nz + S.st.stale_placed, j->placed,
			      "%s: played %llu (+ %llu stale) placed %llu", c->name, S.st.nz,
			      S.st.stale_placed, j->placed);
	}
	if (c->stop != 2U) {
		zassert_equal((uint64_t)f->flips * G,
			      j->placed + txs_jit_lost(j) + j->zeroed + S.st.windows_unfilled,
			      "%s: window words: flips %llu placed %llu lost %llu zeroed %llu unfilled %llu",
			      c->name, f->flips, j->placed, txs_jit_lost(j), j->zeroed,
			      S.st.windows_unfilled);
	}
	TC_PRINT("%-14s cpw %2u %-6s stop %u: flips %llu placed %llu lost %llu rec %llu dup %llu late %llu "
		 "far %llu zeroed %llu underruns %u | lateflips %llu stale %llu worst %llu | rxdrop %llu "
		 "resync %u blocked %u qmax %u\n", c->name, c->cpw, c->fmt == TXS_FMT_IQ8 ? "iq8" : "word32",
		 c->stop, f->flips, j->placed, j->lost, j->recovered, j->dup, j->late, j->far, j->zeroed,
		 j->underruns, f->late_flips, f->stale_words, f->late_worst, S.st.rx_drop,
		 S.st.resyncs, S.st.credit_blocked, S.st.qmax);
}

/* No flip was late beyond the tolerance, no hole fill or fill overran: the only stale words are
 * the few the write latency and the spin cost at each flip.
 */
static void clean_flips(void)
{
	zassert_equal(S.f.late_flips + S.f.early_flips + S.f.missed_flips, 0U, "%s", S.c->name);
	zassert_true(S.f.stale_words <= S.f.flips * S.f.cfg.tol, "%s: stale %llu", S.c->name,
		     S.f.stale_words);
}

static const uint32_t CPWS[] = { 4, 8, 16, 24 };

static struct sim_cfg base(const char *name, uint32_t cpw, uint32_t k)
{
	return (struct sim_cfg){
		.name = name, .cpw = cpw, .fmt = (k & 1U) ? TXS_FMT_WORD32 : TXS_FMT_IQ8,
		.idx0 = (k & 2U) ? (1ULL << 32) - 6U * G : 0, .windows = 14, .seed = cpw * 10U + k,
		.fast_kernels = !(k & 4U), .lead_extra = 3000,
	};
}

/* Nothing goes wrong: no zero, no stale, no loss, nothing late; the same words in as out. */
ZTEST(txs_sim, test_clean)
{
	for (size_t i = 0; i < ARRAY_SIZE(CPWS); i++) {
		for (uint32_t k = 0; k < 8U; k++) {
			struct sim_cfg c = base("clean", CPWS[i], k);

			sim_run(&c);
			check_common(&c);
			clean_flips();
			zassert_equal(S.j.lost + S.j.dup + S.j.late + S.j.far + S.j.underruns, 0U);
			zassert_true(S.st.zeros <= S.j.zeroed, "zeros played are the drain's");
			zassert_equal(S.st.rx_drop + S.st.net_lost, 0U);
			zassert_true(S.j.placed >= (uint64_t)(c.windows - 1U) * G);
		}
	}
}

/* Jitter that reorders, loss and duplicates: gaps are zero, reordered packets fill them. */
ZTEST(txs_sim, test_jitter_loss_reorder)
{
	for (size_t i = 0; i < ARRAY_SIZE(CPWS); i++) {
		for (uint32_t k = 0; k < 8U; k++) {
			struct sim_cfg c = base("net", CPWS[i], k);

			c.var_pkt = true;
			c.loss_pm = 30;
			c.dup_pm = 40;
			c.jitter = 25000;
			sim_run(&c);
			check_common(&c);
			zassert_true(S.j.recovered > 0U, "reordering fills holes");
			zassert_true(S.j.dup > 0U);
			zassert_true(txs_jit_lost(&S.j) > 0U);
			clean_flips();
			/* 3 % of the packets lost, and a few that came too late: most words arrive */
			zassert_true(S.j.placed * 100U >= 85U * (S.f.flips - 2U) * G, "placed %llu", S.j.placed);
		}
	}
}

/* The sender stalls (SIGSTOP) and the module is preempted: underruns, zeros, late flips, resync. */
ZTEST(txs_sim, test_stalls_and_preemption)
{
	for (size_t i = 0; i < ARRAY_SIZE(CPWS); i++) {
		for (uint32_t k = 0; k < 8U; k++) {
			struct sim_cfg c = base("stall", CPWS[i], k);

			c.windows = 30;
			c.var_pkt = true;
			c.loss_pm = 5;
			c.jitter = 4000;
			c.stall_pp = 700;
			c.stall_max = 2U * G * c.cpw;
			c.preempt_pm = 4000;
			c.preempt_max = 50000;
			sim_run(&c);
			check_common(&c);
			zassert_true(S.j.underruns > 0U);
			zassert_true(S.st.resyncs > 0U, "the sender jumped ahead after an underrun");
			zassert_true(S.j.zeroed > 0U);
			/* after the stalls data came again */
			zassert_true(S.j.placed > 4U * G);
		}
	}
}

/* Many preemptions of the module: late flips are counted, the index is right afterwards. */
ZTEST(txs_sim, test_late_flips)
{
	uint64_t late_total = 0;

	for (size_t i = 0; i < ARRAY_SIZE(CPWS); i++) {
		for (uint32_t k = 0; k < 4U; k++) {
			struct sim_cfg c = base("late", CPWS[i], k);

			c.windows = 40;
			c.preempt_pm = 25000;
			c.preempt_max = 90000;
			sim_run(&c);
			check_common(&c);
			late_total += S.f.late_flips;
		}
	}
	zassert_true(late_total > 5U, "late flips happened: %llu", late_total);
}

/* The PC runs faster than the board: the credit holds it back, no buffer overflows. */
ZTEST(txs_sim, test_credit_backpressure)
{
	/* not at 4 cycles a word (80 MS/s on this CPU): the fill's own overhead per step leaves it less than
	 * twice the rate, and nothing here runs at that rate */
	for (size_t i = 1; i < ARRAY_SIZE(CPWS); i++) {
		for (uint32_t k = 0; k < 2U; k++) {
			struct sim_cfg c = base("credit", CPWS[i], k);

			/* the sender's clock runs 3 % fast, so it creeps ahead until the credit stops it, and
			 * then it sits at the window's end plus the depth: what waits for the flip, and the
			 * arrivals while the fill (twice the rate) takes it in (the sender at 1.25 times), fit
			 * the queue of 34 */
			c.lead_extra = 3000;
			c.skew_ppm = 30000;
			c.send_q = 5;
			c.prefill = true;
			c.depth = 16U * MAXPKT;
			c.windows = 40;
			sim_run(&c);
			check_common(&c);
			zassert_true(S.st.credit_blocked > 0U, "the credit limit held the sender");
			zassert_equal(S.st.rx_drop, 0U);
			zassert_equal(S.j.far + S.j.late + S.j.underruns, 0U);
			clean_flips();
		}
	}
}

/* Stop at a random time, DRAIN and NOW: in every state of a window, with trouble on the way. */
ZTEST(txs_sim, test_stop_anywhere)
{
	for (uint32_t seed = 1; seed <= 40U; seed++) {
		for (uint32_t stop = 1; stop <= 2U; stop++) {
			struct sim_cfg c = base(stop == 1U ? "drain" : "now", CPWS[seed % 4U], seed);

			c.windows = 8;
			c.seed = seed * 31U + stop;
			c.stop = stop;
			c.loss_pm = (seed & 1U) ? 20 : 0;
			c.jitter = (seed & 2U) ? 20000 : 0;
			c.stall_pp = (seed & 4U) ? 800 : 0;
			c.stall_max = G * c.cpw;
			c.preempt_pm = (seed & 8U) ? 6000 : 0;
			c.preempt_max = 40000;
			sim_run(&c);
			check_common(&c);
			zassert_equal(S.f.reason, stop == 1U ? TXS_STOP_DRAINED : TXS_STOP_NOW,
				      "seed %u", seed);
		}
	}
}

/* The module hangs for two and a half windows: the flip is missed by groups. */
ZTEST(txs_sim, test_missed_flip)
{
	for (size_t i = 0; i < ARRAY_SIZE(CPWS); i++) {
		for (uint32_t k = 0; k < 4U; k++) {
			struct sim_cfg c = base("missed", CPWS[i], k);

			c.windows = 20;
			c.hang_at = 6U * G * c.cpw + G * c.cpw / 3U;
			c.hang_len = 5U * G * c.cpw / 2U;
			sim_run(&c);
			check_common(&c);
			zassert_true(S.f.missed_flips > 0U);
			zassert_true(S.j.underruns > 0U && S.st.resyncs > 0U);
		}
	}
}

/*
 * The window dynamics of the real configuration (apps/txstream_eth): 20 MS/s on a 320 MHz CPU,
 * 8+8 bit packets of 702 words, `qcap` buffers, the window of 32768 words that opens at the flip.
 * The sender stays a group plus `lead_extra` words ahead of the play position and sends at twice
 * the rate when it may; the credit holds it at the open window's end plus the depth, (qcap - 2) x 702.
 * What the window needs: the data of its last word is due when the next flip comes, a pass later;
 * a sender in step with the clock (lead_extra 0) has no slack, and what it has ready ahead of the
 * window's opening is at most the depth (the buffers).
 */
static struct sim_cfg real_cfg(const char *name, uint32_t lead_extra, uint32_t qcap)
{
	struct sim_cfg c = base(name, 16, 0);

	c.windows = 40;
	c.pkt = 702;
	c.qcap = qcap;
	c.lead_extra = lead_extra;
	c.chunk = 702;
	c.lock_lead = 6000;
	c.jitter = 2000;
	return c;
}

ZTEST(txs_sim, test_real_config_lead)
{
	static const uint32_t leads[] = {0, 500, 1000, 3000, 7020};
	uint32_t under[ARRAY_SIZE(leads)];

	for (size_t i = 0; i < ARRAY_SIZE(leads); i++) {
		struct sim_cfg c = real_cfg("lead", leads[i], 12);

		sim_run(&c);
		check_common(&c);
		under[i] = S.j.underruns;
		TC_PRINT("lead_extra %5u words (%4u us): underruns %u, lost %llu, zeroed %llu, late flips %llu, "
			 "credit blocked %u\n", leads[i], leads[i] / 20U, S.j.underruns,
			 (unsigned long long)S.j.lost, (unsigned long long)S.j.zeroed,
			 (unsigned long long)S.f.late_flips, S.st.credit_blocked);
	}
	/* a sender in step with the clock has its data when the window needs it, with some words to spare */
	zassert_equal(under[ARRAY_SIZE(leads) - 1U], 0U, "a lead of the depth");
	zassert_equal(under[3], 0U, "a lead of 3000 words");
}

/* One stall of the sender, how long can it be: with 12 buffers and with the pool's 34. */
ZTEST(txs_sim, test_real_config_stall)
{
	static const uint32_t bufs[] = {12, 19, 34};

	for (size_t b = 0; b < ARRAY_SIZE(bufs); b++) {
		const uint32_t depth_w = (bufs[b] - 2U) * 702U;
		uint32_t tol_us = 0;

		for (uint32_t us = 100; us <= 2000U; us += 100U) {
			uint32_t worst = 0;

			/* the stall at several phases of the window */
			for (uint32_t ph = 0; ph < 8U; ph++) {
				struct sim_cfg c = real_cfg("stall", depth_w, bufs[b]);

				c.seed += ph * 7U;
				c.stall_at = (10ULL * G + ph * G / 8U) * c.cpw;
				c.stall_len = (uint64_t)us * 320U;
				sim_run(&c);
				check_common(&c);
				worst = MAX(worst, S.j.underruns);
			}
			if (worst == 0U) {
				tol_us = us;
			} else {
				break;
			}
		}
		TC_PRINT("%2u buffers (depth %5u words = %4u us): the stall tolerated with no underrun at any "
			 "window phase: %u us\n", bufs[b], depth_w, depth_w / 20U, tol_us);
		if (b == 0U) {
			zassert_true(tol_us >= 300U, "12 buffers");
		}
	}
}

/* How often the status must come: depth / rate / div for div 4 (88 us here), 2, 1 (351 us). */
ZTEST(txs_sim, test_real_config_status_period)
{
	static const uint32_t divs[] = {8, 4, 2, 1};
	static const uint32_t leads[] = {3000, 7020};

	for (size_t l = 0; l < ARRAY_SIZE(leads); l++) {
		for (size_t i = 0; i < ARRAY_SIZE(divs); i++) {
			struct sim_cfg c = real_cfg("status", leads[l], 12);

			c.status_div = divs[i];
			sim_run(&c);
			check_common(&c);
			TC_PRINT("lead %u, status every %3u us (depth/rate/%u): underruns %u, credit blocked %u, "
				 "sent %llu words\n", leads[l], 7020U / 20U / divs[i], divs[i], S.j.underruns,
				 S.st.credit_blocked, (unsigned long long)S.st.sent);
		}
	}
}

/* The injector's reorder: batches sent last packet first. Reordered packets are recovered; what is
 * not (batches that end within a margin of the flip) is bounded only for the lead of 7020 words and
 * batches up to 4: not every word comes back.
 */
ZTEST(txs_sim, test_real_config_reorder)
{
	static const uint32_t leads[] = {3000, 7020};
	static const uint32_t batches[] = {2, 4, 8};

	for (size_t l = 0; l < ARRAY_SIZE(leads); l++) {
		for (size_t i = 0; i < ARRAY_SIZE(batches); i++) {
			struct sim_cfg c = real_cfg("reorder", leads[l], 12);

			c.reorder = batches[i];
			sim_run(&c);
			check_common(&c);
			TC_PRINT("lead %u, reorder %u: late %llu, lost %llu, recovered %llu, dup %llu, far %llu, "
				 "underruns %u, zeroed %llu, sent %llu\n", leads[l], batches[i],
				 (unsigned long long)S.j.late, (unsigned long long)S.j.lost,
				 (unsigned long long)S.j.recovered, (unsigned long long)S.j.dup,
				 (unsigned long long)S.j.far, S.j.underruns, (unsigned long long)S.j.zeroed,
				 (unsigned long long)S.st.sent);
			/* reordered packets are recovered; what is not is the batches that end within a margin of
			 * the flip (this sender is in step with the clock) */
			zassert_true(S.j.recovered > 0U);
			if (leads[l] == 7020U && batches[i] <= 4U) {
				zassert_true(S.j.lost - S.j.recovered <= S.st.sent / 50U, "more than 2 percent lost");
			}
		}
	}
}

/*
 * The round trip. The sender and the board are a network apart: data takes `d_data` to arrive, the
 * status `d_st` to reach the sender, a status goes at every flip and every 250 us. The credit is the
 * open window's end plus the depth; the sender keeps a group plus `lead_extra` words ahead of the play
 * position, and needs lead_extra >= rate x d_data + 120 us of slack for the window's last word
 * to be in before the point of no return. The credit it knows lags by d_st: at a flip the new window's
 * end has not reached it, and its position rises by rate x d_st words meanwhile, so it needs
 * depth >= lead_extra + rate x d_st: depth >= rate x (RTT + 120 us). (A sender that catches up after
 * waiting for the credit is faster than the fill: only a sender that keeps its lead needs no catching up.)
 * Cases: 300 and 600 us round trip (half each way), 20 and 40 MS/s (CPU 320 MHz: 16 and 8 cycles a
 * word), 12 buffers (depth 7020) and as many as the round trip needs. Asserted: where the depth rule
 * holds nothing is lost, late or underrun, and the credit is never exceeded (no far word, no drop).
 */
ZTEST(txs_sim, test_round_trip)
{
	static const uint32_t cpws[] = {16, 8};
	static const uint32_t rtts[] = {300, 600};
	/* the last word of a window must be in this long before its flip: the point of no return, the
	 * module's fill of the packet, the network's jitter */
	static const uint32_t slack_us = 120;

	for (size_t i = 0; i < ARRAY_SIZE(cpws); i++) {
		for (size_t r = 0; r < ARRAY_SIZE(rtts); r++) {
			const uint32_t rate = 320U / cpws[i]; /* words a microsecond */
			const uint32_t owd_cyc = rtts[r] / 2U * 320U;
			const uint32_t need = rate * (rtts[r] + slack_us) + 702U;
			/* buffers for the depth the rule asks for, at least the 12 of the board */
			const uint32_t qneed = MAX(12U, (need + 701U) / 702U + 2U);

			for (size_t q = 0; q < 2U; q++) {
				const uint32_t qc = q == 0U ? 12U : qneed;
				struct sim_cfg c = real_cfg("rtt", rate * (rtts[r] / 2U + slack_us), qc);
				const uint32_t depth_w = (qc - 2U) * 702U;
				const bool rule = depth_w >= need - 702U;

				c.cpw = cpws[i];
				c.net_delay = owd_cyc;
				c.st_delay = owd_cyc;
				c.st_period = 250U * 320U;
				/* a host bursts at line rate: 3 times the rate at 20 MS/s, 1.5 at 40; the module's
				 * fast kernel fills 2 times as fast (a word costs cpw / 2 cycles) */
				c.send_q = cpws[i] == 16U ? 12U : 6U;
				c.prefill = true;
				c.windows = 30;
				c.lock_lead = 6000;
				sim_run(&c);
				check_common(&c);
				TC_PRINT("rtt %3u us, %2u MS/s, %2u buffers (depth %5u, rule asks %5u), lead_extra %5u: "
					 "underruns %u lost %llu late %llu far %llu rxdrop %llu blocked %u\n", rtts[r],
					 rate, qc, depth_w, need - 702U, c.lead_extra, S.j.underruns,
					 (unsigned long long)txs_jit_lost(&S.j), (unsigned long long)S.j.late,
					 (unsigned long long)S.j.far, (unsigned long long)S.st.rx_drop,
					 S.st.credit_blocked);
				/* the credit is never exceeded, with the depth the rule asks for or not (no word beyond
				 * the sender's limit, no packet that finds the queue full), as long as the round trip's
				 * words fit twice the depth. 600 us at 40 MS/s with 12 buffers does not: the board
				 * underruns, and the data that comes too late for the open window waits in the queue
				 * (on the board: "stream busy" drops). Printed.*/
				if (rate * rtts[r] <= 2U * depth_w) {
					zassert_equal(S.st.rx_drop + S.j.far, 0U, "the credit was exceeded");
				}
				/* with the depth the rule asks for nothing is lost; with less (the board's 12 buffers at
				 * 600 us and 40 MS/s) the sender sits on the credit and the board underruns: printed */
				if (rule) {
					zassert_equal(S.j.underruns + txs_jit_lost(&S.j) + S.j.late, 0U);
				}
			}
		}
	}
}
