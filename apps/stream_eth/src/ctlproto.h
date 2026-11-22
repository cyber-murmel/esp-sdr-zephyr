/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The subscription protocol of UDP port 4992, without a network stack under it (so it runs on the
 * host): the board streams only to a host that has shown it can receive at its address.
 *
 *   host -> board  "VRTS"                  board -> host  "VRTC" + 8 byte cookie
 *   host -> board  "VRTS" + cookie         subscribes or renews;  board -> host "VRTK" on a new
 *                                          subscription, "VRTB" when another host has the stream
 *   host -> board  "VRTT" + cookie         takes the stream over from another host
 *   (VRTS and VRTT may have a 13th byte after the cookie: bit 0 asks for a zero UDP checksum over
 *   IPv6, for a host that set UDP_NO_CHECK6_RX; the board does it only if built for it.)
 *   host -> board  "VRTX" + cookie         stops (from the subscriber only)
 *
 * The cookie is SipHash-2-4 of the source address, port, source MAC, the address the host sent to,
 * the interface and a 30 s epoch under a key made at boot: the board keeps no state for a host that has not subscribed. The current and the
 * previous epoch are accepted. A VITA 49.2 command packet is handed back to the caller with a flag
 * telling whether it comes from the subscriber.
 */
#ifndef STREAM_ETH_CTLPROTO_H_
#define STREAM_ETH_CTLPROTO_H_

#include <stddef.h>
#include <stdint.h>

#include "peer.h"

#define CTL_COOKIE_BYTES 8U
#define CTL_EPOCH_MS     30000
#define CTL_MAX_REPLY    12U

enum ctl_act { CTL_IGNORE, CTL_REPLY, CTL_CMD };
enum ctl_evt { CTL_EVT_NONE, CTL_EVT_SUBSCRIBED, CTL_EVT_RENEWED, CTL_EVT_STOPPED, CTL_EVT_TAKEOVER };

struct ctl_out {
	enum ctl_act act;
	enum ctl_evt evt;
	uint8_t reply[CTL_MAX_REPLY];
	uint8_t reply_len;
	/** For CTL_CMD: the packet comes from the live subscriber. */
	bool is_sub;
	/** The subscribe asked for a zero UDP checksum over IPv6 (the flag byte after the cookie). */
	bool nocsum;
};

/** Per-source budgets: a small table, the least recently seen source goes. */
struct ctl_src {
	bool used, v6;
	uint8_t addr[16];
	int64_t win, seen;
	uint32_t n_adm, n_c, n_q;
};
#define CTL_NSRC 8

struct ctl_proto {
	struct ctl_src src[CTL_NSRC];
	/** The live subscriber's own counters (full match: address, port, MAC, local address). */
	struct ctl_src subs;
	int64_t gwin, awin;
	uint32_t gcnt, acnt;
	uint8_t key[16];
	uint32_t ifindex;
	struct peer sub;
	bool live;
	/** `stream stop` on the shell: nobody may subscribe until it is lifted. */
	bool blocked;
	int64_t last_ms;
	int64_t silent_ms;
	int64_t qwin, gwin2;
	uint32_t qcnt, gcnt2;
};

uint64_t siphash24(const uint8_t key[16], const uint8_t *in, size_t len);
void ctl_proto_init(struct ctl_proto *c, const uint8_t key[16], uint32_t ifindex, int64_t silent_ms);
void ctl_proto_cookie(const struct ctl_proto *c, const struct peer *p, uint32_t epoch, uint8_t out[8]);
bool ctl_peer_same(const struct peer *a, const struct peer *b);
void ctl_proto_dgram(struct ctl_proto *c, const struct peer *p, const uint8_t *d, size_t len,
		     int64_t now_ms, struct ctl_out *o);
/** True once when the subscriber has been silent too long: it is gone. */
bool ctl_proto_expire(struct ctl_proto *c, int64_t now_ms);
/**
 * Another host's query-state command: 2 answers a second to a source and 10 a second in all (the
 * subscriber has no share in this: it asks nothing it needs).
 */
bool ctl_proto_query_ok(struct ctl_proto *c, const struct peer *p, int64_t now_ms);

/**
 * Before anything is queued or looked at: may this datagram's source be taken at all? 20 a
 * second from a source and 300 in all; the live subscriber has 60 a second of its own, outside
 * the 300, so that it is never shut out by a flood from others.
 */
bool ctl_proto_admit(struct ctl_proto *c, const struct peer *p, int64_t now_ms);

#endif
