/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * IEEE 802.15.4 MAC frames as a Zigbee coordinator needs them (frame version 0, no MAC
 * security): parse any frame header, build and parse beacons, build the commands and data
 * frames the coordinator sends. Buffers never include the FCS (the radio adds and checks it).
 * Portable C, no Zephyr. Layouts and examples: tmp/zstack/phase2/mac_final.md.
 */
#ifndef ZIGBEE_ZB_MAC_FRAME_H_
#define ZIGBEE_ZB_MAC_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The radio takes at most 125 bytes (127 with the FCS and the 2 byte PHR margin of the driver). */
#define ZB_MAC_MAX_FRAME 125

#ifndef BIT
#define BIT(n) (1U << (n))
#endif

enum zb_mac_ftype {
	ZB_FT_BEACON = 0,
	ZB_FT_DATA = 1,
	ZB_FT_ACK = 2,
	ZB_FT_CMD = 3,
};

enum zb_mac_cmd {
	ZB_CMD_ASSOC_REQ = 0x01,
	ZB_CMD_ASSOC_RSP = 0x02,
	ZB_CMD_DISASSOC = 0x03,
	ZB_CMD_DATA_REQ = 0x04,
	ZB_CMD_PAN_CONFLICT = 0x05,
	ZB_CMD_ORPHAN = 0x06,
	ZB_CMD_BEACON_REQ = 0x07,
	ZB_CMD_REALIGN = 0x08,
	ZB_CMD_GTS_REQ = 0x09,
};

enum zb_addr_mode {
	ZB_ADDR_NONE = 0,
	ZB_ADDR_SHORT = 2,
	ZB_ADDR_EXT = 3,
};

enum zb_assoc_status {
	ZB_ASSOC_OK = 0x00,
	ZB_ASSOC_PAN_FULL = 0x01,
	ZB_ASSOC_DENIED = 0x02,
};

#define ZB_PAN_BROADCAST   0xFFFF
#define ZB_SHORT_BROADCAST 0xFFFF
#define ZB_SHORT_NONE      0xFFFE

/* Capability information of an association request. */
#define ZB_CAP_FFD        BIT(1)
#define ZB_CAP_MAINS      BIT(2)
#define ZB_CAP_RX_IDLE    BIT(3)
#define ZB_CAP_SECURITY   BIT(6)
#define ZB_CAP_ALLOC_ADDR BIT(7)

struct zb_mac_hdr {
	uint8_t type;
	uint8_t seq;
	uint8_t ver;
	bool sec, pend, ar, comp;
	uint8_t dst_mode, src_mode;
	uint16_t dst_pan, src_pan;
	uint16_t dst_short, src_short;
	/* Little endian, as on air. */
	uint8_t dst_ext[8], src_ext[8];
	/* Header length in bytes: the payload (for a command, the command id first) follows. */
	uint8_t len;
};

/*
 * Parse the MAC header of @p n bytes (no FCS). Returns 0, -EINVAL (malformed) or -ENOTSUP
 * (frame version 2 or 3, or security enabled: the radio would not ACK them either).
 */
int zb_mac_parse(const uint8_t *f, size_t n, struct zb_mac_hdr *h);

/* The Zigbee view of a beacon, as received or to send. */
struct zb_beacon_info {
	uint16_t pan;
	uint16_t src_short;
	uint8_t channel;
	bool permit;
	bool pan_coord;
	uint8_t stack_profile;
	uint8_t proto_ver;
	bool router_cap;
	bool ed_cap;
	uint8_t depth;
	uint8_t upd;
	/* Little endian, as on air. */
	uint8_t epid[8];
	uint8_t lqi;
	int8_t rssi;
};

/* Decode the beacon payload that follows the header @p h; -EINVAL if it is no Zigbee beacon. */
int zb_mac_parse_beacon(const struct zb_mac_hdr *h, const uint8_t *payload, size_t n,
			struct zb_beacon_info *b);

/* Builders return the frame length. All frames use version 0. */
size_t zb_mac_build_beacon_req(uint8_t *out, uint8_t seq);
size_t zb_mac_build_beacon(uint8_t *out, uint8_t bsn, const struct zb_beacon_info *b);
size_t zb_mac_build_assoc_req(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
			      const uint8_t src_ext[8], uint8_t cap);
size_t zb_mac_build_assoc_rsp(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t dst_ext[8],
			      const uint8_t src_ext[8], uint16_t short_addr, uint8_t status);
/* Data request: from a joiner that only has its extended address (src_short == NULL) or a poll. */
size_t zb_mac_build_data_req(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
			     const uint8_t *src_ext, const uint16_t *src_short);
/* Data frame between short addresses inside one PAN (compression); @p ar for unicast. */
size_t zb_mac_build_data(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
			 uint16_t src_short, bool ar, const uint8_t *payload, size_t plen);
/* Zero-length data frame answering a data request that finds nothing queued. */
size_t zb_mac_build_empty_data(uint8_t *out, uint8_t seq, uint16_t pan, const uint16_t *dst_short,
			       const uint8_t *dst_ext, uint16_t src_short);

#endif /* ZIGBEE_ZB_MAC_FRAME_H_ */
