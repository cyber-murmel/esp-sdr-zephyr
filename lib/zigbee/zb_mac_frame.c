/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MAC frame codec: see include/zigbee/zb_mac_frame.h.
 */
#include <errno.h>
#include <string.h>

#include <zigbee/mt.h>
#include <zigbee/zb_mac_frame.h>

/* Frame control bits (bit 0 = LSB of the first byte on air). */
#define FCF_TYPE(f)  ((f) & 7U)
#define FCF_SEC      BIT(3)
#define FCF_PEND     BIT(4)
#define FCF_AR       BIT(5)
#define FCF_COMP     BIT(6)
#define FCF_DST(f)   (((f) >> 10) & 3U)
#define FCF_VER(f)   (((f) >> 12) & 3U)
#define FCF_SRC(f)   (((f) >> 14) & 3U)

/* Beacon superframe specification (open: permit set, PAN coordinator set, no beacon order). */
#define SF_BASE      0x0FFFU
#define SF_PAN_COORD BIT(14)
#define SF_PERMIT    BIT(15)

/* Zigbee beacon payload: protocol id 0, profile 2 (PRO), nwkcProtocolVersion 2. */
#define ZB_BEACON_PAYLOAD 15

static uint16_t get16(const uint8_t *p)
{
	return mt_get_u16(p);
}

static uint8_t *put16(uint8_t *p, uint16_t v)
{
	mt_put_u16(p, v);
	return p + 2;
}

static uint8_t *put_fcf(uint8_t *p, unsigned int type, bool pend, bool ar, bool comp,
			unsigned int dst, unsigned int src)
{
	uint16_t f = type | (pend ? FCF_PEND : 0U) | (ar ? FCF_AR : 0U) | (comp ? FCF_COMP : 0U) |
		     (dst << 10) | (src << 14);

	return put16(p, f);
}

int zb_mac_parse(const uint8_t *f, size_t n, struct zb_mac_hdr *h)
{
	size_t o = 3;
	uint16_t fcf;

	if (n < 3U) {
		return -EINVAL;
	}
	memset(h, 0, sizeof(*h));
	fcf = get16(f);
	h->type = FCF_TYPE(fcf);
	h->seq = f[2];
	h->ver = FCF_VER(fcf);
	h->sec = (fcf & FCF_SEC) != 0U;
	h->pend = (fcf & FCF_PEND) != 0U;
	h->ar = (fcf & FCF_AR) != 0U;
	h->comp = (fcf & FCF_COMP) != 0U;
	h->dst_mode = FCF_DST(fcf);
	h->src_mode = FCF_SRC(fcf);

	if (h->ver >= 2U || h->sec) {
		return -ENOTSUP;
	}
	if (h->dst_mode == 1U || h->src_mode == 1U || h->type > ZB_FT_CMD) {
		return -EINVAL;
	}
	if (h->comp && h->dst_mode == 0U) {
		return -EINVAL;
	}

	if (h->dst_mode != 0U) {
		size_t need = 2U + (h->dst_mode == ZB_ADDR_EXT ? 8U : 2U);

		if (n < o + need) {
			return -EINVAL;
		}
		h->dst_pan = get16(&f[o]);
		if (h->dst_mode == ZB_ADDR_SHORT) {
			h->dst_short = get16(&f[o + 2]);
		} else {
			memcpy(h->dst_ext, &f[o + 2], 8);
		}
		o += need;
	}
	if (h->src_mode != 0U) {
		bool pan_present = !(h->comp && h->dst_mode != 0U);
		size_t need = (pan_present ? 2U : 0U) + (h->src_mode == ZB_ADDR_EXT ? 8U : 2U);

		if (n < o + need) {
			return -EINVAL;
		}
		h->src_pan = pan_present ? get16(&f[o]) : h->dst_pan;
		o += pan_present ? 2U : 0U;
		if (h->src_mode == ZB_ADDR_SHORT) {
			h->src_short = get16(&f[o]);
		} else {
			memcpy(h->src_ext, &f[o], 8);
		}
		o += h->src_mode == ZB_ADDR_EXT ? 8U : 2U;
	}
	h->len = (uint8_t)o;
	return 0;
}

int zb_mac_parse_beacon(const struct zb_mac_hdr *h, const uint8_t *p, size_t n,
			struct zb_beacon_info *b)
{
	uint16_t sf;
	size_t o = 0;
	uint8_t b2;

	if (h->type != ZB_FT_BEACON || h->src_mode != ZB_ADDR_SHORT || n < 4U) {
		return -EINVAL;
	}
	memset(b, 0, sizeof(*b));
	sf = get16(p);
	/* GTS specification (count in bits 0-2; a direction byte and 3 bytes per descriptor). */
	o = 3;
	if ((p[2] & 7U) != 0U) {
		o += 1U + 3U * (p[2] & 7U);
	}
	if (n < o + 1U) {
		return -EINVAL;
	}
	/* Pending address specification: short count bits 0-2, extended count bits 4-6. */
	o += 1U + 2U * (p[o] & 7U) + 8U * ((p[o] >> 4) & 7U);
	if (n < o + ZB_BEACON_PAYLOAD || p[o] != 0U) {
		return -EINVAL;
	}
	p += o;
	b->pan = h->src_pan;
	b->src_short = h->src_short;
	b->permit = (sf & SF_PERMIT) != 0U;
	b->pan_coord = (sf & SF_PAN_COORD) != 0U;
	b->stack_profile = p[1] & 15U;
	b->proto_ver = p[1] >> 4;
	b2 = p[2];
	b->router_cap = (b2 & BIT(2)) != 0U;
	b->depth = (b2 >> 3) & 15U;
	b->ed_cap = (b2 & BIT(7)) != 0U;
	memcpy(b->epid, &p[3], 8);
	b->upd = p[14];
	return 0;
}

