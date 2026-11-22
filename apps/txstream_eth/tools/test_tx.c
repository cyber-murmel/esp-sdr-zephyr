// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Unit test of vrt_tx's parts, native, no network: the messages of tx_proto.h (round trips,
 * bounds, byte layout, against the library's own status record), the tone source, the sliding
 * minimum, and the sender's pacing and credit logic (tx_pace.h) against a simulated board
 * (tx_board.h) on a virtual clock: a play index at 20 or 40 MS/s, a status every 250 us and on
 * every flip, a one way delay of 100 to 600 us with jitter in both directions, the credit rule
 * of doc/dev/txs.md, a board stall of 5 ms and a stall of the sender of 50 ms.
 * Build and run: make -C apps/txstream_eth/tools test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tx_board.h"
#include "tx_core.h"
#include "tx_pace.h"
#include "tx_proto.h"
#include "tx_src.h"
#include "txs_credit.h"
#include "txs_expand.h"
#include "txs_wire.h"

static int fails, checks;

#define CHECK(c)                                                                               \
	do {                                                                                   \
		checks++;                                                                      \
		if (!(c)) {                                                                    \
			fails++;                                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                    \
		}                                                                              \
	} while (0)

/* ---- messages ---- */

static void test_proto(void)
{
	uint8_t b[128], big[128];
	struct txp_go go = {.cookie = {1, 2, 3, 4, 5, 6, 7, 8}, .rate_msps = 40, .fmt = 1, .flags = 3,
			    .seconds = 0x01020304U, .nonce = 0xdeadbeefU}, go2;
	struct txp_ak ak = {.nonce = 0xdeadbeefU, .code = 5, .flags = 1, .stream_id = 0x11223344U,
			    .max_words = 702, .depth = 11200, .group = 32768}, ak2;
	struct txp_en en = {.cookie = {9, 8, 7, 6, 5, 4, 3, 2}, .nonce = 77}, en2;
	struct txp_ping pg = {.cookie = {1, 1, 2, 3, 5, 8, 13, 21}, .seq = 5, .t_ns = 0x0102030405060708ULL}, pg2;
	struct txp_st st = {.seq = 9, .flags = 5}, st2;

	memset(big, 0xa5, sizeof(big));
	/* TXGO: byte layout and round trip */
	CHECK(txp_go_put(b, sizeof(b), &go) == 24);
	CHECK(memcmp(b, "TXGO", 4) == 0 && b[4] == 1 && b[11] == 8 && b[12] == 40 && b[13] == 1);
	CHECK(b[14] == 0 && b[15] == 3 && b[16] == 1 && b[19] == 4 && b[20] == 0xde && b[23] == 0xef);
	CHECK(txp_go_get(&go2, b, 24) == 0 && memcmp(&go, &go2, sizeof(go)) == 0);
	CHECK(txp_go_put(b, 23, &go) == -1 && txp_go_put(b, 0, &go) == -1);
	CHECK(txp_go_get(&go2, b, 23) == -1 && txp_go_get(&go2, b, 25) == -1 && txp_go_get(&go2, b, 0) == -1);
	b[0] = 'Y';
	CHECK(txp_go_get(&go2, b, 24) == -1);
	CHECK(txp_go_valid(&go) == TXP_OK);
	go.rate_msps = 30;
	CHECK(txp_go_valid(&go) == TXP_BAD_RATE);
	go.rate_msps = 20;
	go.fmt = 2;
	CHECK(txp_go_valid(&go) == TXP_BAD_FMT);
	go.fmt = 0;
	go.flags = 4;
	CHECK(txp_go_valid(&go) == TXP_BAD_FLAGS);
	/* TXAK */
	CHECK(txp_ak_put(b, sizeof(b), &ak) == 28);
	CHECK(b[8] == 0 && b[9] == 5 && b[11] == 1 && b[12] == 0x11 && b[15] == 0x44 && b[18] == 0x02 && b[19] == 0xbe);
	CHECK(b[22] == 0x2b && b[23] == 0xc0 && b[26] == 0x80 && b[27] == 0);
	CHECK(txp_ak_get(&ak2, b, 28) == 0 && memcmp(&ak, &ak2, sizeof(ak)) == 0);
	CHECK(txp_ak_put(b, 27, &ak) == -1 && txp_ak_get(&ak2, b, 27) == -1 && txp_ak_get(&ak2, b, 29) == -1);
	/* TXEN */
	CHECK(txp_en_put(b, sizeof(b), &en) == 16 && txp_en_get(&en2, b, 16) == 0 && memcmp(&en, &en2, sizeof(en)) == 0);
	CHECK(txp_en_put(b, 15, &en) == -1 && txp_en_get(&en2, b, 15) == -1 && txp_en_get(&en2, b, 17) == -1);
	/* TXPG, TXPO */
	CHECK(txp_pg_put(b, sizeof(b), &pg) == 24 && txp_pg_get(&pg2, b, 24) == 0 && memcmp(&pg, &pg2, sizeof(pg)) == 0);
	CHECK(b[16] == 1 && b[23] == 8);
	CHECK(txp_pg_put(b, 23, &pg) == -1 && txp_pg_get(&pg2, b, 23) == -1);
	pg.b_ns = 0x1112131415161718ULL;
	CHECK(txp_po_put(b, sizeof(b), &pg) == 24 && txp_po_get(&pg2, b, 24) == 0);
	CHECK(pg2.seq == 5 && pg2.t_ns == pg.t_ns && pg2.b_ns == pg.b_ns && b[8] == 1 && b[15] == 8 && b[16] == 0x11 && b[23] == 0x18);
	CHECK(txp_po_put(b, 23, &pg) == -1 && txp_po_get(&pg2, b, 23) == -1 && txp_po_get(&pg2, b, 25) == -1);
	/* TXST, and the status against the library's record */
	st.status = (struct txp_status){.session = 1, .play = 0x0102030405060708ULL, .contig = 2, .credit = 3,
					.restart = 4, .underruns = 5, .lost = 6, .late = 7, .stale = 8};
	CHECK(txp_st_put(b, sizeof(b), &st) == 76 && txp_st_get(&st2, b, 76) == 0 && memcmp(&st, &st2, sizeof(st)) == 0);
	CHECK(memcmp(b, "TXST", 4) == 0 && b[7] == 9 && b[8 + 4] == 1 && b[8 + 11] == 8 && b[75] == 5);
	CHECK(txp_st_put(b, 75, &st) == -1 && txp_st_get(&st2, b, 75) == -1 && txp_st_get(&st2, b, 77) == -1);
	{
		struct txs_status ls = {.session = 1, .play = 0x0102030405060708ULL, .contig = 2, .credit = 3,
					.restart = 4, .underruns = 5, .lost = 6, .late = 7, .stale = 8};
		uint8_t lb[TXS_STATUS_BYTES];

		txs_status_put(lb, &ls);
		CHECK(memcmp(lb, b + 8, 64) == 0);
	}
	/* put never writes past cap: the guard bytes of big stay */
	memset(big, 0xa5, sizeof(big));
	CHECK(txp_st_put(big, 76, &st) == 76 && big[76] == 0xa5 && big[127] == 0xa5);
	CHECK(txp_kind(b, 76) == 'S' && txp_kind(b, 75) == 0);
	memcpy(b, "TXAK", 4);
	CHECK(txp_kind(b, 28) == 'A' && txp_kind(b, 24) == 0);
	/* the data header of vrt_tx is the library's */
	{
		struct txs_wire_cfg wc = {.stream_id = 0xabcd1234U, .port = 4994, .fmt = TXS_FMT_IQ8, .max_words = 702};
		uint8_t pkt[1420] __attribute__((aligned(4)));
		struct txs_wire_pkt pk;

		txs_wire_header(pkt, &wc, 3, 0x123456789ULL, 702);
		CHECK(txs_wire_parse(&wc, 4994, pkt, 1420, &pk) == TXS_WIRE_OK && pk.index == 0x123456789ULL && pk.words == 702);
		CHECK(txp_get32(pkt) == (0x10100000U | (3U << 16) | 355U) && txp_get64(pkt + 8) == 0x123456789ULL);
	}
}

