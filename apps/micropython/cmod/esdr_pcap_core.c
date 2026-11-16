/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Pcap app's packet capture (esdr_pcap_core.h). The detector is apps/pcap's src/narrow.c
 * on int16 samples; the decoding of a closed burst is its src/decoder.c (decode(), done(),
 * record_meta()) for one channel, at once (esdr_pcap_feed()) or by the other end of a queue.
 * The detector's fields are its thread's, the decoders' theirs; the two queues of buffer
 * numbers are the only shared state (one producer, one consumer each).
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "dec_util.h"
#include "decode.h"
#include "esdr_pcap_core.h"
#include "pcap_proto.h"
#include "pcapng.h"
#include "records.h"

/* The detector (apps/pcap/src/narrow.c): samples a block, the pre-roll, quiet blocks before
 * closing, loud blocks a burst needs, blocks the floor is first learnt from and its follow
 * rate. */
#define NB_BLOCK    16U
#define NB_PRE      64U
#define NB_HANG     6U
#define NB_MIN_LOUD 3U
#define NB_LEARN    32U
#define NB_FOLLOW   64.0f

/* The decoders' gates and limits (apps/pcap/src/decoder.c, which gives their reasons). */
#define GATE_POINTS        256U
#define GATE_NARROW        0.45f
#define GATE_NARROW_1M     0.85f
#define GATE_NARROW_2M_MAX 0.92f
#define GATE_WIDE          0.5f
#define GATE_SNR           1.0f
#define BLE_US_MIN         64U
#define BLE_US_MAX         2400U
#define WPAN_US_MIN        300U
#define WPAN_US_MAX        4500U
#define MIN_BURST_US       20U
#define GEN_SAMPLES        8192U
#define AA_LEARNT          4U

/* Packets one call keeps at most. */
#define OUT_MAX 128U

#define MAX_OF(a, b) ((a) > (b) ? (a) : (b))
#define MIN_OF(a, b) ((a) < (b) ? (a) : (b))

struct out_item {
	int64_t at;
	int32_t offset_hz;
	uint32_t duration_us;
	int16_t power_x10, floor_x10;
	uint16_t len, comment_len;
	uint8_t iface;
	bool ok;
};

struct esdr_pcap {
	struct esdr_pcap_cfg cfg;
	/* The detector's floor, the current block's power sum and samples, quiet blocks in a row
	 * while open, blocks learnt. */
	float floor;
	uint64_t acc;
	uint32_t in_block, quiet, learnt;
	float learn[NB_LEARN];
	/* The last NB_PRE samples (a word each: an I, Q pair in memory), oldest at pre_at once
	 * full. */
	uint32_t pre[NB_PRE];
	uint32_t pre_at, pre_n;
	/* The next sample's index on the caller's axis. */
	int64_t next;
	bool have_next, open, skip, loud_set;
	uint32_t loud_blocks;
	/* The burst being assembled (in the pool), NULL while none is open, and buffers the
	 * detector holds besides (taken from free_q, or its own bursts that went unseen). */
	struct burst *cur;
	uint8_t own[ESDR_PCAP_POOL_MAX];
	uint32_t own_n;
	/* esdr_pcap_feed(): each burst decoded as it closes. */
	bool sync;
	/* The pool: burst buffers (iq: burst_max samples each), free ones for the detector and
	 * closed ones for the decoders. */
	struct burst slot[ESDR_PCAP_POOL_MAX];
	struct queue {
		uint32_t head, tail;
		uint8_t idx[ESDR_PCAP_POOL_MAX];
	} free_q, closed_q;
	/* Bursts no decoder can claim, by their numbers alone (the detector's copies, no samples),
	 * so that their buffers stay free for the others while the decoders are behind. */
	struct meta_queue {
		uint32_t head, tail;
		struct burst b[ESDR_PCAP_POOL_MAX];
	} meta_q;
	/* The burst being decoded. */
	struct burst *b;
	/* Access addresses of CONNECT_IND PDUs seen, the newest AA_LEARNT. */
	struct dec_ble_aa learnt_aa[AA_LEARNT];
	uint8_t learnt_n, learnt_next;
	bool claimed;
	uint32_t analyse_left;
	void *scratch;
	size_t scratch_len;
	uint8_t rec[REC_BURST_MAX + REC_BLE_MAX] __attribute__((aligned(4)));
	char comment[REC_COMMENT_MAX];
	struct dec_generic_out gen;
	/* The packets of the last call: ESDR_PCAP_OUT_BYTES at out, where each starts. */
	uint8_t *out;
	uint32_t out_fill, out_n;
	uint32_t off[OUT_MAX];
	struct esdr_pcap_stats st;
};

static uint32_t now(const struct esdr_pcap *c)
{
	return c->cfg.clock != NULL ? c->cfg.clock() : 0U;
}

