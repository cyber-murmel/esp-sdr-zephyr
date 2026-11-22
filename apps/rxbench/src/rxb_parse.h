/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Which frames the receive hook takes: Ethernet, then IPv4 (any header length, not a fragment) or
 * IPv6 (UDP straight after the fixed header: no extension headers, so no fragment header), then
 * UDP to one port, unicast to the board's MAC address. Everything else is the stack's. Plain C.
 */

#ifndef RXB_PARSE_H_
#define RXB_PARSE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RXB_ETH_HLEN   14U
#define RXB_ETYPE_IPV4 0x0800U
#define RXB_ETYPE_IPV6 0x86ddU
#define RXB_IP_UDP     17U
#define RXB_IPV4_MINHL 20U
#define RXB_IPV6_HLEN  40U
#define RXB_UDP_HLEN   8U
/* more fragments (0x2000) and the fragment offset (0x1fff): the don't-fragment bit is no fragment */
#define RXB_IPV4_FRAG  0x3fffU

/* Where the payload sits in a taken frame. */
struct rxb_hit {
	uint16_t off;	/* of the UDP payload in the frame */
	uint16_t plen;	/* its length (from the UDP length field) */
	uint8_t ip;	/* 4 or 6 */
};

static inline uint16_t rxb_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* 4 (IPv4) or 6 (IPv6) when the frame is UDP to port for mac, filling *hit; else 0. */
static inline int rxb_parse(const uint8_t *f, size_t len, const uint8_t mac[6], uint16_t port,
			    struct rxb_hit *hit)
{
	const uint8_t *ip;
	const uint8_t *udp;
	size_t avail, hl, ulen, room;
	uint16_t etype;
	int ver;

	if (len < RXB_ETH_HLEN || memcmp(f, mac, 6) != 0) {
		return 0;
	}
	etype = rxb_be16(f + 12);
	ip = f + RXB_ETH_HLEN;
	avail = len - RXB_ETH_HLEN;

	if (etype == RXB_ETYPE_IPV4) {
		if (avail < RXB_IPV4_MINHL || (ip[0] >> 4) != 4) {
			return 0;
		}
		hl = (size_t)(ip[0] & 0xfU) * 4U;
		room = rxb_be16(ip + 2);	/* the IP packet; the frame may be padded beyond it */
		if (hl < RXB_IPV4_MINHL || room > avail || room < hl + RXB_UDP_HLEN ||
		    ip[9] != RXB_IP_UDP || (rxb_be16(ip + 6) & RXB_IPV4_FRAG) != 0U) {
			return 0;
		}
		room -= hl;
		ver = 4;
	} else if (etype == RXB_ETYPE_IPV6) {
		if (avail < RXB_IPV6_HLEN + RXB_UDP_HLEN || (ip[0] >> 4) != 6 ||
		    ip[6] != RXB_IP_UDP) {
			return 0;
		}
		hl = RXB_IPV6_HLEN;
		room = rxb_be16(ip + 4);
		if (room > avail - hl || room < RXB_UDP_HLEN) {
			return 0;
		}
		ver = 6;
	} else {
		return 0;
	}

	udp = ip + hl;
	ulen = rxb_be16(udp + 4);
	if (rxb_be16(udp + 2) != port || ulen < RXB_UDP_HLEN || ulen > room) {
		return 0;
	}
	hit->off = (uint16_t)(RXB_ETH_HLEN + hl + RXB_UDP_HLEN);
	hit->plen = (uint16_t)(ulen - RXB_UDP_HLEN);
	hit->ip = (uint8_t)ver;
	return ver;
}

#endif /* RXB_PARSE_H_ */