/* ---- the tone ---- */

static void test_tone(void)
{
	struct txsrc_tone t;
	static uint8_t a[2 * 4000], c[2 * 4000], w[4 * 700];
	int peak;

	txsrc_tone_init(&t, 20000000U, 1000000, 100);
	txsrc_tone_fill(&t, 0, 4000, a, TXP_FMT_IQ8);
	/* continuity: any split gives the same samples; at a distant index too */
	txsrc_tone_fill(&t, 1234567890123ULL, 1000, c, TXP_FMT_IQ8);
	txsrc_tone_fill(&t, 1234567890123ULL + 1000, 3000, c + 2000, TXP_FMT_IQ8);
	{
		static uint8_t d[2 * 4000];

		txsrc_tone_fill(&t, 1234567890123ULL, 4000, d, TXP_FMT_IQ8);
		CHECK(memcmp(c, d, sizeof(d)) == 0);
	}
	/* 1 MHz at 20 MS/s: 20 samples a turn, the amplitude as asked, I = cos, Q = sin */
	CHECK(memcmp(a, a + 40, 2 * 3000) == 0);
	CHECK(txsrc_peak(a, 4000, TXP_FMT_IQ8) == 100);
	CHECK((int8_t)a[0] == 100 && (int8_t)a[1] == 0);
	CHECK((int8_t)a[2 * 5] == 0 && (int8_t)a[2 * 5 + 1] == 100); /* a quarter turn */
	/* a negative frequency turns the other way */
	txsrc_tone_init(&t, 20000000U, -1000000, 100);
	txsrc_tone_fill(&t, 0, 40, a, TXP_FMT_IQ8);
	CHECK((int8_t)a[2 * 5 + 1] == -100);
	/* amp 1 is 0 or +-1 only */
	txsrc_tone_init(&t, 40000000U, 123000, 1);
	txsrc_tone_fill(&t, 77, 700, a, TXP_FMT_IQ8);
	CHECK(txsrc_peak(a, 700, TXP_FMT_IQ8) == 1);
	/* word32: the DAC word of the same samples, as the board's kernels make it from iq8 */
	txsrc_tone_fill(&t, 77, 700, w, TXP_FMT_WORD32);
	for (uint32_t k = 0; k < 700; k++) {
		uint32_t x = a[2 * k] | ((uint32_t)a[2 * k + 1] << 8);
		uint32_t got = w[4 * k] | ((uint32_t)w[4 * k + 1] << 8) | ((uint32_t)w[4 * k + 2] << 16) |
			       ((uint32_t)w[4 * k + 3] << 24);

		if (got != txs_iq8_word(x)) {
			CHECK(got == txs_iq8_word(x));
			break;
		}
	}
	peak = txsrc_peak(w, 700, TXP_FMT_WORD32);
	CHECK(peak == 1);
}

/* ---- the sliding minimum ---- */

static void test_minq(void)
{
	static struct txp_minq q;
	static int64_t tv[20000], vv[20000];
	uint64_t s = 12345;

	txp_minq_reset(&q);
	for (int i = 0; i < 20000; i++) {
		int64_t v, ref = INT64_MAX;

		s = s * 6364136223846793005ULL + 1442695040888963407ULL;
		tv[i] = (int64_t)i * 250000;
		vv[i] = (int64_t)(s >> 40) % 1000000 - 500000 + (i % 7000) * 10;
		txp_minq_push(&q, tv[i], vv[i], TXP_EST_WIN_NS);
		for (int j = i; j >= 0 && tv[j] >= tv[i] - TXP_EST_WIN_NS; j--) {
			if (vv[j] < ref) {
				ref = vv[j];
			}
		}
		CHECK(txp_minq_get(&q, &v) && v == ref);
		if (fails != 0) {
			return;
		}
	}
}