static size_t al16(size_t n)
{
	return (n + 15U) & ~(size_t)15U;
}

static uint32_t burst_max(const struct esdr_pcap_cfg *cfg)
{
	return cfg->burst_max != 0U ? cfg->burst_max : cfg->rate_hz / 200U;
}

static uint32_t pool_size(const struct esdr_pcap_cfg *cfg)
{
	return cfg->pool != 0U ? cfg->pool : 2U;
}

/* ---- the queues: one thread puts, one takes ---- */

static bool q_put(struct queue *q, uint8_t v)
{
	uint32_t h = q->head;

	if (h - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE) >= ESDR_PCAP_POOL_MAX) {
		return false;
	}
	q->idx[h % ESDR_PCAP_POOL_MAX] = v;
	__atomic_store_n(&q->head, h + 1U, __ATOMIC_RELEASE);
	return true;
}

static bool q_get(struct queue *q, uint8_t *v)
{
	uint32_t t = q->tail;

	if (t == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)) {
		return false;
	}
	*v = q->idx[t % ESDR_PCAP_POOL_MAX];
	__atomic_store_n(&q->tail, t + 1U, __ATOMIC_RELEASE);
	return true;
}

static uint32_t q_len(const struct queue *q)
{
	return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) -
	       __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}

/* The decoders' side: a buffer back to the detector. */
static void slot_free(struct esdr_pcap *c, struct burst *b)
{
	(void)q_put(&c->free_q, (uint8_t)(b - c->slot));
}

/* The detector's side: a buffer it keeps, and one for a new burst (NULL: none free). */
static void slot_keep(struct esdr_pcap *c, struct burst *b)
{
	c->own[c->own_n++] = (uint8_t)(b - c->slot);
}

static struct burst *slot_take(struct esdr_pcap *c)
{
	uint8_t k;

	if (c->own_n != 0U) {
		return &c->slot[c->own[--c->own_n]];
	}
	return q_get(&c->free_q, &k) ? &c->slot[k] : NULL;
}

/* The metadata queue: the detector puts copies without samples, the decoders take them. */
static bool m_put(struct meta_queue *q, const struct burst *b)
{
	uint32_t h = q->head;

	if (h - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE) >= ESDR_PCAP_POOL_MAX) {
		return false;
	}
	q->b[h % ESDR_PCAP_POOL_MAX] = *b;
	q->b[h % ESDR_PCAP_POOL_MAX].iq = NULL;
	__atomic_store_n(&q->head, h + 1U, __ATOMIC_RELEASE);
	return true;
}

static bool m_get(struct meta_queue *q, struct burst *b)
{
	uint32_t t = q->tail;

	if (t == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)) {
		return false;
	}
	*b = q->b[t % ESDR_PCAP_POOL_MAX];
	__atomic_store_n(&q->tail, t + 1U, __ATOMIC_RELEASE);
	return true;
}

static uint32_t m_len(const struct meta_queue *q)
{
	return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE) -
	       __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}

static size_t scratch_size(uint32_t n)
{
	return MAX_OF(MAX_OF(dec_ble_scratch(n), dec_wpan_scratch(n)), dec_generic_scratch(n));
}

size_t esdr_pcap_size(const struct esdr_pcap_cfg *cfg)
{
	uint32_t n;

	if (cfg == NULL || (cfg->rate_hz != 2000000U && cfg->rate_hz != 4000000U) ||
	    cfg->shift > 8U) {
		return 0;
	}
	n = burst_max(cfg);
	if (n < 2U * NB_PRE || n > BURST_MAX_SAMPLES || pool_size(cfg) < 2U ||
	    pool_size(cfg) > ESDR_PCAP_POOL_MAX) {
		return 0;
	}
	return 16U + al16(sizeof(struct esdr_pcap)) + pool_size(cfg) * al16(4U * (size_t)n) +
	       al16(scratch_size(n)) + ESDR_PCAP_OUT_BYTES;
}

static void detector_reset(struct esdr_pcap *c)
{
	c->floor = 0.0f;
	c->acc = 0U;
	c->in_block = 0U;
	c->quiet = 0U;
	c->learnt = 0U;
	c->pre_at = 0U;
	c->pre_n = 0U;
	c->have_next = false;
	c->open = false;
	c->skip = false;
	if (c->cur != NULL) {
		slot_keep(c, c->cur);
		c->cur = NULL;
	}
}

struct esdr_pcap *esdr_pcap_init(void *mem, size_t size, const struct esdr_pcap_cfg *cfg)
{
	size_t need = esdr_pcap_size(cfg);
	uint8_t *p = mem;
	struct esdr_pcap *c;
	uint32_t n;

