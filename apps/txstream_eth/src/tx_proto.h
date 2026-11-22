/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, network protocol messages on UDP port 4992 (doc/dev/txs.md, "Network
 * protocol"). Plain C, header only, no Zephyr calls, no libc beyond string.h: the board and the
 * host tool (tools/vrt_tx.c) include this one file. Everything is big endian. Every put/get
 * checks the buffer: put returns the message length or -1 (buffer too small), get returns 0 or
 * -1 (wrong length or magic). A get does not validate values: use the txp_*_valid() helpers.
 *
 * All of these come after the cookie handshake of apps/stream_eth/src/ctlproto.h (VRTS/VRTC/VRTK).
 * The ones the host sends carry the 8 byte cookie and are honoured only from the subscriber.
 *
 *   TXGO  host -> board  24 B  "TXGO" cookie[8] rate u8 fmt u8 flags u16 seconds u32 nonce u32
 *   TXAK  board -> host  28 B  "TXAK" nonce u32 code u16 flags u16 stream_id u32 max_words u32
 *                              depth u32 group u32
 *   TXEN  host -> board  16 B  "TXEN" cookie[8] nonce u32      (drain and stop; answered by TXAK)
 *   TXPG  host -> board  24 B  "TXPG" cookie[8] seq u32 t_ns u64   (ping)
 *   TXPO  board -> host  24 B  "TXPO" seq u32 t_ns u64 b_ns u64   (the echo of the ping, and the
 *                              board's free running clock in ns when it arrived)
 *   TXST  board -> host  76 B  "TXST" seq u32 status[64] flags u32
 */

#ifndef TX_PROTO_H_
#define TX_PROTO_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TXP_COOKIE_BYTES 8U

#define TXP_GO_BYTES 24U
#define TXP_AK_BYTES 28U
#define TXP_EN_BYTES 16U
#define TXP_PG_BYTES 24U
#define TXP_PO_BYTES 24U
#define TXP_ST_BYTES 76U
#define TXP_STATUS_BYTES 64U

/* TXGO */
#define TXP_FMT_IQ8   0U
#define TXP_FMT_WORD32 1U
#define TXP_GO_RF     0x0001U /* ask for RF on air (the board may refuse: TXP_RF_REFUSED) */
#define TXP_GO_VERIFY 0x0002U /* the board reads back and checks the bank (CRC) */
#define TXP_GO_FLAGS  (TXP_GO_RF | TXP_GO_VERIFY)

/* TXAK code */
enum txp_code {
	TXP_OK = 0,
	TXP_BUSY = 1,       /* a run is going on (or ended and not reset) */
	TXP_BAD_RATE = 2,   /* rate is not 20 or 40 */
	TXP_BAD_FMT = 3,
	TXP_BAD_FLAGS = 4,  /* flags outside TXP_GO_FLAGS */
	TXP_RF_REFUSED = 5, /* not armed on the UART, or txs_rf_ok() says no */
	TXP_NOT_SUB = 6,    /* the sender is not the subscriber */
	TXP_NO_RESOURCE = 7,
};
/* TXAK flags */
#define TXP_AK_RF 0x0001U /* RF granted */

/* TXST flags */
#define TXP_ST_RUNNING 0x0001U /* the engine plays: play advances at the sample rate */
#define TXP_ST_ENDED   0x0002U /* the run is over (stopped, drained, hang, deadman) */
#define TXP_ST_RF      0x0004U /* RF is on */

struct txp_go {
	uint8_t cookie[TXP_COOKIE_BYTES];
	uint8_t rate_msps; /* 20 or 40 */
	uint8_t fmt;       /* TXP_FMT_* */
	uint16_t flags;
	uint32_t seconds;  /* cap on the run, 0: none */
	uint32_t nonce;    /* echoed in TXAK, so a retried TXGO is matched */
};

struct txp_ak {
	uint32_t nonce;
	uint16_t code;
	uint16_t flags;
	uint32_t stream_id; /* the session's random 32 bit value, in the VITA header */
	uint32_t max_words; /* most DAC words in a data packet */
	uint32_t depth;     /* credit depth, words */
	uint32_t group;     /* words in a window (32768) */
};

struct txp_en {
	uint8_t cookie[TXP_COOKIE_BYTES];
	uint32_t nonce;
};

struct txp_ping {
	uint8_t cookie[TXP_COOKIE_BYTES]; /* unused (zero) in a TXPO */
	uint32_t seq;
	uint64_t t_ns; /* the host's clock, echoed untouched */
	uint64_t b_ns; /* TXPO only: the board's clock (any epoch, free running, ns) at the arrival */
};

/* The same fields and byte layout as struct txs_status / txs_status_put() (lib/esp_sdr/src/txs/txs_credit.h). */
struct txp_status {
	uint32_t session;
	uint64_t play;    /* the word the engine plays when the record is made */
	uint64_t contig;  /* first word neither written nor zeroed */
	uint64_t credit;  /* the sender may send words below this index */
	uint64_t restart; /* where a sender that lost its place goes on */
	uint32_t underruns;
	uint64_t lost, late, stale;
};

struct txp_st {
	uint32_t seq;
	struct txp_status status;
	uint32_t flags;
};

static inline void txp_put16(uint8_t *o, uint16_t v)
{
	o[0] = (uint8_t)(v >> 8);
	o[1] = (uint8_t)v;
}

static inline void txp_put32(uint8_t *o, uint32_t v)
{
	o[0] = (uint8_t)(v >> 24);
	o[1] = (uint8_t)(v >> 16);
	o[2] = (uint8_t)(v >> 8);
	o[3] = (uint8_t)v;
}

static inline void txp_put64(uint8_t *o, uint64_t v)
{
	txp_put32(o, (uint32_t)(v >> 32));
	txp_put32(o + 4, (uint32_t)v);
}

static inline uint16_t txp_get16(const uint8_t *i)
{
	return (uint16_t)(((uint16_t)i[0] << 8) | i[1]);
}

static inline uint32_t txp_get32(const uint8_t *i)
{
	return ((uint32_t)i[0] << 24) | ((uint32_t)i[1] << 16) | ((uint32_t)i[2] << 8) | i[3];
}

static inline uint64_t txp_get64(const uint8_t *i)
{
	return ((uint64_t)txp_get32(i) << 32) | txp_get32(i + 4);
}

/** The message kind of a datagram: its length and magic (0 if none): 'G','A','E','P','O','S'. */
static inline char txp_kind(const uint8_t *d, size_t len)
{
	static const struct {
		const char *m;
		size_t len;
		char k;
	} t[] = {{"TXGO", TXP_GO_BYTES, 'G'}, {"TXAK", TXP_AK_BYTES, 'A'}, {"TXEN", TXP_EN_BYTES, 'E'},
		 {"TXPG", TXP_PG_BYTES, 'P'}, {"TXPO", TXP_PO_BYTES, 'O'}, {"TXST", TXP_ST_BYTES, 'S'}};

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (len == t[i].len && memcmp(d, t[i].m, 4) == 0) {
			return t[i].k;
		}
	}
	return 0;
}

static inline int txp_go_put(uint8_t *buf, size_t cap, const struct txp_go *m)
{
	if (cap < TXP_GO_BYTES) {
		return -1;
	}
	memcpy(buf, "TXGO", 4);
	memcpy(buf + 4, m->cookie, TXP_COOKIE_BYTES);
	buf[12] = m->rate_msps;
	buf[13] = m->fmt;
	txp_put16(buf + 14, m->flags);
	txp_put32(buf + 16, m->seconds);
	txp_put32(buf + 20, m->nonce);
	return (int)TXP_GO_BYTES;
}

static inline int txp_go_get(struct txp_go *m, const uint8_t *buf, size_t len)
{
	if (len != TXP_GO_BYTES || memcmp(buf, "TXGO", 4) != 0) {
		return -1;
	}
	memcpy(m->cookie, buf + 4, TXP_COOKIE_BYTES);
	m->rate_msps = buf[12];
	m->fmt = buf[13];
	m->flags = txp_get16(buf + 14);
	m->seconds = txp_get32(buf + 16);
	m->nonce = txp_get32(buf + 20);
	return 0;
}

/** The TXAK code a TXGO's values deserve before the board looks at anything else (TXP_OK: fine). */
static inline enum txp_code txp_go_valid(const struct txp_go *m)
{
	if (m->rate_msps != 20U && m->rate_msps != 40U) {
		return TXP_BAD_RATE;
	}
	if (m->fmt != TXP_FMT_IQ8 && m->fmt != TXP_FMT_WORD32) {
		return TXP_BAD_FMT;
	}
	if ((m->flags & ~TXP_GO_FLAGS) != 0U) {
		return TXP_BAD_FLAGS;
	}
	return TXP_OK;
}

static inline int txp_ak_put(uint8_t *buf, size_t cap, const struct txp_ak *m)
{
	if (cap < TXP_AK_BYTES) {
		return -1;
	}
	memcpy(buf, "TXAK", 4);
	txp_put32(buf + 4, m->nonce);
	txp_put16(buf + 8, m->code);
	txp_put16(buf + 10, m->flags);
	txp_put32(buf + 12, m->stream_id);
	txp_put32(buf + 16, m->max_words);
	txp_put32(buf + 20, m->depth);
	txp_put32(buf + 24, m->group);
	return (int)TXP_AK_BYTES;
}

static inline int txp_ak_get(struct txp_ak *m, const uint8_t *buf, size_t len)
{
	if (len != TXP_AK_BYTES || memcmp(buf, "TXAK", 4) != 0) {
		return -1;
	}
	m->nonce = txp_get32(buf + 4);
	m->code = txp_get16(buf + 8);
	m->flags = txp_get16(buf + 10);
	m->stream_id = txp_get32(buf + 12);
	m->max_words = txp_get32(buf + 16);
	m->depth = txp_get32(buf + 20);
	m->group = txp_get32(buf + 24);
	return 0;
}

static inline int txp_en_put(uint8_t *buf, size_t cap, const struct txp_en *m)
{
	if (cap < TXP_EN_BYTES) {
		return -1;
	}
	memcpy(buf, "TXEN", 4);
	memcpy(buf + 4, m->cookie, TXP_COOKIE_BYTES);
	txp_put32(buf + 12, m->nonce);
	return (int)TXP_EN_BYTES;
}

static inline int txp_en_get(struct txp_en *m, const uint8_t *buf, size_t len)
{
	if (len != TXP_EN_BYTES || memcmp(buf, "TXEN", 4) != 0) {
		return -1;
	}
	memcpy(m->cookie, buf + 4, TXP_COOKIE_BYTES);
	m->nonce = txp_get32(buf + 12);
	return 0;
}

static inline int txp_pg_put(uint8_t *buf, size_t cap, const struct txp_ping *m)
{
	if (cap < TXP_PG_BYTES) {
		return -1;
	}
	memcpy(buf, "TXPG", 4);
	memcpy(buf + 4, m->cookie, TXP_COOKIE_BYTES);
	txp_put32(buf + 12, m->seq);
	txp_put64(buf + 16, m->t_ns);
	return (int)TXP_PG_BYTES;
}

static inline int txp_pg_get(struct txp_ping *m, const uint8_t *buf, size_t len)
{
	if (len != TXP_PG_BYTES || memcmp(buf, "TXPG", 4) != 0) {
		return -1;
	}
	memcpy(m->cookie, buf + 4, TXP_COOKIE_BYTES);
	m->seq = txp_get32(buf + 12);
	m->t_ns = txp_get64(buf + 16);
	m->b_ns = 0;
	return 0;
}

static inline int txp_po_put(uint8_t *buf, size_t cap, const struct txp_ping *m)
{
	if (cap < TXP_PO_BYTES) {
		return -1;
	}
	memcpy(buf, "TXPO", 4);
	txp_put32(buf + 4, m->seq);
	txp_put64(buf + 8, m->t_ns);
	txp_put64(buf + 16, m->b_ns);
	return (int)TXP_PO_BYTES;
}

static inline int txp_po_get(struct txp_ping *m, const uint8_t *buf, size_t len)
{
	if (len != TXP_PO_BYTES || memcmp(buf, "TXPO", 4) != 0) {
		return -1;
	}
	memset(m->cookie, 0, TXP_COOKIE_BYTES);
	m->seq = txp_get32(buf + 4);
	m->t_ns = txp_get64(buf + 8);
	m->b_ns = txp_get64(buf + 16);
	return 0;
}

static inline void txp_status_put(uint8_t *o, const struct txp_status *s)
{
	txp_put32(o, s->session);
	txp_put64(o + 4, s->play);
	txp_put64(o + 12, s->contig);
	txp_put64(o + 20, s->credit);
	txp_put64(o + 28, s->restart);
	txp_put32(o + 36, s->underruns);
	txp_put64(o + 40, s->lost);
	txp_put64(o + 48, s->late);
	txp_put64(o + 56, s->stale);
}

static inline void txp_status_get(struct txp_status *s, const uint8_t *i)
{
	s->session = txp_get32(i);
	s->play = txp_get64(i + 4);
	s->contig = txp_get64(i + 12);
	s->credit = txp_get64(i + 20);
	s->restart = txp_get64(i + 28);
	s->underruns = txp_get32(i + 36);
	s->lost = txp_get64(i + 40);
	s->late = txp_get64(i + 48);
	s->stale = txp_get64(i + 56);
}

static inline int txp_st_put(uint8_t *buf, size_t cap, const struct txp_st *m)
{
	if (cap < TXP_ST_BYTES) {
		return -1;
	}
	memcpy(buf, "TXST", 4);
	txp_put32(buf + 4, m->seq);
	txp_status_put(buf + 8, &m->status);
	txp_put32(buf + 72, m->flags);
	return (int)TXP_ST_BYTES;
}

static inline int txp_st_get(struct txp_st *m, const uint8_t *buf, size_t len)
{
	if (len != TXP_ST_BYTES || memcmp(buf, "TXST", 4) != 0) {
		return -1;
	}
	m->seq = txp_get32(buf + 4);
	txp_status_get(&m->status, buf + 8);
	m->flags = txp_get32(buf + 72);
	return 0;
}

#endif /* TX_PROTO_H_ */