/* ---- the session core, driven by hand ---- */

static void test_core(void)
{
	struct txc c;
	uint8_t o[64], rx[128];
	int64_t t = 1000000000LL;
	int n, pings = 0, gos = 0;
	struct txp_ak ak = {.code = TXP_OK, .stream_id = 0xfeed0001U, .max_words = 702, .depth = 11200, .group = 32768};
	struct txp_st st = {0};
	struct txp_plan pl;

	txc_init(&c, 20, TXP_FMT_IQ8, 0, 5, 77, 0, 0);
	CHECK(c.state == TXC_HELLO && c.pkt_words == 702);
	n = txc_poll(&c, t, o, sizeof(o));
	CHECK(n == 4 && memcmp(o, "VRTS", 4) == 0);
	CHECK(txc_poll(&c, t, o, sizeof(o)) == 0);
	memcpy(rx, "VRTC\x01\x02\x03\x04\x05\x06\x07\x08", 12);
	txc_rx(&c, t, rx, 12);
	CHECK(c.state == TXC_SUB);
	n = txc_poll(&c, t, o, sizeof(o));
	CHECK(n == 12 && memcmp(o, "VRTS", 4) == 0 && o[4] == 1 && o[11] == 8);
	/* pings every 5 ms, each answered at once; TXGO after 100 pongs */
	while (c.state == TXC_SUB || c.state == TXC_GO) {
		t += 1000000;
		while ((n = txc_poll(&c, t, o, sizeof(o))) > 0) {
			if (txp_kind(o, (size_t)n) == 'P') {
				struct txp_ping pg;

				txp_pg_get(&pg, o, (size_t)n);
				pg.b_ns = (uint64_t)t + 50000;
				txp_po_put(rx, sizeof(rx), &pg);
				txc_rx(&c, t + 100000, rx, TXP_PO_BYTES);
				pings++;
			} else if (txp_kind(o, (size_t)n) == 'G') {
				struct txp_go go;

				CHECK(txp_go_get(&go, o, (size_t)n) == 0 && go.nonce == 77 && go.rate_msps == 20 &&
				      go.seconds == 5 && memcmp(go.cookie, c.cookie, 8) == 0);
				gos++;
				break;
			}
		}
		if (gos != 0) {
			break;
		}
	}
	CHECK(pings >= 100 && gos == 1 && c.state == TXC_GO);
	/* a TXAK with another nonce is ignored, the right one starts the run */
	ak.nonce = 5;
	txp_ak_put(rx, sizeof(rx), &ak);
	CHECK(txc_rx(&c, t, rx, TXP_AK_BYTES) == 0 && c.state == TXC_GO);
	ak.nonce = 77;
	txp_ak_put(rx, sizeof(rx), &ak);
	CHECK((txc_rx(&c, t, rx, TXP_AK_BYTES) & TXC_EV_ACK) && c.state == TXC_RUN && c.pace.depth == 11200);
	/* before a status there is nothing to send; then the first window, up to the credit */
	CHECK(txc_plan(&c, t, 6, &pl) == 0);
	st.flags = 0;
	st.status.restart = 3510;
	st.status.credit = 3510 + 32768;
	txp_st_put(rx, sizeof(rx), &st);
	CHECK((txc_rx(&c, t, rx, TXP_ST_BYTES) & TXC_EV_STATUS));
	CHECK(txc_plan(&c, t, 16, &pl) == TXP_BURST && pl.first == 3510);
	{
		uint8_t pkt[1420];
		struct txs_wire_cfg wc = {.stream_id = 0xfeed0001U, .port = 4994, .fmt = TXS_FMT_IQ8, .max_words = 702};
		struct txs_wire_pkt pk;

		memset(pkt + 16, 0, 1404);
		txc_header(&c, pkt, 3510);
		CHECK(txc_packet_bytes(&c) == 1420);
		CHECK(txs_wire_parse(&wc, 4994, pkt, 1420, &pk) == TXS_WIRE_OK && pk.index == 3510 && pk.count == 0);
	}
	/* stop: TXEN, then ENDED ends it */
	txc_stop(&c);
	n = txc_poll(&c, t, o, sizeof(o));
	CHECK(n == 16 && memcmp(o, "TXEN", 4) == 0 && c.state == TXC_END);
	st.flags = TXP_ST_ENDED;
	txp_st_put(rx, sizeof(rx), &st);
	CHECK((txc_rx(&c, t, rx, TXP_ST_BYTES) & TXC_EV_ENDED) && c.state == TXC_DONE);
	/* a refused TXGO and another host */
	txc_init(&c, 40, TXP_FMT_WORD32, TXP_GO_RF, 0, 9, 0, 0);
	CHECK(c.pkt_words == 351);
	c.state = TXC_GO;
	ak.nonce = 9;
	ak.code = TXP_RF_REFUSED;
	txp_ak_put(rx, sizeof(rx), &ak);
	CHECK((txc_rx(&c, t, rx, TXP_AK_BYTES) & TXC_EV_FAIL) && c.state == TXC_FAILED && c.fail == TXC_FAIL_REFUSED);
	txc_init(&c, 20, TXP_FMT_IQ8, 0, 0, 1, 0, 0);
	txc_rx(&c, t, (const uint8_t *)"VRTB", 4);
	CHECK(c.state == TXC_FAILED && c.fail == TXC_FAIL_BUSY);
	/* no answer for 5 s */
	txc_init(&c, 20, TXP_FMT_IQ8, 0, 0, 1, 0, 0);
	txc_poll(&c, t, o, sizeof(o));
	txc_poll(&c, t + 5100000000LL, o, sizeof(o));
	CHECK(c.state == TXC_FAILED && c.fail == TXC_FAIL_NOCONT);
}