	if (need == 0U || mem == NULL || size < need) {
		return NULL;
	}
	p = (uint8_t *)al16((uintptr_t)p);
	c = (struct esdr_pcap *)p;
	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	if (!(c->cfg.ratio > 1.0f)) {
		c->cfg.ratio = 4.0f;
	}
	n = burst_max(cfg);
	c->cfg.burst_max = n;
	c->cfg.pool = pool_size(cfg);
	p += al16(sizeof(*c));
	for (uint32_t k = 0; k < c->cfg.pool; k++) {
		c->slot[k].iq = (int16_t *)p;
		p += al16(4U * (size_t)n);
		(void)q_put(&c->free_q, (uint8_t)k);
	}
	c->scratch = p;
	c->scratch_len = al16(scratch_size(n));
	p += c->scratch_len;
	c->out = p;
	detector_reset(c);
	return c;
}

void esdr_pcap_tune(struct esdr_pcap *c, uint64_t freq_hz, uint8_t gain)
{
	c->cfg.freq_hz = freq_hz;
	c->cfg.gain = gain;
	detector_reset(c);
}

void esdr_pcap_get_stats(const struct esdr_pcap *c, struct esdr_pcap_stats *st)
{
	*st = c->st;
}

uint32_t esdr_pcap_rate(const struct esdr_pcap *c)
{
	return c->cfg.rate_hz;
}

/* ---- the packets of a call ---- */

static void put(struct esdr_pcap *c, uint8_t iface, bool ok, int64_t at, int16_t power_x10,
		int32_t offset_hz, uint32_t duration_us, const uint8_t *rec, uint32_t len,
		const char *comment)
{
	size_t cl = comment != NULL ? strlen(comment) + 1U : 0U;
	size_t need = (sizeof(struct out_item) + len + cl + 7U) & ~(size_t)7U;
	struct out_item it = {
		.at = at,
		.offset_hz = offset_hz,
		.duration_us = duration_us,
		.power_x10 = power_x10,
		.floor_x10 = dec_dbfs_x10(c->b->floor),
		.len = (uint16_t)len,
		.comment_len = (uint16_t)cl,
		.iface = iface,
		.ok = ok,
	};
	uint8_t *p;

	if (c->out_n >= OUT_MAX || c->out_fill + need > ESDR_PCAP_OUT_BYTES || len > UINT16_MAX) {
		c->st.lost++;
		return;
	}
	p = c->out + c->out_fill;
	memcpy(p, &it, sizeof(it));
	memcpy(p + sizeof(it), rec, len);
	if (cl != 0U) {
		memcpy(p + sizeof(it) + len, comment, cl);
	}
	c->off[c->out_n++] = c->out_fill;
	c->out_fill += (uint32_t)need;
}

bool esdr_pcap_next(const struct esdr_pcap *c, uint32_t k, struct esdr_pcap_pkt *p)
{
	struct out_item it;
	const uint8_t *q;

	if (k >= c->out_n) {
		return false;
	}
	q = c->out + c->off[k];
	memcpy(&it, q, sizeof(it));
	*p = (struct esdr_pcap_pkt){
		.iface = it.iface,
		.ok = it.ok,
		.power_x10 = it.power_x10,
		.floor_x10 = it.floor_x10,
		.at = it.at,
		.offset_hz = it.offset_hz,
		.duration_us = it.duration_us,
		.rec = q + sizeof(it),
		.len = it.len,
		.comment = it.comment_len != 0U ? (const char *)q + sizeof(it) + it.len : NULL,
	};
	return true;
}

/* ---- decoding a closed burst (apps/pcap/src/decoder.c) ---- */

static uint32_t span_us(uint32_t samples, uint32_t rate_hz)
{
	return (uint32_t)((uint64_t)samples * 1000000U / rate_hz);
}

static const char *comment(struct esdr_pcap *c, int32_t offset_hz, float power,
			   uint32_t duration_us, const char *phy)
{
	const struct burst *b = c->b;
	const struct rec_meta m = {
		.freq_hz = b->freq_hz,
		.offset_hz = offset_hz,
		.power_dbfs_x10 = dec_dbfs_x10(power),
		.floor_dbfs_x10 = dec_dbfs_x10(b->floor),
		.gain = b->gain,
		.channel = b->chan,
		.channels = b->channels,
		.rate_hz = b->rate_hz,
		.duration_us = duration_us,
		.phy = phy,
	};

	if ((c->cfg.decoders & PCAP_DEC_COMMENTS) == 0U ||
	    rec_comment(c->comment, sizeof(c->comment), &m) < 0) {
		return NULL;
	}
	return c->comment;
}

