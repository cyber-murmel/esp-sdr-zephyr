/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive hook's filter (tx_net.c) as a pure function of a frame and the session: no Zephyr, no
 * state, so that the host test (tests/unit/txs/src/test_filter.c) feeds it frames. Header only; it is
 * inlined into the hook, which sits in IRAM.
 *
 * A frame for our MAC, IPv4 (no options, not a fragment) or IPv6 (UDP as the next header), UDP to the
 * sample port is the stream's: it either matches (the subscriber's address, port and the session's
 * stream ID) and is taken, or it is dropped for its reason. Anything else is the stack's.
 */

#ifndef TX_FILTER_H_
#define TX_FILTER_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TXF_PORT 4994U

/** What the session knows; the control thread writes it while no run takes data. */
struct txf_state {
	bool v6;
	uint16_t sport;   /* the subscriber's port */
	uint8_t mac[6];   /* ours */
	uint8_t sid[4];   /* the stream ID as the packet has it (big endian) */
	uint8_t src[16];  /* the subscriber's address (4 bytes for IPv4) */
	uint8_t dst[16];  /* the address it sends to: ours */
};

enum txf_verdict {
	TXF_STACK,   /* not ours: the stack */
	TXF_LEN,     /* for the port, lengths inconsistent (drop) */
	TXF_IDLE,    /* for the port, no run takes data (drop) */
	TXF_SRC,     /* for the port, not from the subscriber's address and port (drop) */
	TXF_SID,     /* for the port, another stream ID (drop) */
	TXF_TAKE,    /* the stream's: the VITA packet is at *off, *len bytes */
};

static inline uint16_t txf_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**
 * @param d the Ethernet frame from the destination address on, len bytes (no FCS)
 * @param accept a run takes data
 * @param off, @param vlen where the VITA packet starts in the frame and its length, for TXF_TAKE
 */
static inline enum txf_verdict txf_classify(const struct txf_state *s, bool accept, const uint8_t *d,
					    uint16_t len, uint16_t *off, uint16_t *vlen)
{
	unsigned int hdr, alen, sa;
	uint16_t ulen, iplen;
	bool v6;

	/* Ethernet 14, IPv4 20 or IPv6 40, UDP 8, a VITA header of 16 and a word */
	if (len < 14U + 20U + 8U + 20U || memcmp(d, s->mac, 6) != 0) {
		return TXF_STACK;
	}
	if (d[12] == 0x08U && d[13] == 0x00U) {
		/* IPv4: 5 words of header, UDP, no more fragments and a zero offset */
		if (d[14] != 0x45U || d[23] != 17U || ((d[20] & 0x3fU) | d[21]) != 0U) {
			return TXF_STACK;
		}
		v6 = false;
		hdr = 14U + 20U + 8U;
		alen = 4U;
		sa = 14U + 12U;
		iplen = (uint16_t)(txf_be16(d + 16) - 20U);
	} else if (d[12] == 0x86U && d[13] == 0xddU) {
		if (len < 14U + 40U + 8U + 20U || (d[14] & 0xf0U) != 0x60U || d[20] != 17U) {
			return TXF_STACK;
		}
		v6 = true;
		hdr = 14U + 40U + 8U;
		alen = 16U;
		sa = 14U + 8U;
		iplen = txf_be16(d + 18);
	} else {
		return TXF_STACK;
	}
	if (txf_be16(d + hdr - 6) != TXF_PORT) {
		return TXF_STACK;
	}
	/* for the sample port: from here on the frame is the stream's, taken or dropped */
	ulen = txf_be16(d + hdr - 4);
	if (ulen < 8U + 20U || ulen != iplen || hdr - 8U + ulen > len) {
		return TXF_LEN;
	}
	if (!accept) {
		return TXF_IDLE;
	}
	if (v6 != s->v6 || txf_be16(d + hdr - 8) != s->sport || memcmp(d + sa, s->src, alen) != 0 ||
	    memcmp(d + sa + alen, s->dst, alen) != 0) {
		return TXF_SRC;
	}
	if (memcmp(d + hdr + 4, s->sid, 4) != 0) {
		return TXF_SID;
	}
	*off = (uint16_t)hdr;
	*vlen = (uint16_t)(ulen - 8U);
	return TXF_TAKE;
}

#endif