/* A run that starts after a wait for the arming (TXGO refused with code 5 a few times, pings going on) starts from
 * the same state as one that does not: the refusals leave nothing in the pacing; and a sender at the edge of its
 * credit never resyncs to an index under its own (it would send the last packet's words again). */
static void test_wait(void)
{
	struct txc c, d;
	uint8_t o[64], rx[128];
	int64_t t = 1000000000LL;
	struct txp_ak ak = {.nonce = 77, .code = TXP_RF_REFUSED};
	struct txp_ping pg;
	int n, refused = 0;

	txc_init(&c, 20, TXP_FMT_IQ8, TXP_GO_RF, 5, 77, 0, 0);
	c.arm_wait_ns = 60000000000LL;
	memcpy(rx, "VRTC\x01\x02\x03\x04\x05\x06\x07\x08", 12);
	(void)txc_poll(&c, t, o, sizeof(o));
	txc_rx(&c, t, rx, 12);
	/* ping every 5 ms (answered at once) until the 100 pongs, TXGO, refusal; four seconds of that */
	while (c.state == TXC_SUB || c.state == TXC_GO) {
		t += 1000000;
		while ((n = txc_poll(&c, t, o, sizeof(o))) > 0) {
			if (txp_kind(o, (size_t)n) == 'P') {
				txp_pg_get(&pg, o, (size_t)n);
				pg.b_ns = (uint64_t)t + 50000;
				txp_po_put(rx, sizeof(rx), &pg);
				txc_rx(&c, t + 100000, rx, TXP_PO_BYTES);
			} else if (txp_kind(o, (size_t)n) == 'G') {
				ak.code = refused < 3 ? TXP_RF_REFUSED : TXP_OK;
				ak.stream_id = 0xfeed0001U;
				ak.max_words = 702;
				ak.depth = 7020;
				ak.group = 32768;
				txp_ak_put(rx, sizeof(rx), &ak);
				if (refused++ < 3) {
					txc_rx(&c, t, rx, TXP_AK_BYTES);
					CHECK(c.state == TXC_GO);
				} else {
					CHECK((txc_rx(&c, t, rx, TXP_AK_BYTES) & TXC_EV_ACK) && c.state == TXC_RUN);
				}
			}
		}
	}
	CHECK(refused == 4 && c.state == TXC_RUN);
	/* what the run starts from: nothing of the wait but the delays of the pings */
	CHECK(!c.pace.have_start && !c.pace.running && !c.pace.jumping && c.pace.sent == 0 && c.pace.credit == 0 &&
	      c.pace.restart == 0 && c.pace.est.n == 0 && c.pace.resyncs == 0 && c.pace.credit_waits == 0 &&
	      c.pace.statuses == 0 && c.pace.next_burst_ns == 0 && c.pace.run_since_ns == 0 &&
	      !c.have_first && c.pace.pongs > 100);
	CHECK(c.pace.depth == 7020 && c.pace.group == 32768);
	/* the same counters as a run without the wait */
	txc_init(&d, 20, TXP_FMT_IQ8, TXP_GO_RF, 5, 77, 0, 0);
	CHECK(d.pace.have_start == c.pace.have_start && d.pace.est.n == c.pace.est.n && d.pace.sent == c.pace.sent);
	{
		/* at the credit edge: room 100 words (under a packet), the estimate says the play index is within the
		 * horizon: the old code jumped to credit - packet, 602 words back */
		struct txp_pace p;
		struct txp_plan pl;
		const int64_t now = 5000000000LL;
		uint64_t before;

		txp_pace_init(&p, 20, 702, 32768, 7020);
		p.have_start = p.running = true;
		p.credit = 100000;
		p.sent = 99900;
		before = p.sent;
		p.restart = 0;
		/* the estimate: play = sent - 1000 now */
		txp_minq_push(&p.est, now, now - (int64_t)((p.sent - 1000U) * p.npw), TXP_EST_WIN_NS);
		txp_pace_plan(&p, now, 16, &pl);
		CHECK(pl.n == 0 && pl.credit_bound && p.sent == before && p.resyncs == 0);
		/* the credit of the next flip comes: now it jumps, forward */
		p.credit += 32768;
		txp_pace_plan(&p, now + 1000, 16, &pl);
		CHECK(pl.resync && p.resyncs == 1 && pl.first > before && pl.n > 0);
	}
}

/* ---- the simulation ---- */

struct ev {
	int64_t t;
	uint64_t seq;
	int64_t bns;
	int kind; /* 0 packet to board, 1 status to PC, 2 ping to board, 3 pong to PC */
	uint64_t idx;
	uint32_t n;
	int64_t tns;
	struct txp_st st;
};

struct sim_cfg {
	const char *name;
	uint32_t msps;
	int64_t dmin_ns, dmax_ns;       /* one way delay range, both directions */
	int64_t board_stall_at, board_stall_ns, pc_stall_at, pc_stall_ns;
	int64_t drift_ppm;
	int64_t wake_late_ns;           /* the sender wakes up to this late */
	int64_t duration_ns;
	uint32_t depth;
	int64_t spike_period_ns, spike_dur_ns, spike_ns; /* a one way delay of spike_ns in both directions for spike_dur_ns every period */
	uint32_t max_mbps;
};