static void aa_learn(struct esdr_pcap *c, uint32_t aa, uint32_t crc_init)
{
	for (unsigned int k = 0; k < c->learnt_n; k++) {
		if (c->learnt_aa[k].aa == aa) {
			c->learnt_aa[k].crc_init = crc_init;
			return;
		}
	}
	c->learnt_aa[c->learnt_next] = (struct dec_ble_aa){.aa = aa, .crc_init = crc_init};
	c->learnt_next = (uint8_t)((c->learnt_next + 1U) % AA_LEARNT);
	c->learnt_n = (uint8_t)MIN_OF(c->learnt_n + 1U, AA_LEARNT);
}

void esdr_pcap_learn(struct esdr_pcap *c, uint32_t aa, uint32_t crc_init)
{
	aa_learn(c, aa, crc_init);
}

bool esdr_pcap_learnt(const struct esdr_pcap *c, uint32_t k, uint32_t *aa, uint32_t *crc_init)
{
	if (k >= c->learnt_n) {
		return false;
	}
	*aa = c->learnt_aa[k].aa;
	*crc_init = c->learnt_aa[k].crc_init;
	return true;
}

static void ble_emit(const struct dec_ble_pkt *p, void *user)
{
	struct esdr_pcap *c = user;
	const struct burst *b = c->b;
	/* On air: the preamble (1 byte at LE 1M, 2 at LE 2M) and the bytes from the address on. */
	uint32_t us = p->phy == DEC_BLE_PHY_2M ? (2U + p->len) * 4U : (1U + p->len) * 8U;
	bool ok = p->crc_ok || !p->crc_checked;
	int n;

	if (ok) {
		c->claimed = true;
	}
	if (p->conn_aa != 0U) {
		aa_learn(c, p->conn_aa, p->conn_crc_init);
	}
	n = rec_ble(c->rec, sizeof(c->rec), p, dec_ble_rf_channel(b->freq_hz));
	if (n <= 0) {
		return;
	}
	if (ok) {
		c->st.ble++;
	} else {
		c->st.ble_bad++;
	}
	put(c, PCAP_IF_BLE, ok, (int64_t)b->m0 + p->start, dec_dbfs_x10(p->power), p->offset_hz, us,
	    c->rec, (uint32_t)n,
	    comment(c, p->offset_hz, p->power, us, p->phy == DEC_BLE_PHY_2M ? "LE 2M" : "LE 1M"));
}

static void wpan_emit(const struct dec_wpan_pkt *p, void *user)
{
	struct esdr_pcap *c = user;
	const struct burst *b = c->b;
	uint32_t us = (6U + p->len) * 32U;
	int n;

	if (p->fcs_ok) {
		c->claimed = true;
	}
	n = rec_wpan(c->rec, sizeof(c->rec), p, b->freq_hz);
	if (n <= 0) {
		return;
	}
	if (p->fcs_ok) {
		c->st.wpan++;
	} else {
		c->st.wpan_bad++;
	}
	put(c, PCAP_IF_WPAN, p->fcs_ok, (int64_t)b->m0 + p->start, dec_dbfs_x10(p->power),
	    p->offset_hz, us, c->rec, (uint32_t)n,
	    comment(c, p->offset_hz, p->power, us, "O-QPSK"));
}

struct gate {
	float snr, narrow;
};

/* decoder.c's gate_look(): power above the floor over the floor, and the correlation one
 * sample apart over that power, at up to GATE_POINTS points of the loud part. */
static struct gate gate_look(const struct burst *b)
{
	uint32_t from = b->loud_to > b->loud_from ? b->loud_from : 0U;
	uint32_t to = b->loud_to > b->loud_from ? b->loud_to : b->n;
	uint32_t step = MAX_OF((to - from) / GATE_POINTS, 1U);
	float r0 = 0.0f, ri = 0.0f, rq = 0.0f, sig;
	uint32_t pts = 0;
	struct gate g = {0};

	for (uint32_t k = MAX_OF(from, 1U); k < to; k += step, pts++) {
		float i0 = b->iq[2U * k], q0 = b->iq[2U * k + 1U];
		float i1 = b->iq[2U * k - 2U], q1 = b->iq[2U * k - 1U];

		r0 += i0 * i0 + q0 * q0;
		ri += i0 * i1 + q0 * q1;
		rq += q0 * i1 - i0 * q1;
	}
	sig = r0 - b->floor * (float)pts;
	if (sig > 0.0f && pts > 0U) {
		g.snr = b->floor > 0.0f ? sig / (b->floor * (float)pts) : 1e9f;
		g.narrow = sqrtf(ri * ri + rq * rq) / sig;
	}
	return g;
}

/* gate_look() once per burst (decoder.c's gate()): the detector may have looked already. */
static struct gate gate(struct burst *b)
{
	if ((b->flags & BURST_F_GATED) == 0U) {
		struct gate g = gate_look(b);

		b->gate_snr = g.snr;
		b->gate_narrow = g.narrow;
		b->flags |= BURST_F_GATED;
	}
	return (struct gate){.snr = b->gate_snr, .narrow = b->gate_narrow};
}

