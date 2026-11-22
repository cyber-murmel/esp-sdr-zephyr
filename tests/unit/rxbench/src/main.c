/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/rxbench: the frame filter of the receive hook, the SPSC ring, the payload
 * check and the sequence accounting.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "rxb_parse.h"
#include "rxb_proto.h"
#include "rxb_ring.h"
#include "rxb_seq.h"

#define PORT RXB_PORT
#define FRAME_MAX 1600

static const uint8_t MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t OTHER[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x66};

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* Ethernet + IPv4 (ihl words) + UDP, a payload of plen bytes; returns the frame length. */
static size_t build4(uint8_t *f, unsigned int ihl, uint16_t port, uint16_t plen, uint16_t frag,
		     uint8_t proto)
{
	size_t hl = ihl * 4U, ip_total = hl + 8U + plen;

	memset(f, 0, FRAME_MAX);
	memcpy(f, MAC, 6);
	put16(f + 12, 0x0800);
	f[14] = (uint8_t)(0x40 | ihl);
	put16(f + 14 + 2, (uint16_t)ip_total);
	put16(f + 14 + 6, frag);
	f[14 + 9] = proto;
	put16(f + 14 + hl + 2, port);
	put16(f + 14 + hl + 4, (uint16_t)(8U + plen));
	rxb_fill(f + 14 + hl + 8, plen, 7);
	return 14 + ip_total;
}

static size_t build6(uint8_t *f, uint16_t port, uint16_t plen, uint8_t next)
{
	memset(f, 0, FRAME_MAX);
	memcpy(f, MAC, 6);
	put16(f + 12, 0x86dd);
	f[14] = 0x60;
	put16(f + 14 + 4, (uint16_t)(8U + plen));
	f[14 + 6] = next;
	put16(f + 14 + 40 + 2, port);
	put16(f + 14 + 40 + 4, (uint16_t)(8U + plen));
	rxb_fill(f + 14 + 48, plen, 9);
	return 14 + 48 + plen;
}

ZTEST(rxbench, test_parse_ipv4)
{
	static uint8_t f[FRAME_MAX];
	struct rxb_hit h;
	size_t n = build4(f, 5, PORT, 1440, 0x4000, 17);

	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 4);
	zassert_equal(h.off, 14 + 20 + 8);
	zassert_equal(h.plen, 1440);
	zassert_equal(h.ip, 4);

	/* options: the header is longer, the payload moves */
	n = build4(f, 8, PORT, 100, 0, 17);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 4);
	zassert_equal(h.off, 14 + 32 + 8);
	zassert_equal(h.plen, 100);

	/* padding behind the IP packet (a short frame) is not payload */
	n = build4(f, 5, PORT, 8, 0, 17);
	zassert_equal(rxb_parse(f, n + 20, MAC, PORT, &h), 4);
	zassert_equal(h.plen, 8);
}

ZTEST(rxbench, test_parse_ipv4_rejects)
{
	static uint8_t f[FRAME_MAX];
	struct rxb_hit h;
	size_t n;

	n = build4(f, 5, PORT + 1, 100, 0, 17);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "other port");
	n = build4(f, 5, PORT, 100, 0, 6);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "TCP");
	n = build4(f, 5, PORT, 100, 0x2000, 17);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "more fragments");
	n = build4(f, 5, PORT, 100, 0x00b9, 17);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "fragment offset");
	n = build4(f, 4, PORT, 100, 0, 17);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "header below 20 bytes");
	n = build4(f, 5, PORT, 100, 0, 17);
	zassert_equal(rxb_parse(f, n, OTHER, PORT, &h), 0, "other MAC address");
	zassert_equal(rxb_parse(f, n - 1, MAC, PORT, &h), 0, "IP length beyond the frame");
	zassert_equal(rxb_parse(f, 13, MAC, PORT, &h), 0, "shorter than Ethernet");
	zassert_equal(rxb_parse(f, 14 + 20 + 7, MAC, PORT, &h), 0, "no room for UDP");
	f[14] = 0x65;
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "version 6 in an IPv4 frame");
	n = build4(f, 5, PORT, 100, 0, 17);
	put16(f + 14 + 20 + 4, 200);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "UDP length beyond the IP packet");
	put16(f + 14 + 20 + 4, 7);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "UDP length below the header");
	n = build4(f, 5, PORT, 100, 0, 17);
	put16(f + 12, 0x0806);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "ARP");
	put16(f + 12, 0x8100);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "VLAN tag");
}

ZTEST(rxbench, test_parse_ipv6)
{
	static uint8_t f[FRAME_MAX];
	struct rxb_hit h;
	size_t n = build6(f, PORT, 1420, 17);

	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 6);
	zassert_equal(h.off, 14 + 40 + 8);
	zassert_equal(h.plen, 1420);
	zassert_equal(h.ip, 6);

	n = build6(f, PORT + 1, 100, 17);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "other port");
	n = build6(f, PORT, 100, 44);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "fragment header");
	n = build6(f, PORT, 100, 0);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "hop by hop options");
	n = build6(f, PORT, 100, 58);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "ICMPv6");
	n = build6(f, PORT, 100, 17);
	zassert_equal(rxb_parse(f, n, OTHER, PORT, &h), 0, "other MAC address");
	zassert_equal(rxb_parse(f, n - 1, MAC, PORT, &h), 0, "payload length beyond the frame");
	zassert_equal(rxb_parse(f, 14 + 40 + 7, MAC, PORT, &h), 0, "no room for UDP");
	put16(f + 14 + 40 + 4, 200);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0, "UDP length beyond the IP payload");
}