struct sim_res {
	int64_t lead_min, lead_max;     /* words, true play, measured outside the recovery windows */
	int64_t lead_min_all;
	uint64_t over_credit;           /* sends beyond the true credit at that time */
	uint64_t sent_words;
	double rate_err;
	uint32_t underruns;
	uint64_t lost, late, far;
	uint64_t resyncs, credit_waits;
	uint32_t underruns_after;       /* underruns later than 20 ms after the end of a stall */
	int64_t recover_ns;             /* from the end of a stall to the first lead in the band, -1: none */
	double est_err_max;
	uint32_t underruns_steady;      /* after the warm up */
	uint32_t max_run;               /* most packets back to back at the board (gaps of one wire time) */
	uint32_t max_queue;             /* most packets waiting for the board's CPU (8.6 us each), 16 = the DMA ring */
	uint64_t lead_low;
	int64_t pace_min_lead;
};

/* the pings of the probe phase before TXGO, as vrt_tx does (100 pongs at 5 ms) */
#define PROBE_NS 500000000LL
#define WIRE_NS 12048LL /* 1506 bytes at 1 Gb/s */
#define CPU_NS  8600LL  /* the board's CPU1 per packet */
#define HEAP_N 8192
static struct ev heap[HEAP_N];
static uint32_t heap_n;
static uint64_t ev_seq;

static bool ev_less(const struct ev *a, const struct ev *b)
{
	return a->t < b->t || (a->t == b->t && a->seq < b->seq);
}