/* A record of the burst's metadata alone (decoder.c's record_meta()). */
static void record_meta(struct esdr_pcap *c, uint8_t mod, uint16_t flags)
{
	struct burst v = *c->b;
	struct pcap_burst_hdr *h = &c->gen.hdr;
	int n;

	v.n = 0U;
	v.iq = NULL;
	if (dec_generic_run(&v, NULL, NULL, 0U, &c->gen) != 0) {
		return;
	}
	if (mod != PCAP_MOD_UNKNOWN) {
		h->modulation = mod;
	}
	h->flags |= flags;
	n = rec_burst(c->rec, sizeof(c->rec), h, NULL);
	if (n > 0) {
		put(c, PCAP_IF_BURST, true, (int64_t)c->b->m0 + c->gen.start, h->power_dbfs_x10,
		    h->offset_hz, h->duration_us, c->rec, (uint32_t)n, NULL);
	}
}

static void analyse(struct esdr_pcap *c)
{
	const struct dec_generic_cfg gc = {
		.max_bits = PCAP_BURST_BITS_MAX,
		.max_samples = GEN_SAMPLES,
	};
	struct pcap_burst_hdr *h = &c->gen.hdr;
	int n;

	if (dec_generic_run(c->b, &gc, c->scratch, c->scratch_len, &c->gen) != 0) {
		return;
	}
	n = rec_burst(c->rec, sizeof(c->rec), h, c->gen.bits);
	if (n > 0) {
		put(c, PCAP_IF_BURST, true, (int64_t)c->b->m0 + c->gen.start, h->power_dbfs_x10,
		    h->offset_hz, h->duration_us, c->rec, (uint32_t)n, NULL);
	}
}

/* What no decoder claimed: a band filling the channel as a record unanalysed, the rest
 * analysed while the call's budget lasts, then as records of their metadata (decoder.c's
 * done()). */
static void unclaimed(struct esdr_pcap *c)
{
	struct burst *b = c->b;
	uint32_t loud = b->loud_to > b->loud_from ? b->loud_to - b->loud_from : b->n;
	struct gate g;

	if ((c->cfg.decoders & PCAP_DEC_GENERIC) == 0U || b->n == 0U ||
	    span_us(loud, b->rate_hz) < MIN_BURST_US) {
		return;
	}
	g = gate(b);
	if (g.snr >= GATE_SNR && g.narrow < GATE_WIDE) {
		record_meta(c, PCAP_MOD_WIDEBAND, 0U);
		c->st.wide++;
	} else if (c->analyse_left == 0U) {
		record_meta(c, PCAP_MOD_UNKNOWN, PCAP_BF_SKIPPED);
		c->st.skipped++;
	} else {
		uint32_t t0 = now(c);

		c->analyse_left--;
		analyse(c);
		c->st.generic++;
		c->st.cyc_generic += now(c) - t0;
	}
}

#define WANT_BLE  0x1U
#define WANT_WPAN 0x2U

/* The decoders decode() gives @p b (decoder.c's decode()'s conditions); looks at it once. */
static uint32_t wants(const struct esdr_pcap *c, struct burst *b)
{
	const uint32_t flags = c->cfg.decoders;
	uint32_t loud = b->loud_to > b->loud_from ? b->loud_to - b->loud_from : b->n;
	uint32_t loud_us = span_us(loud, b->rate_hz);
	bool cut = (b->flags & (BURST_F_TRUNCATED | BURST_F_CUT | BURST_F_DROPPED)) != 0U;
	uint32_t w = 0;

	if ((flags & PCAP_DEC_BLE) != 0U && dec_ble_rf_channel(b->freq_hz) >= 0 &&
	    loud_us >= BLE_US_MIN && (cut || loud_us <= BLE_US_MAX) &&
	    gate(b).narrow >= GATE_NARROW) {
		w |= WANT_BLE;
	}
	if ((flags & PCAP_DEC_WPAN) != 0U && dec_wpan_channel(b->freq_hz) != 0 &&
	    loud_us >= WPAN_US_MIN && (cut || loud_us <= WPAN_US_MAX)) {
		w |= WANT_WPAN;
	}
	return w;
}