ZTEST(rxbench, test_parse_short_lengths)
{
	static uint8_t f[FRAME_MAX];
	struct rxb_hit h;
	size_t n = build4(f, 5, PORT, 100, 0, 17);

	/* IPv4 total length below the header plus a UDP header */
	put16(f + 14 + 2, 20 + 7);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0);
	put16(f + 14 + 2, 20 + 8);
	put16(f + 14 + 20 + 4, 8);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 4, "an empty payload is still the port's");
	zassert_equal(h.plen, 0);

	/* IPv6 payload length below a UDP header */
	n = build6(f, PORT, 100, 17);
	put16(f + 14 + 4, 7);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 0);

	/* trailing padding behind the IPv6 packet is not payload */
	n = build6(f, PORT, 8, 17);
	zassert_equal(rxb_parse(f, n + 10, MAC, PORT, &h), 6);
	zassert_equal(h.plen, 8);
	/* UDP length below the IP payload: the rest of the IP payload is not ours either */
	n = build6(f, PORT, 100, 17);
	put16(f + 14 + 40 + 4, 8 + 50);
	zassert_equal(rxb_parse(f, n, MAC, PORT, &h), 6);
	zassert_equal(h.plen, 50);
}

ZTEST(rxbench, test_seq_restart_and_outage)
{
	struct rxb_seq s;
	struct rxb_seq_counts c;

	memset(&c, 0, sizeof(c));
	rxb_seq_reset(&s);
	for (uint32_t i = 1000000U; i < 1000100U; i++) {
		rxb_seq_track(&s, i, &c);
	}
	zassert_equal(s.first, 1000000U);
	zassert_equal(s.last, 1000099U);
	zassert_equal(s.expected, 1000100U);
	/* an outage: the sender went on counting, what did not come is lost */
	rxb_seq_track(&s, 1050000U, &c);
	zassert_equal(c.lost, 1050000U - 1000100U);
	zassert_equal(c.gaps, 1U);
	zassert_equal(c.restarts, 0U);
	/* the sender starts again from 0: a restart, no loss, no out of order */
	rxb_seq_track(&s, 0U, &c);
	rxb_seq_track(&s, 1U, &c);
	zassert_equal(c.restarts, 1U);
	zassert_equal(c.ooo, 0U);
	zassert_equal(c.lost, 1050000U - 1000100U);
	zassert_equal(s.first, 0U);
	zassert_equal(s.expected, 2U);
	/* a late packet within the window is out of order, not a restart */
	rxb_seq_track(&s, 3U, &c);
	rxb_seq_track(&s, 2U, &c);
	zassert_equal(c.ooo, 1U);
	zassert_equal(c.restarts, 1U);
}

ZTEST(rxbench, test_proto)
{
	uint8_t p[64];
	uint32_t seq;

	rxb_fill(p, sizeof(p), 0x01020304U);
	zassert_equal(p[0], 1);
	zassert_equal(p[3], 4);
	zassert_equal(p[4], 0xa5);
	zassert_equal(p[7], 0x3c);
	zassert_equal(p[8], 0xa5);
	zassert_true(rxb_check(p, sizeof(p), true, &seq));
	zassert_equal(seq, 0x01020304U);
	zassert_true(rxb_check(p, sizeof(p), false, &seq));

	/* a flipped byte in the middle: only the full check sees it */
	p[30] ^= 1;
	zassert_true(rxb_check(p, sizeof(p), false, &seq));
	zassert_false(rxb_check(p, sizeof(p), true, &seq));
	p[30] ^= 1;
	/* in the first pattern word or the tail: both see it */
	p[5] ^= 1;
	zassert_false(rxb_check(p, sizeof(p), false, &seq));
	p[5] ^= 1;
	p[sizeof(p) - 1] ^= 1;
	zassert_false(rxb_check(p, sizeof(p), false, &seq));

	/* shortest payload, and shorter */
	rxb_fill(p, RXB_MIN_PAYLOAD, 5);
	zassert_true(rxb_check(p, RXB_MIN_PAYLOAD, false, &seq));
	zassert_true(rxb_check(p, RXB_MIN_PAYLOAD, true, &seq));
	zassert_false(rxb_check(p, RXB_MIN_PAYLOAD - 1, false, &seq));
	/* a length that is no multiple of the pattern: the tail follows the position */
	rxb_fill(p, 13, 6);
	zassert_true(rxb_check(p, 13, true, &seq));
}