static void heap_push(const struct ev *e)
{
	uint32_t i = heap_n++;

	if (heap_n > HEAP_N) {
		printf("heap full\n");
		exit(2);
	}
	heap[i] = *e;
	heap[i].seq = ev_seq++;
	while (i > 0 && ev_less(&heap[i], &heap[(i - 1) / 2])) {
		struct ev t = heap[i];

		heap[i] = heap[(i - 1) / 2];
		heap[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
}

static struct ev heap_pop(void)
{
	struct ev top = heap[0];
	uint32_t i = 0;

	heap[0] = heap[--heap_n];
	for (;;) {
		uint32_t l = 2 * i + 1, r = l + 1, m = i;

		if (l < heap_n && ev_less(&heap[l], &heap[m])) {
			m = l;
		}
		if (r < heap_n && ev_less(&heap[r], &heap[m])) {
			m = r;
		}
		if (m == i) {
			break;
		}
		struct ev t = heap[i];

		heap[i] = heap[m];
		heap[m] = t;
		i = m;
	}
	return top;
}

static uint64_t rng_s = 88172645463325252ULL;

static double urand(void)
{
	rng_s ^= rng_s << 13;
	rng_s ^= rng_s >> 7;
	rng_s ^= rng_s << 17;
	return (double)(rng_s >> 11) / 9007199254740992.0;
}

/* min + (max - min) * u^6: mostly near the least, now and then far up to the most */
static int64_t delay_at(const struct sim_cfg *c, int64_t now)
{
	if (c->spike_period_ns > 0 && now % c->spike_period_ns < c->spike_dur_ns) {
		return c->spike_ns;
	}
	double u = urand(), u6 = u * u * u * u * u * u;

	return c->dmin_ns + (int64_t)((double)(c->dmax_ns - c->dmin_ns) * u6);
}

#define delay(c) delay_at((c), t)

static void run_sim(const struct sim_cfg *c, struct sim_res *r)
{
	const uint32_t G = 32768, pkt = 702;
	struct tbm b;
	struct txp_pace p;
	int64_t t = 0, next_status = PROBE_NS, next_ping = 0, next_sample = 0, wake = PROBE_NS, last_arr[2] = {0, 0};
	int64_t b_s0 = c->board_stall_at, b_s1 = c->board_stall_at + c->board_stall_ns;
	int64_t p_s0 = c->pc_stall_at, p_s1 = c->pc_stall_at + c->pc_stall_ns;
	int64_t warm = PROBE_NS + 300000000LL, ping_n = 0;
	int64_t recover_from = -1, nic_free = 0, cpu_free = 0, prev_arr = -1000000;
	bool sender_cb = false;
	uint32_t run = 0, u_warm = 0;
	bool u_warm_cap = false;
	uint64_t sent_total = 0, sent_at_warm = 0;
	int64_t t_at_warm = 0;
	uint64_t prev_lo = 0;
	uint32_t dbg_u = 0, u_at = 0;
	bool u_cap = false;
	bool band_ok;

	heap_n = 0;
	memset(r, 0, sizeof(*r));
	r->lead_min = INT64_MAX;
	r->lead_max = INT64_MIN;
	r->lead_min_all = INT64_MAX;
	r->recover_ns = -1;
	tbm_init(&b, c->msps, G, c->depth, 3510, 2000000);
	b.drift_ppm = c->drift_ppm;
	txp_pace_init(&p, c->msps, pkt, G, c->depth);
	txp_pace_set_line(&p, 1420, c->max_mbps ? c->max_mbps : 900);
	prev_lo = b.lo;
	/* the recovery window: after a stall, until the lead is back in the band, nothing is measured */
	(void)band_ok;

	while (t < c->duration_ns) {
		int64_t tb_stall_end = (c->board_stall_ns > 0 && t >= b_s0 && t < b_s1) ? b_s1 : 0;
		bool bstall = tb_stall_end != 0;
		int64_t cand = heap_n ? heap[0].t : INT64_MAX;
		int which = 0; /* 0 heap, 1 status, 2 ping, 3 sample, 4 wake, 5 board deadline */
		int64_t bd = (b.run && !bstall) ? tbm_deadline(&b) : INT64_MAX;

		if (next_status < cand && !bstall) {
			cand = next_status;
			which = 1;
		}
		if (bstall && next_status < b_s1) {
			next_status = b_s1;
		}
		if (bstall && next_status < cand) {
			cand = next_status; /* handled right after the stall */
			which = 1;
		}
		if (next_ping < cand) {
			cand = next_ping;
			which = 2;
		}
		if (next_sample < cand) {
			cand = next_sample;
			which = 3;
		}
		if (wake < cand) {
			cand = wake;
			which = 4;
		}
		if (bd < cand) {
			cand = bd;
			which = 5;
		}
		t = cand > t ? cand : t;
		if (t >= c->duration_ns) {
			break;
		}
		if (which == 0) {
			struct ev e = heap_pop();

			if ((e.kind == 0 || e.kind == 2) && t >= b_s0 && t < b_s1 && c->board_stall_ns > 0) {
				e.t = b_s1;
				heap_push(&e);
				continue;
			}
			switch (e.kind) {
			case 0:
				if (t - prev_arr <= WIRE_NS + 300) {
					run++;
				} else {
					run = 1;
				}
				prev_arr = t;
				if (run > r->max_run) {
					r->max_run = run;
				}
				{
					int64_t q = cpu_free > t ? (cpu_free - t + CPU_NS - 1) / CPU_NS : 0;

					if ((uint32_t)q + 1U > r->max_queue) {
						r->max_queue = (uint32_t)q + 1U;
					}
					cpu_free = (cpu_free > t ? cpu_free : t) + CPU_NS;
				}
				tbm_advance(&b, t);
				tbm_packet(&b, t, e.idx, e.n);
				if (getenv("TXDBG") && b.underruns != dbg_u) {
					dbg_u = b.underruns;
					printf("  t=%lld underrun %u lo=%llu cursor=%llu hi=%llu play=%llu sent=%llu credit=%llu\n", (long long)t, dbg_u,
					       (unsigned long long)b.lo, (unsigned long long)b.cursor, (unsigned long long)b.hi,
					       (unsigned long long)tbm_play(&b, t), (unsigned long long)p.sent, (unsigned long long)p.credit);
				}
				break;
			case 1:
				txp_pace_status(&p, t, &e.st);
				if (sender_cb && !(c->pc_stall_ns > 0 && t >= p_s0 && t < p_s1)) {
					wake = t + (int64_t)(urand() * (double)c->wake_late_ns);
				}
				{
					/* estimator error against the true play index */
					if (p.running && t > warm) {
						double err = (double)((int64_t)txp_pace_play(&p, t) - (int64_t)tbm_play(&b, t));

						if (err < 0) {
							err = -err;
						}
						if (err > r->est_err_max) {
							r->est_err_max = err;
						}
					}
				}
				break;
			case 2: {
				struct ev o = {.kind = 3, .tns = e.tns, .bns = t};

				o.t = t + delay(c);
				if (o.t < last_arr[1]) {
					o.t = last_arr[1];
				}
				last_arr[1] = o.t;
				heap_push(&o);
				break;
			}
			default:
				txp_pace_pong(&p, t, e.tns, e.bns);
				break;
			}
		} else if (which == 1) {
			struct ev o = {.kind = 1};

			tbm_advance(&b, t);
			tbm_status(&b, t, &o.st);
			o.t = t + delay(c);
			if (o.t < last_arr[1]) {
				o.t = last_arr[1];
			}
			last_arr[1] = o.t;
			heap_push(&o);
			next_status = t + 250000;
			prev_lo = b.lo;
		} else if (which == 5) {
			tbm_advance(&b, t);
			if (getenv("TXDBG") && b.underruns != dbg_u) {
				dbg_u = b.underruns;
				printf("  t=%lld (deadline) underrun %u lo=%llu cursor=%llu hi=%llu play=%llu sent=%llu credit=%llu pcredit=%llu\n", (long long)t, dbg_u,
				       (unsigned long long)b.lo, (unsigned long long)b.cursor, (unsigned long long)b.hi,
				       (unsigned long long)tbm_play(&b, t), (unsigned long long)p.sent, (unsigned long long)b.hi + b.depth, (unsigned long long)p.credit);
			}
			if (b.lo != prev_lo) {
				/* a flip: a status at once */
				struct ev o = {.kind = 1};

				tbm_status(&b, t, &o.st);
				o.t = t + delay(c);
				if (o.t < last_arr[1]) {
					o.t = last_arr[1];
				}
				last_arr[1] = o.t;
				heap_push(&o);
				prev_lo = b.lo;
			}
		} else if (which == 2) {
			struct ev o = {.kind = 2, .tns = t};

			o.t = t + delay(c);
			if (o.t < last_arr[0]) {
				o.t = last_arr[0];
			}
			last_arr[0] = o.t;
			heap_push(&o);
			ping_n++;
			next_ping = t + (5000000);
		} else if (which == 4) {
			struct txp_plan pl;
			int64_t w;

			txp_pace_plan(&p, t, 16, &pl);
			sender_cb = pl.credit_bound;
			if (pl.resync) {
				r->resyncs++;
			}
			for (uint32_t k = 0; k < pl.n; k++) {
				struct ev o = {.kind = 0, .idx = pl.first + (uint64_t)k * pkt, .n = pkt};
				int64_t dep = t + 5000 > nic_free ? t + 5000 : nic_free; /* the NIC sends at 1 Gb/s */

				nic_free = dep + WIRE_NS;
				o.t = dep + WIRE_NS + delay(c);
				if (o.t < last_arr[0] + WIRE_NS) { /* the last hop serialises too */
					o.t = last_arr[0] + WIRE_NS;
				}
				last_arr[0] = o.t;
				heap_push(&o);
				/* never beyond the true credit of this moment */
				if (o.idx + pkt > (b.started ? b.hi + b.depth : b.hi) && b.started) {
					r->over_credit++;
				}
				sent_total += pkt;
			}
			w = pl.wake_ns + (int64_t)(urand() * (double)c->wake_late_ns);
			if (pl.credit_bound) {
				w = t + 1000000; /* the timeout; a status wakes it sooner */
			}
			if (t >= p_s0 && t < p_s1 && c->pc_stall_ns > 0) {
				w = p_s1;
			} else if (w > p_s0 && w < p_s1 && t < p_s0 && c->pc_stall_ns > 0) {
				w = p_s0 < w ? w : p_s0;
			}
			wake = w > t ? w : t + 1000;
			/* the PC stall starts when it would run next inside it: it sleeps through */
			if (c->pc_stall_ns > 0 && wake >= p_s0 && wake < p_s1) {
				wake = p_s1;
			}
		} else {
			/* a sample of the lead against the true play index */
			if (b.run && t >= b.t0 + 1000000 && p.have_start) {
				int64_t lead = (int64_t)p.sent - (int64_t)tbm_play(&b, t);
				bool in_stall = c->board_stall_ns > 0 && t >= b_s0 && t < b_s1 + 40000000;
				bool in_pcstall = c->pc_stall_ns > 0 && t >= p_s0 && t < p_s1 + 40000000;

				if (lead < r->lead_min_all) {
					r->lead_min_all = lead;
				}
				if (t > warm && !in_stall && !in_pcstall) {
					if (lead < r->lead_min) {
						r->lead_min = lead;
					}
					if (lead > r->lead_max) {
						r->lead_max = lead;
					}
				}
				if (t > warm && !u_warm_cap) {
					u_warm_cap = true;
					u_warm = b.underruns;
				}
				if (t > warm && sent_at_warm == 0) {
					sent_at_warm = sent_total;
					t_at_warm = t;
				}
				if (recover_from < 0 && ((c->board_stall_ns > 0 && t >= b_s1) || (c->pc_stall_ns > 0 && t >= p_s1))) {
					recover_from = c->board_stall_ns > 0 ? b_s1 : p_s1;
				}
				if (recover_from >= 0 && r->recover_ns < 0 && lead >= (int64_t)G + 3000 &&
				    lead <= (int64_t)G + 3000 + 14000 && t > recover_from) {
					/* in the band, and the board has stopped underrunning for 2 windows */
					r->recover_ns = t - recover_from;
				}
			}
			if (recover_from >= 0 && !u_cap && t >= recover_from + 20000000) {
				u_cap = true;
				u_at = b.underruns;
			}
			next_sample = t + 50000;
		}
	}
	r->sent_words = sent_total;
	if (t_at_warm != 0) {
		double exp_words = (double)(c->duration_ns - t_at_warm) * (double)c->msps / 1000.0;
		double got = (double)(sent_total - sent_at_warm);

		r->rate_err = (got - exp_words) / exp_words;
	}
	r->underruns = b.underruns;
	r->underruns_steady = u_warm_cap ? b.underruns - u_warm : b.underruns;
	r->lead_low = p.lead_low;
	r->pace_min_lead = p.min_lead;
	r->underruns_after = u_cap ? b.underruns - u_at : 0;
	r->lost = b.lost;
	r->late = b.late;
	r->far = b.far;
	r->resyncs = p.resyncs;
	r->credit_waits = p.credit_waits;
}

static void report(const struct sim_cfg *c, const struct sim_res *r)
{
	printf("%-30s %2u MS/s depth %5u lead [%6lld, %6lld] est %3.0f rate %+.4f%% underruns %u (steady %u) lost %llu late %llu far %llu "
	       "resync %llu over_credit %llu recover %lld us | board burst %u back to back, cpu queue %u, lead floor warned %llu (least %lld)\n",
	       c->name, c->msps, c->depth, (long long)(r->lead_min - 32768), (long long)(r->lead_max - 32768),
	       r->est_err_max, r->rate_err * 100.0, r->underruns, r->underruns_steady, (unsigned long long)r->lost,
	       (unsigned long long)r->late, (unsigned long long)r->far, (unsigned long long)r->resyncs,
	       (unsigned long long)r->over_credit, (long long)(r->recover_ns < 0 ? -1 : r->recover_ns / 1000),
	       r->max_run, r->max_queue, (unsigned long long)r->lead_low,
	       (long long)(r->pace_min_lead == INT64_MAX ? 0 : r->pace_min_lead - 32768));
}

/* the budget of tx_pace.h for a configuration, total status + data delay in ns */
static int64_t budget_of(const struct sim_cfg *c)
{
	struct txp_pace p;

	txp_pace_init(&p, c->msps, 702, 32768, c->depth);
	txp_pace_set_line(&p, 1420, c->max_mbps ? c->max_mbps : 900);
	return txp_delay_budget_ns(&p);
}

static void sim_all(void)
{
	static const uint32_t rates[2] = {20, 40};
	/* 7020: 16 buffers held in a pool of 34 (the first build); 13338: TX_DEPTH_PKTS packets of 702 (src/tx_hold.h) */
	static const uint32_t depths[2] = {7020, TX_DEPTH_PKTS * 702U};

	for (int ri = 0; ri < 2; ri++) {
		for (int di = 0; di < 2; di++) {
			uint32_t m = rates[ri];
			struct sim_cfg base = {.msps = m, .dmin_ns = 60000, .dmax_ns = 120000, .wake_late_ns = 40000,
					       .duration_ns = 3000000000LL, .depth = depths[di], .max_mbps = 900};
			struct sim_cfg c;
			struct sim_res r;
			int64_t bud = budget_of(&base), lo, hi, found;
			char name[64];

			/* steady, a modest delay: nothing lost, the credit never exceeded, rate right */
			c = base;
			c.name = "steady 60-120us";
			run_sim(&c, &r);
			report(&c, &r);
			CHECK(r.over_credit == 0 && r.far == 0);
			if (bud > 600000) {
				CHECK(r.underruns == 0 && r.late == 0 && r.lost == 0);
			}
			CHECK(r.rate_err > -0.001 && r.rate_err < 0.001);
			CHECK(r.max_queue <= 2); /* jitter can merge bursts at the receiver; the CPU is faster than the line */

			/* the largest constant one way delay (the same both ways: status + data = twice it)
			 * without a steady state underrun, by bisection: with an ideal sender (it wakes on
			 * time: the budget of tx_pace.h is the prediction) and with one that wakes up to 40 us
			 * late at each of its 3 or 4 wakeups per flip (a plain scheduler)
			 */
			{
				int64_t f[2];

				for (int w = 0; w < 2; w++) {
					lo = 10000;
					hi = 1500000;
					found = lo;
					while (hi - lo > 10000) {
						int64_t mid = (lo + hi) / 2;

						c = base;
						c.duration_ns = 2200000000LL;
						c.wake_late_ns = w == 0 ? 0 : 40000;
						c.dmin_ns = c.dmax_ns = mid;
						run_sim(&c, &r);
						if (r.underruns_steady == 0 && r.over_credit == 0 && r.far == 0) {
							found = mid;
							lo = mid;
						} else {
							hi = mid;
						}
					}
					f[w] = found;
				}
				printf("  %2u MS/s depth %5u: largest one way delay without underruns: %.0f us (status + data %.0f us) "
				       "with an exact sender, %.0f us (%.0f us) with one up to 40 us late; budget formula %.0f us together\n",
				       m, depths[di], (double)f[0] / 1e3, 2.0 * (double)f[0] / 1e3, (double)f[1] / 1e3,
				       2.0 * (double)f[1] / 1e3, (double)bud / 1e3);
				CHECK(bud > 0 && 2 * f[0] >= bud * 9 / 10 && 2 * f[0] <= bud * 12 / 10);
				CHECK(2 * f[1] >= bud * 4 / 10);
			}
			if (bud > 0) {
				c = base;
				c.name = "at 0.9 of the budget";
				c.wake_late_ns = 0;
				c.dmin_ns = c.dmax_ns = bud * 9 / 20;
				run_sim(&c, &r);
				report(&c, &r);
				CHECK(r.underruns_steady == 0 && r.over_credit == 0 && r.far == 0);
				/* bursts of TXP_BURST follow each other a few us apart at the line cap: the stream is one run; what the board
				 * needs is a CPU queue of at most 2 (the hold of buffers covers depth + burst + 2) */
				CHECK(r.max_run <= 32 && r.max_queue <= 2);
			}

			/* the measured link: RTT 0.175 min, 0.31 mean, 1.15 ms max: a one way spike of 575 us in
			 * both directions for 2 ms every 100 ms on top of 88 to 150 us
			 */
			c = base;
			snprintf(name, sizeof(name), "spike RTT 1.15ms / 100ms");
			c.name = name;
			c.dmin_ns = 88000;
			c.dmax_ns = 150000;
			c.spike_period_ns = 100000000LL;
			c.spike_dur_ns = 2000000;
			c.spike_ns = 575000;
			c.duration_ns = 4000000000LL;
			run_sim(&c, &r);
			report(&c, &r);
			CHECK(r.over_credit == 0 && r.far == 0);
			if (bud * 9 / 10 > 1150000) {
				CHECK(r.underruns_steady == 0 && r.late == 0);
			}
		}
	}
	/* stalls, the depth the board reports now */
	for (int ri = 0; ri < 2; ri++) {
		uint32_t m = rates[ri];
		struct sim_cfg c = {.msps = m, .dmin_ns = 60000, .dmax_ns = 120000, .wake_late_ns = 40000,
				    .duration_ns = 3000000000LL, .depth = TX_DEPTH_PKTS * 702U, .max_mbps = 900};
		struct sim_res r;

		c.name = "drift +20ppm";
		c.drift_ppm = 20;
		run_sim(&c, &r);
		report(&c, &r);
		CHECK(r.over_credit == 0 && r.far == 0 && r.underruns == 0 && r.late == 0);
		c.name = "drift -20ppm";
		c.drift_ppm = -20;
		run_sim(&c, &r);
		report(&c, &r);
		CHECK(r.over_credit == 0 && r.far == 0 && r.underruns == 0 && r.late == 0);

		c.drift_ppm = 0;
		c.name = "board stall 5 ms";
		c.board_stall_at = 1000000000LL;
		c.board_stall_ns = 5000000;
		run_sim(&c, &r);
		report(&c, &r);
		CHECK(r.over_credit == 0 && r.far == 0);
		CHECK(r.resyncs >= 1 && r.underruns >= 1 && r.underruns <= 4U + (uint32_t)(5000000ULL * m / 1000 / 32768));
		/* 40 MS/s has the least margin (the formula 0.42 ms against delays of 60 to 120 us each way and wakeups up
		 * to 40 us late): one steady state underrun in the 2 s after the stall (not the stall's) is the sequence of
		 * random delays, not a failed recovery; at 20 MS/s there is none */
		CHECK(r.underruns_after <= (m == 40U ? 1U : 0U));

		c.board_stall_at = 0;
		c.board_stall_ns = 0;
		c.name = "PC stall 50 ms";
		c.pc_stall_at = 1000000000LL;
		c.pc_stall_ns = 50000000;
		run_sim(&c, &r);
		report(&c, &r);
		CHECK(r.over_credit == 0 && r.far == 0);
		CHECK(r.resyncs >= 1 && r.underruns >= 1 && r.underruns <= 4U + (uint32_t)(50000000ULL * m / 1000 / 32768));
		CHECK(r.underruns_after == 0);
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	test_proto();
	test_tone();
	test_minq();
	test_core();
	test_wait();
	sim_all();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
