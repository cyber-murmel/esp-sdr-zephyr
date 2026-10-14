/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Zigbee NWK and APS frames and the security header they share: parse, build, protect and open
 * (CCM* with a 4 byte MIC). Portable C. The layouts are Zigbee PRO's (05-3474); the security
 * control field carries security level 0 on air and the real level (5) in the nonce and in
 * the authenticated header.
 */
#ifndef ZIGBEE_ZB_FRAME_H_
#define ZIGBEE_ZB_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zigbee/zb_crypto.h>

/* NWK frame types. */
#define ZB_NWK_DATA    0
#define ZB_NWK_COMMAND 1

/* NWK command ids. */
#define ZB_NWKC_ROUTE_REQUEST 0x01
#define ZB_NWKC_ROUTE_REPLY   0x02
#define ZB_NWKC_NETWORK_STATUS 0x03
#define ZB_NWKC_LEAVE         0x04
#define ZB_NWKC_REJOIN_REQ    0x06
#define ZB_NWKC_REJOIN_RSP    0x07
#define ZB_NWKC_LINK_STATUS   0x08

/* NWK broadcast addresses. */
#define ZB_BCAST_ALL        0xFFFF
#define ZB_BCAST_RX_ON_IDLE 0xFFFD
#define ZB_BCAST_ROUTERS    0xFFFC

/* Radius for application data (twice the network depth, as Z-Stack does). */
#define ZB_NWK_RADIUS 30

struct zb_nwk_hdr {
	uint8_t type;
	uint8_t proto_ver;
	uint8_t discover;
	bool security;
	bool multicast;
	bool src_route;
	bool has_ext_dst, has_ext_src;
	uint16_t dst, src;
	uint8_t radius, seq;
	uint8_t ext_dst[8], ext_src[8];
	/* Header length in bytes, up to the (security header or) payload. */
	uint8_t len;
};

/* -EINVAL when truncated; source routes are skipped, not interpreted. */
int zb_nwk_parse(const uint8_t *f, size_t n, struct zb_nwk_hdr *h);
/* Returns the length (the optional fields are written when flagged). */
size_t zb_nwk_build(const struct zb_nwk_hdr *h, uint8_t *out);

/* APS frame types and delivery modes. */
#define ZB_APS_DATA    0
#define ZB_APS_COMMAND 1
#define ZB_APS_ACK     2
#define ZB_APS_UNICAST   0
#define ZB_APS_BROADCAST 2
#define ZB_APS_GROUP     3

#define ZB_APSC_TRANSPORT_KEY 0x05
#define ZB_APSC_REQUEST_KEY   0x08

struct zb_aps_hdr {
	uint8_t type;
	uint8_t delivery;
	bool ack_format, security, ack_req, ext_hdr;
	uint8_t dst_ep, src_ep;
	uint16_t group, cluster, profile;
	uint8_t counter;
	uint8_t len;
};

int zb_aps_parse(const uint8_t *f, size_t n, struct zb_aps_hdr *h);
size_t zb_aps_build(const struct zb_aps_hdr *h, uint8_t *out);

/*
 * The auxiliary security header: control (key id, extended nonce), frame counter, the sender's
 * IEEE address and, for the network key, its sequence number.
 */
#define ZB_KEYID_DATA      0
#define ZB_KEYID_NETWORK   1
#define ZB_KEYID_TRANSPORT 2
#define ZB_KEYID_LOAD      3

struct zb_sec_hdr {
	uint8_t key_id;
	uint32_t counter;
	uint8_t src[8];
	uint8_t key_seq;
	uint8_t len;
};

#define ZB_SEC_MAX_LEN 14

/*
 * Protect a frame: @p buf holds @p hdr_len bytes of header (security flag set by the caller)
 * followed by @p plen bytes of plain payload. The auxiliary header goes between them, the
 * payload is encrypted and the MIC appended. Returns the new length, 0 if it does not fit
 * @p cap.
 */
size_t zb_sec_protect(uint8_t *buf, size_t cap, size_t hdr_len, size_t plen,
		      const uint8_t key[ZB_KEY_LEN], uint8_t key_id, uint8_t key_seq,
		      uint32_t counter, const uint8_t src[8]);

/*
 * Parse the auxiliary header at @p hdr_len (without checking the MIC): -EINVAL if malformed.
 */
int zb_sec_parse(const uint8_t *f, size_t n, size_t hdr_len, struct zb_sec_hdr *s);

/*
 * Verify and decrypt in place with @p key. On success the plain payload is at
 * buf + hdr_len + s->len, of *plen bytes. -EBADMSG when the MIC fails.
 */
int zb_sec_open(uint8_t *buf, size_t n, size_t hdr_len, const uint8_t key[ZB_KEY_LEN],
		const struct zb_sec_hdr *s, size_t *plen);

/* The APS Transport-Key command payload for a standard network key; returns its length (35). */
size_t zb_aps_transport_key_payload(uint8_t *out, const uint8_t key[ZB_KEY_LEN], uint8_t key_seq,
				    const uint8_t dst[8], const uint8_t src[8]);

#endif /* ZIGBEE_ZB_FRAME_H_ */
