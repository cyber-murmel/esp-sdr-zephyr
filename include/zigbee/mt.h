/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Z-Stack MT (monitor and test) serial framing, as ZNP hosts such as zigpy-znp and
 * zigbee2mqtt speak it: SOF 0xFE, length, CMD0 (type in bits 7..5, subsystem in 4..0),
 * CMD1, payload, XOR of everything between SOF and FCS. Portable C, no Zephyr.
 */
#ifndef ZIGBEE_MT_H_
#define ZIGBEE_MT_H_

#include <stddef.h>
#include <stdint.h>

#define MT_SOF         0xFE
#define MT_MAX_PAYLOAD 250
/* SOF, length, CMD0, CMD1, payload, FCS. */
#define MT_MAX_WIRE    (MT_MAX_PAYLOAD + 5)

enum mt_type {
	MT_POLL = 0,
	MT_SREQ = 1,
	MT_AREQ = 2,
	MT_SRSP = 3,
};

enum mt_subsys {
	MT_RPC_ERROR = 0x00,
	MT_SYS = 0x01,
	MT_MAC = 0x02,
	MT_NWK = 0x03,
	MT_AF = 0x04,
	MT_ZDO = 0x05,
	MT_SAPI = 0x06,
	MT_UTIL = 0x07,
	MT_DEBUG = 0x08,
	MT_APP = 0x09,
	MT_APP_CNF = 0x0F,
	MT_ZGP = 0x15,
	MT_SUBSYS_COUNT = 0x20,
};

/* Error codes of the RPC error SRSP (subsystem 0, command 0). */
enum mt_rpc_error {
	MT_ERR_SUBSYSTEM = 0x01,
	MT_ERR_COMMAND = 0x02,
	MT_ERR_PARAMETER = 0x03,
	MT_ERR_LENGTH = 0x04,
};

/* Status bytes used in SRSP payloads (Z-Stack ZStatus_t). */
enum mt_status {
	MT_STATUS_SUCCESS = 0x00,
	MT_STATUS_FAILURE = 0x01,
	MT_STATUS_INVALID_PARAMETER = 0x02,
	MT_STATUS_NV_ITEM_UNINIT = 0x09,
	MT_STATUS_NV_OPER_FAILED = 0x0A,
	MT_STATUS_NV_BAD_ITEM_LEN = 0x0C,
	MT_STATUS_BUFFER_FULL = 0x11,
};

struct mt_frame {
	uint8_t type;
	uint8_t sub;
	uint8_t cmd;
	uint8_t len;
	uint8_t data[MT_MAX_PAYLOAD];
};

/* Byte-wise parser; a bad checksum rescans the bytes after the failed SOF. */
struct mt_parser {
	uint8_t raw[MT_MAX_WIRE];
	uint16_t n;
	/* Bad frames dropped (checksum or length) since init. */
	uint32_t errors;
	struct mt_frame frame;
};

enum mt_feed {
	MT_FEED_MORE = 0,
	MT_FEED_FRAME = 1,
};

void mt_parser_init(struct mt_parser *p);

/* Drop a partial frame: the transport calls this after an idle gap in the byte stream. */
static inline void mt_parser_reset(struct mt_parser *p)
{
	p->n = 0;
}

/* MT_FEED_FRAME: p->frame holds a complete frame, valid until the next feed. */
enum mt_feed mt_parser_feed(struct mt_parser *p, uint8_t byte);

/* Serialise @p f to @p out (at least f->len + 5 bytes); returns the byte count. */
size_t mt_encode(const struct mt_frame *f, uint8_t *out);

static inline uint16_t mt_get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t mt_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void mt_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void mt_put_u32(uint8_t *p, uint32_t v)
{
	mt_put_u16(p, (uint16_t)v);
	mt_put_u16(p + 2, (uint16_t)(v >> 16));
}

#endif /* ZIGBEE_MT_H_ */