static void decode(struct esdr_pcap *c)
{
	struct burst *b = c->b;
	const uint32_t flags = c->cfg.decoders;
	const uint32_t w = wants(c, b);
	struct gate g = gate(b);

	c->claimed = false;
	if ((w & WANT_BLE) != 0U) {
		struct dec_ble_cfg bc = {
			.max_errors = 2,
			.phy_2m = b->rate_hz == 4000000U && g.narrow < GATE_NARROW_2M_MAX,
			.skip_1m = b->rate_hz == 4000000U && g.narrow < GATE_NARROW_1M,
			.keep_bad = (flags & PCAP_DEC_BAD_CRC) != 0U,
		};

		bc.aa[bc.aa_count++] = (struct dec_ble_aa){.aa = DEC_BLE_ADV_AA,
							   .crc_init = DEC_BLE_ADV_CRC_INIT};
		if ((flags & PCAP_DEC_BLE_DATA) != 0U) {
			for (unsigned int k = 0; k < c->learnt_n && bc.aa_count < DEC_BLE_AA_MAX;
			     k++) {
				bc.aa[bc.aa_count++] = c->learnt_aa[k];
			}
		}
		uint32_t t0 = now(c);

		(void)dec_ble_run(b, &bc, c->scratch, c->scratch_len, ble_emit, c);
		c->st.cyc_ble += now(c) - t0;
	}
	if ((w & WANT_WPAN) != 0U) {
		const struct dec_wpan_cfg wc = {.keep_bad = (flags & PCAP_DEC_BAD_CRC) != 0U};
		uint32_t t0 = now(c);

		(void)dec_wpan_run(b, &wc, c->scratch, c->scratch_len, wpan_emit, c);
		c->st.cyc_wpan += now(c) - t0;
	}
	if (!c->claimed) {
		unclaimed(c);
	}
}

/* ---- the detector (apps/pcap/src/narrow.c) ---- */

/*
 * A closed burst no decoder takes (two threads only): one too short for a record goes, one that
 * fills the channel (a wideband record, never analysed) or one the generic analysis would see
 * while half the pool is in use goes to the decoders as its numbers alone. Its buffer stays with
 * the detector for the next burst. Returns whether it was dealt with.
 */
static bool triage(struct esdr_pcap *c, struct burst *b)
{
	uint32_t loud = b->loud_to > b->loud_from ? b->loud_to - b->loud_from : b->n;
	struct gate g;

	if ((c->cfg.decoders & PCAP_DEC_GENERIC) == 0U || span_us(loud, b->rate_hz) < MIN_BURST_US) {
		slot_keep(c, b);
		return true;
	}
	g = gate(b);
	if (!(g.snr >= GATE_SNR && g.narrow < GATE_WIDE) &&
	    2U * (q_len(&c->free_q) + c->own_n) >= c->cfg.pool) {
		return false;
	}
	if (!m_put(&c->meta_q, b)) {
		c->st.pool_full++;
	}
	slot_keep(c, b);
	return true;
}

static void close_burst(struct esdr_pcap *c, uint16_t flags)
{
	struct burst *b = c->cur;

	if (!c->open) {
		return;
	}
	c->open = false;
	c->cur = NULL;
	b->flags |= flags;
	if (flags == 0U && c->loud_blocks < NB_MIN_LOUD) {
		c->st.blips++;
		slot_keep(c, b);
		return;
	}
	c->st.bursts++;
	if ((flags & BURST_F_TRUNCATED) != 0U) {
		c->st.truncated++;
	}
	if (c->sync) {
		c->b = b;
		decode(c);
		slot_keep(c, b);
		return;
	}
	if (wants(c, b) == 0U && triage(c, b)) {
		return;
	}
	/* Never full: the pool has at most as many buffers as the queue has room. */
	(void)q_put(&c->closed_q, (uint8_t)(b - c->slot));
}

/* A new burst from the pre-roll on, which ends with the opening block (@p end: the index after
 * its last sample); none when the pool has no free buffer (counted). */
static void open_burst(struct esdr_pcap *c, int64_t end)
{
	struct burst *b = slot_take(c);
	uint32_t n = c->pre_n;
	uint32_t first = (c->pre_at + NB_PRE - n) % NB_PRE;
	uint32_t m = MIN_OF(n, NB_PRE - first);

	if (b == NULL) {
		c->st.pool_full++;
		c->skip = true;
		return;
	}
	b->chan = 0U;
	b->channels = 1U;
	b->rate_hz = c->cfg.rate_hz;
	b->freq_hz = c->cfg.freq_hz;
	b->floor = c->floor;
	b->power = 0.0f;
	b->flags = 0U;
	b->gain = c->cfg.gain;
	b->ref = 0U;
	b->wide_lo = 0U;
	b->wide_hi = 0U;
	b->gate_snr = 0.0f;
	b->gate_narrow = 0.0f;
	b->m0 = (uint64_t)(end - (int64_t)n);
	/* The pre-roll, oldest first. */
	memcpy(b->iq, &c->pre[first], 4U * m);
	memcpy(&b->iq[2U * m], c->pre, 4U * (n - m));
	b->n = n;
	b->loud_from = 0U;
	b->loud_to = 0U;
	c->cur = b;
	c->open = true;
	c->quiet = 0U;
	c->loud_set = false;
	c->loud_blocks = 0U;
}

