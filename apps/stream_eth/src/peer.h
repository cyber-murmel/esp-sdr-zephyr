/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef STREAM_ETH_PEER_H_
#define STREAM_ETH_PEER_H_

#include <stdbool.h>
#include <stdint.h>

/** Where a stream goes: from the datagram that asked. */
struct peer {
	bool v6;
	uint8_t mac[6];
	uint8_t addr[16];
	/** The address the host sent to, which the packets come from. */
	uint8_t local[16];
	uint16_t port;
	/** IPv6: the host takes a zero UDP checksum (it asked, UDP_NO_CHECK6_RX); see README. */
	bool nocsum;
};

#endif