size_t zb_mac_build_beacon_req(uint8_t *out, uint8_t seq)
{
	uint8_t *p = put_fcf(out, ZB_FT_CMD, false, false, false, ZB_ADDR_SHORT, ZB_ADDR_NONE);

	*p++ = seq;
	p = put16(p, ZB_PAN_BROADCAST);
	p = put16(p, ZB_SHORT_BROADCAST);
	*p++ = ZB_CMD_BEACON_REQ;
	return (size_t)(p - out);
}

size_t zb_mac_build_beacon(uint8_t *out, uint8_t bsn, const struct zb_beacon_info *b)
{
	uint8_t *p = put_fcf(out, ZB_FT_BEACON, false, false, false, ZB_ADDR_NONE, ZB_ADDR_SHORT);
	uint16_t sf = SF_BASE | (b->pan_coord ? SF_PAN_COORD : 0U) | (b->permit ? SF_PERMIT : 0U);

	*p++ = bsn;
	p = put16(p, b->pan);
	p = put16(p, b->src_short);
	p = put16(p, sf);
	*p++ = 0; /* GTS specification: no GTS */
	*p++ = 0; /* pending address specification: none */
	*p++ = 0; /* protocol id */
	*p++ = (uint8_t)((b->proto_ver << 4) | (b->stack_profile & 15U));
	*p++ = (uint8_t)((b->router_cap ? BIT(2) : 0U) | ((b->depth & 15U) << 3) |
			 (b->ed_cap ? BIT(7) : 0U));
	memcpy(p, b->epid, 8);
	p += 8;
	*p++ = 0xFF; /* tx offset: not supported */
	*p++ = 0xFF;
	*p++ = 0xFF;
	*p++ = b->upd;
	return (size_t)(p - out);
}

size_t zb_mac_build_assoc_req(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
			      const uint8_t src_ext[8], uint8_t cap)
{
	uint8_t *p = put_fcf(out, ZB_FT_CMD, false, true, false, ZB_ADDR_SHORT, ZB_ADDR_EXT);

	*p++ = seq;
	p = put16(p, pan);
	p = put16(p, dst_short);
	p = put16(p, ZB_PAN_BROADCAST);
	memcpy(p, src_ext, 8);
	p += 8;
	*p++ = ZB_CMD_ASSOC_REQ;
	*p++ = cap;
	return (size_t)(p - out);
}

size_t zb_mac_build_assoc_rsp(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t dst_ext[8],
			      const uint8_t src_ext[8], uint16_t short_addr, uint8_t status)
{
	uint8_t *p = put_fcf(out, ZB_FT_CMD, false, true, true, ZB_ADDR_EXT, ZB_ADDR_EXT);

	*p++ = seq;
	p = put16(p, pan);
	memcpy(p, dst_ext, 8);
	p += 8;
	memcpy(p, src_ext, 8);
	p += 8;
	*p++ = ZB_CMD_ASSOC_RSP;
	p = put16(p, short_addr);
	*p++ = status;
	return (size_t)(p - out);
}

size_t zb_mac_build_data_req(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
			     const uint8_t *src_ext, const uint16_t *src_short)
{
	uint8_t *p = put_fcf(out, ZB_FT_CMD, false, true, true, ZB_ADDR_SHORT,
			     src_short != NULL ? ZB_ADDR_SHORT : ZB_ADDR_EXT);

	*p++ = seq;
	p = put16(p, pan);
	p = put16(p, dst_short);
	if (src_short != NULL) {
		p = put16(p, *src_short);
	} else {
		memcpy(p, src_ext, 8);
		p += 8;
	}
	*p++ = ZB_CMD_DATA_REQ;
	return (size_t)(p - out);
}

size_t zb_mac_build_data(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
			 uint16_t src_short, bool ar, const uint8_t *payload, size_t plen)
{
	uint8_t *p = put_fcf(out, ZB_FT_DATA, false, ar, true, ZB_ADDR_SHORT, ZB_ADDR_SHORT);

	*p++ = seq;
	p = put16(p, pan);
	p = put16(p, dst_short);
	p = put16(p, src_short);
	memcpy(p, payload, plen);
	return (size_t)(p - out) + plen;
}

size_t zb_mac_build_empty_data(uint8_t *out, uint8_t seq, uint16_t pan, const uint16_t *dst_short,
			       const uint8_t *dst_ext, uint16_t src_short)
{
	uint8_t *p = put_fcf(out, ZB_FT_DATA, false, true, true,
			     dst_short != NULL ? ZB_ADDR_SHORT : ZB_ADDR_EXT, ZB_ADDR_SHORT);

	*p++ = seq;
	p = put16(p, pan);
	if (dst_short != NULL) {
		p = put16(p, *dst_short);
	} else {
		memcpy(p, dst_ext, 8);
		p += 8;
	}
	p = put16(p, src_short);
	return (size_t)(p - out);
}