/* The end of a detector block (@p end: the index after its last sample): learn, open,
 * follow, close. */
static void block_done(struct esdr_pcap *c, int64_t end)
{
	/* In two halves: a uint64_t to float is a library call on these CPUs. */
	float pw = ((float)(uint32_t)(c->acc >> 32) * 4294967296.0f + (float)(uint32_t)c->acc) *
		   (1.0f / NB_BLOCK);
	bool loud;

	c->acc = 0U;
	c->in_block = 0U;
	if (c->learnt < NB_LEARN) {
		/* Kept sorted as it fills: the lower quartile is then one read. */
		uint32_t j = c->learnt++;

		for (; j > 0U && c->learn[j - 1U] > pw; j--) {
			c->learn[j] = c->learn[j - 1U];
		}
		c->learn[j] = pw;
		if (c->learnt == NB_LEARN) {
			c->floor = MAX_OF(c->learn[NB_LEARN / 4U], 1.0f);
		}
		return;
	}
	loud = pw > c->floor * c->cfg.ratio;
	if (!loud) {
		c->floor += (pw - c->floor) / NB_FOLLOW;
	}
	if (c->skip) {
		/* Cut short or without a buffer: nothing until a quiet block. */
		c->skip = loud;
		return;
	}
	if (!c->open) {
		if (!loud) {
			return;
		}
		open_burst(c, end);
		if (!c->open) {
			return;
		}
	}
	if (loud) {
		struct burst *b = c->cur;

		c->quiet = 0U;
		c->loud_blocks++;
		if (!c->loud_set) {
			b->loud_from = b->n >= NB_BLOCK ? b->n - NB_BLOCK : 0U;
			c->loud_set = true;
		}
		b->loud_to = b->n;
		b->power = MAX_OF(b->power, pw);
	} else if (++c->quiet >= NB_HANG) {
		close_burst(c, 0U);
	}
}

_Static_assert((NB_PRE & (NB_PRE - 1U)) == 0U, "NB_PRE: a power of 2");

/*
 * @p m samples (at most the rest of a block) into the pre-roll, the open burst and the block's
 * power. A sample is one word (iq 4-byte aligned), shifted whole to full scale 16384: the bits
 * of I that cross into Q's half are cleared, each half wraps as an int16 would. The whole
 * detector, measured on the S3 (CPU 0, 240 MHz): 35 cycles a sample, 43 with an int16 loop, a
 * shift pass and memcpy() calls.
 */
static void take(struct esdr_pcap *c, const int16_t *iq, uint32_t m)
{
	const uint32_t *w = (const uint32_t *)(const void *)iq;
	const uint32_t s = c->cfg.shift;
	const uint32_t keep = ~((((uint32_t)1U << s) - 1U) << 16);
	uint32_t at = c->pre_at;
	uint64_t acc = c->acc;

	for (uint32_t k = 0; k < m; k++) {
		const uint32_t v = (w[k] << s) & keep;
		const int32_t i = (int16_t)(v & 0xFFFFU), q = (int16_t)(v >> 16);

		c->pre[(at + k) & (NB_PRE - 1U)] = v;
		acc += (uint32_t)(i * i) + (uint32_t)(q * q);
	}
	c->acc = acc;
	c->pre_at = (at + m) & (NB_PRE - 1U);
	c->pre_n = MIN_OF(c->pre_n + m, NB_PRE);
	if (c->open) {
		struct burst *b = c->cur;
		uint32_t room = MIN_OF(m, c->cfg.burst_max - b->n);
		uint32_t *dst = (uint32_t *)(void *)&b->iq[2U * b->n];

		for (uint32_t k = 0; k < room; k++) {
			dst[k] = c->pre[(at + k) & (NB_PRE - 1U)];
		}
		b->n += room;
		if (b->n >= c->cfg.burst_max) {
			close_burst(c, BURST_F_TRUNCATED);
			c->skip = true;
		}
	}
	c->in_block += m;
}

static void call_start(struct esdr_pcap *c, uint32_t analyse)
{
	c->out_fill = 0U;
	c->out_n = 0U;
	c->analyse_left = analyse;
}

static void detect(struct esdr_pcap *c, const int16_t *iq, uint32_t n, int64_t at)
{
	uint32_t k = 0;

	if (c->have_next && at != c->next) {
		/* A gap: what was open ends, the pre-roll and the block start again. */
		c->st.gaps++;
		close_burst(c, BURST_F_DROPPED);
		c->pre_n = 0U;
		c->in_block = 0U;
		c->acc = 0U;
		c->skip = false;
	}
	while (k < n) {
		uint32_t m = MIN_OF(NB_BLOCK - c->in_block, n - k);

		take(c, iq + 2U * k, m);
		k += m;
		if (c->in_block == NB_BLOCK) {
			block_done(c, at + (int64_t)k);
		}
	}
	c->st.samples += n;
	c->next = at + (int64_t)n;
	c->have_next = true;
}