ZTEST(rxbench, test_seq)
{
	struct rxb_seq s;
	struct rxb_seq_counts c;

	memset(&c, 0, sizeof(c));
	rxb_seq_reset(&s);
	rxb_seq_track(&s, 100, &c);	/* the first one only sets the number */
	rxb_seq_track(&s, 101, &c);
	rxb_seq_track(&s, 102, &c);
	zassert_equal(c.lost + c.gaps + c.dups + c.ooo, 0U);
	rxb_seq_track(&s, 105, &c);	/* 103 and 104 are lost */
	zassert_equal(c.lost, 2U);
	zassert_equal(c.gaps, 1U);
	rxb_seq_track(&s, 105, &c);	/* the same again */
	zassert_equal(c.dups, 1U);
	rxb_seq_track(&s, 104, &c);	/* a late one */
	zassert_equal(c.ooo, 1U);
	rxb_seq_track(&s, 106, &c);	/* the stream goes on from 105 */
	zassert_equal(c.lost, 2U);
	zassert_equal(c.gaps, 1U);

	/* the number wraps */
	rxb_seq_reset(&s);
	memset(&c, 0, sizeof(c));
	rxb_seq_track(&s, 0xfffffffeU, &c);
	rxb_seq_track(&s, 0xffffffffU, &c);
	rxb_seq_track(&s, 0U, &c);
	rxb_seq_track(&s, 1U, &c);
	zassert_equal(c.lost + c.gaps + c.dups + c.ooo, 0U);
	rxb_seq_track(&s, 4U, &c);
	zassert_equal(c.lost, 2U);

	/* a reset starts a new run */
	rxb_seq_reset(&s);
	rxb_seq_track(&s, 5000U, &c);
	zassert_equal(c.lost, 2U);
}

ZTEST(rxbench, test_ring_single)
{
	struct rxb_item slots[4], it;
	struct rxb_ring r;
	int dummy[8];

	rxb_ring_init(&r, slots, 4);
	zassert_false(rxb_ring_pop(&r, &it), "empty");
	zassert_equal(rxb_ring_count(&r), 0U);
	for (int i = 0; i < 4; i++) {
		it.buf = &dummy[i];
		it.len = (uint16_t)(100 + i);
		it.off = 62;
		it.plen = 38;
		zassert_true(rxb_ring_push(&r, &it), "slot %d", i);
	}
	zassert_equal(rxb_ring_count(&r), 4U);
	zassert_false(rxb_ring_push(&r, &it), "full");
	for (int i = 0; i < 4; i++) {
		zassert_true(rxb_ring_pop(&r, &it));
		zassert_equal(it.buf, &dummy[i], "order");
		zassert_equal(it.len, 100 + i);
	}
	zassert_false(rxb_ring_pop(&r, &it));

	/* the indices wrap by 2^32: start near it */
	r.head = r.tail = 0xfffffffeU;
	for (int i = 0; i < 6; i++) {
		it.buf = &dummy[i];
		zassert_true(rxb_ring_push(&r, &it));
		zassert_true(rxb_ring_pop(&r, &it));
		zassert_equal(it.buf, &dummy[i]);
	}
	zassert_equal(r.head, 4U);
}

#define XFER_ITEMS 100000U
#define XFER_SLOTS 64U
#define XFER_STACK 2048

static struct rxb_item xfer_slots[XFER_SLOTS];
static struct rxb_ring xfer_ring;
static K_THREAD_STACK_DEFINE(xfer_stack, XFER_STACK);
static struct k_thread xfer_thread;

static void producer(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (uint32_t i = 0; i < XFER_ITEMS;) {
		struct rxb_item it = {.buf = (void *)(uintptr_t)(i + 1U), .len = (uint16_t)i,
				      .off = (uint16_t)(i >> 8), .plen = (uint16_t)(i >> 16)};

		if (rxb_ring_push(&xfer_ring, &it)) {
			i++;
		} else {
			k_sleep(K_TICKS(1));
		}
	}
}

/* Every item arrives, once, in order, whole (the fields of an item belong to one number). */
ZTEST(rxbench, test_ring_two_threads)
{
	struct rxb_item it;
	uint32_t want = 0U;

	rxb_ring_init(&xfer_ring, xfer_slots, XFER_SLOTS);
	k_thread_create(&xfer_thread, xfer_stack, K_THREAD_STACK_SIZEOF(xfer_stack), producer, NULL,
			NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	while (want < XFER_ITEMS) {
		if (rxb_ring_pop(&xfer_ring, &it)) {
			zassert_equal((uint32_t)(uintptr_t)it.buf, want + 1U, "order at %u", want);
			zassert_equal(it.len, (uint16_t)want);
			zassert_equal(it.off, (uint16_t)(want >> 8));
			zassert_equal(it.plen, (uint16_t)(want >> 16));
			want++;
		} else {
			k_sleep(K_TICKS(1));
		}
	}
	k_thread_join(&xfer_thread, K_FOREVER);
	zassert_false(rxb_ring_pop(&xfer_ring, &it));
}

ZTEST_SUITE(rxbench, NULL, NULL, NULL, NULL, NULL);
