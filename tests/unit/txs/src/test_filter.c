/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive hook's filter (apps/txstream_eth/src/tx_filter.h): frames of IPv4 and IPv6, ours and
 * not, from the subscriber and not, with the session's stream ID and not, truncated and padded.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "tx_filter.h"
#include "txs_wire.h"

ZTEST_SUITE(tx_filter, NULL, NULL, NULL, NULL, NULL);

static const uint8_t MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t PEER_MAC[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
static const uint8_t A4_SUB[4] = {192, 168, 7, 20}, A4_US[4] = {192, 168, 7, 99};
static const uint8_t A6_SUB[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0xff, 0xfe, 0x78, 0x9a, 0xbc};
static const uint8_t A6_US[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
#define SID 0x5ab3c0deU
#define SPORT 40000U

static struct txf_state state(bool v6)
{
	struct txf_state s = {.v6 = v6, .sport = SPORT};

	memcpy(s.mac, MAC, 6);
	s.sid[0] = (uint8_t)(SID >> 24);
	s.sid[1] = (uint8_t)(SID >> 16);
	s.sid[2] = (uint8_t)(SID >> 8);
	s.sid[3] = (uint8_t)SID;
	memcpy(s.src, v6 ? A6_SUB : A4_SUB, v6 ? 16 : 4);
	memcpy(s.dst, v6 ? A6_US : A4_US, v6 ? 16 : 4);
	return s;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* A frame of a packet of `words` iq8 words (1404 B at 702), the way a host sends it; returns its length. */
static uint16_t frame(uint8_t *f, bool v6, uint32_t words, uint16_t dport)
{
	const struct txs_wire_cfg wc = {.stream_id = SID, .port = TXF_PORT, .fmt = TXS_FMT_IQ8, .max_words = 702};
	const uint16_t vita = (uint16_t)(16U + 2U * words), udp = (uint16_t)(8U + vita);
	uint16_t hdr = v6 ? 14U + 40U + 8U : 14U + 20U + 8U;

	memset(f, 0, 1600);
	memcpy(f, MAC, 6);
	memcpy(f + 6, PEER_MAC, 6);
	if (v6) {
		put16(f + 12, 0x86dd);
		f[14] = 0x60;
		put16(f + 18, udp);
		f[20] = 17;
		f[21] = 64;
		memcpy(f + 22, A6_SUB, 16);
		memcpy(f + 38, A6_US, 16);
	} else {
		put16(f + 12, 0x0800);
		f[14] = 0x45;
		put16(f + 16, (uint16_t)(20U + udp));
		put16(f + 20, 0x4000); /* DF */
		f[22] = 64;
		f[23] = 17;
		memcpy(f + 26, A4_SUB, 4);
		memcpy(f + 30, A4_US, 4);
	}
	put16(f + hdr - 8, SPORT);
	put16(f + hdr - 6, dport);
	put16(f + hdr - 4, udp);
	txs_wire_header(f + hdr, &wc, 0, 12345, words);
	return (uint16_t)(hdr + vita);
}

static enum txf_verdict run(const struct txf_state *s, bool accept, const uint8_t *f, uint16_t len,
			    uint16_t *off, uint16_t *vlen)
{
	return txf_classify(s, accept, f, len, off, vlen);
}

ZTEST(tx_filter, test_take)
{
	static uint8_t f[1600];

	for (int v = 0; v < 2; v++) {
		const struct txf_state s = state(v != 0);
		uint16_t off = 0, vlen = 0;
		const uint16_t len = frame(f, v != 0, 702U, TXF_PORT);
		struct txs_wire_pkt pk;
		const struct txs_wire_cfg wc = {.stream_id = SID, .port = TXF_PORT, .fmt = TXS_FMT_IQ8, .max_words = 702};

		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_TAKE);
		zassert_equal(off, v != 0 ? 62U : 42U);
		zassert_equal(vlen, 1420U);
		/* what the stream parses at that place is the packet */
		zassert_equal(txs_wire_parse(&wc, TXF_PORT, f + off, vlen, &pk), TXS_WIRE_OK);
		zassert_equal(pk.index, 12345U);
		zassert_equal(pk.words, 702U);
		/* padded to the Ethernet minimum or by the MAC: the length is the header's */
		zassert_equal(run(&s, true, f, 1514, &off, &vlen), TXF_TAKE);
		zassert_equal(vlen, 1420U);
		/* a short packet (a word) is taken, the stream parses it */
		zassert_equal(run(&s, true, f, frame(f, v != 0, 2U, TXF_PORT), &off, &vlen), TXF_TAKE);
		zassert_equal(vlen, 20U);
	}
}

ZTEST(tx_filter, test_not_ours)
{
	static uint8_t f[1600];
	struct txf_state s = state(false);
	uint16_t off, vlen, len = frame(f, false, 702U, TXF_PORT);

	/* another MAC: the stack (a frame for another host seen in a promiscuous or broadcast case) */
	f[5] ^= 1;
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	f[5] ^= 1;
	/* another port: the stack, accepted or not */
	len = frame(f, false, 702U, 4992);
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	zassert_equal(run(&s, false, f, len, &off, &vlen), TXF_STACK);
	/* TCP, an ARP, IPv4 with options, a fragment, IPv6 with another next header, another EtherType */
	len = frame(f, false, 702U, TXF_PORT);
	f[23] = 6;
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	f[23] = 17;
	f[14] = 0x46;
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	f[14] = 0x45;
	f[20] = 0x20; /* more fragments */
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	f[20] = 0x40;
	f[21] = 0x08; /* an offset */
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	f[21] = 0;
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_TAKE);
	f[12] = 0x08;
	f[13] = 0x06;
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	s = state(true);
	len = frame(f, true, 702U, TXF_PORT);
	f[20] = 0;
	zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_STACK);
	/* shorter than any packet */
	len = frame(f, false, 702U, TXF_PORT);
	zassert_equal(run(&s, true, f, 61, &off, &vlen), TXF_STACK);
}