uint32_t esdr_pcap_feed(struct esdr_pcap *c, const int16_t *iq, uint32_t n, int64_t at,
			uint32_t analyse)
{
	call_start(c, analyse);
	c->sync = true;
	detect(c, iq, n, at);
	return c->out_n;
}

void esdr_pcap_detect(struct esdr_pcap *c, const int16_t *iq, uint32_t n, int64_t at)
{
	c->sync = false;
	detect(c, iq, n, at);
}

void esdr_pcap_end(struct esdr_pcap *c)
{
	c->sync = false;
	close_burst(c, BURST_F_CUT);
}

/* Metadata records a call makes at most (each some 70 bytes of its output). */
#define META_CALL (OUT_MAX / 2U)

static void decode_meta(struct esdr_pcap *c)
{
	uint32_t keep = c->analyse_left;
	struct burst m;

	/* Records of their numbers only: the samples are gone. */
	c->analyse_left = 0U;
	for (uint32_t k = 0; k < META_CALL && m_get(&c->meta_q, &m); k++) {
		c->b = &m;
		c->claimed = false;
		unclaimed(c);
	}
	c->analyse_left = keep;
}

static void decode_queued(struct esdr_pcap *c, uint32_t max)
{
	uint8_t k;

	decode_meta(c);
	for (uint32_t done = 0; done < max && q_get(&c->closed_q, &k); done++) {
		c->b = &c->slot[k];
		decode(c);
		slot_free(c, c->b);
	}
}

uint32_t esdr_pcap_decode(struct esdr_pcap *c, uint32_t max, uint32_t analyse)
{
	call_start(c, analyse);
	decode_queued(c, max);
	return c->out_n;
}

uint32_t esdr_pcap_analyses_left(const struct esdr_pcap *c)
{
	return c->analyse_left;
}

uint32_t esdr_pcap_pending(const struct esdr_pcap *c)
{
	return q_len(&c->closed_q) + m_len(&c->meta_q);
}

uint32_t esdr_pcap_flush(struct esdr_pcap *c, uint32_t analyse)
{
	call_start(c, analyse);
	/* The detector has stopped: this thread is both ends now. */
	decode_queued(c, ESDR_PCAP_POOL_MAX);
	decode_meta(c);
	c->sync = true;
	close_burst(c, BURST_F_CUT);
	return c->out_n;
}

/* ---- pcapng, the interfaces of apps/pcap/src/output.c ---- */

static const struct pcapng_idb idbs[PCAP_IF_COUNT] = {
	[PCAP_IF_BLE] = {.linktype = PCAP_LINKTYPE_BLE_LL_WITH_PHDR, .name = "esdr-ble",
			 .description = "Bluetooth LE link layer, decoded on the board",
			 .tsresol = 9},
	[PCAP_IF_WPAN] = {.linktype = PCAP_LINKTYPE_IEEE802_15_4_TAP, .name = "esdr-wpan",
			  .description = "IEEE 802.15.4 O-QPSK 2.4 GHz frames, decoded on the board",
			  .tsresol = 9},
	[PCAP_IF_BURST] = {.linktype = PCAP_LINKTYPE_USER0, .name = "esdr-burst",
			   .description = "Bursts no decoder claimed, with their metadata "
					  "(apps/pcap/tools/esdr_burst.lua)",
			   .tsresol = 9},
};

int esdr_pcap_head(uint8_t *buf, size_t cap, const char *hardware, const char *os,
		   const char *appl)
{
	int n = pcapng_shb(buf, cap, hardware, os, appl);
	size_t len;

	if (n < 0) {
		return n;
	}
	len = (size_t)n;
	for (unsigned int k = 0; k < PCAP_IF_COUNT; k++) {
		n = pcapng_idb(buf + len, cap - len, &idbs[k]);
		if (n < 0) {
			return n;
		}
		len += (size_t)n;
	}
	return (int)len;
}

int esdr_pcap_epb(uint8_t *buf, size_t cap, uint32_t iface, uint64_t t_ns, const void *rec,
		  uint32_t len, const char *comment)
{
	const struct pcapng_epb e = {
		.interface = iface,
		.timestamp = t_ns,
		.data = rec,
		.len = len,
		.comment = comment,
		.flags = PCAPNG_EPB_FLAG_INBOUND,
		.dropcount = UINT64_MAX,
	};

	if (iface >= PCAP_IF_COUNT) {
		return -EINVAL;
	}
	return pcapng_epb(buf, cap, &e);
}

size_t esdr_pcap_epb_size(uint32_t len, const char *comment)
{
	return pcapng_epb_size(len, comment != NULL ? strlen(comment) : 0U, 1, 0);
}