ZTEST(tx_filter, test_dropped_for_a_reason)
{
	static uint8_t f[1600];

	for (int v = 0; v < 2; v++) {
		const bool v6 = v != 0;
		const struct txf_state s = state(v6);
		const uint16_t hdr = v6 ? 62U : 42U, sa = v6 ? 22U : 26U, al = v6 ? 16U : 4U;
		uint16_t off, vlen, len = frame(f, v6, 702U, TXF_PORT);

		/* no run takes data: counted as such, whoever sent it */
		zassert_equal(run(&s, false, f, len, &off, &vlen), TXF_IDLE);
		/* the source address, the destination address, the port */
		f[sa] ^= 1;
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_SRC);
		f[sa] ^= 1;
		f[sa + al + al - 1U] ^= 1;
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_SRC);
		f[sa + al + al - 1U] ^= 1;
		f[hdr - 7U] ^= 1; /* the source port's low byte */
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_SRC);
		f[hdr - 7U] ^= 1;
		/* the other address family than the subscriber's */
		{
			struct txf_state o = state(!v6);

			zassert_equal(run(&o, true, f, len, &off, &vlen), TXF_SRC);
		}
		/* the stream ID */
		f[hdr + 7U] ^= 1;
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_SID);
		f[hdr + 7U] ^= 1;
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_TAKE);
		/* lengths: the frame cut short, the UDP length not the IP's, a runt */
		zassert_equal(run(&s, true, f, (uint16_t)(len - 4U), &off, &vlen), TXF_LEN);
		put16(f + hdr - 4U, 1000);
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_LEN);
		put16(f + hdr - 4U, 1428);
		zassert_equal(run(&s, true, f, len, &off, &vlen), TXF_TAKE);
		len = frame(f, v6, 1U, TXF_PORT);
		/* 8 + 16 + 2 bytes: under a VITA header and a word */
		zassert_equal(run(&s, true, f, 1514, &off, &vlen), TXF_LEN);
		/* the length check comes first: a bad length is counted as one even from a stranger, and with
		 * no run (a frame that is not even a packet is not a "no run" one) */
		zassert_equal(run(&s, false, f, 1514, &off, &vlen), TXF_LEN);
	}
}
