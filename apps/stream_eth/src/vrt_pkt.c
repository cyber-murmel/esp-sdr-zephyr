/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "vrt_pkt.h"

#define PS_PER_S 1000000000000ULL

static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

void vrt_clock_seek(struct vrt_clock *c, uint32_t rate, uint64_t index)
{
	c->rate = rate;
	c->ps_unit = (PS_PER_S % rate) == 0U ? (uint32_t)(PS_PER_S / rate) : 0U;
	c->index = index;
	c->sec = (uint32_t)(index / rate);
	c->sub = (uint32_t)(index % rate);
}

void vrt_clock_advance(struct vrt_clock *c, uint32_t n)
{
	c->index += n;
	c->sub += n;
	if (c->sub >= c->rate) {
		c->sub -= c->rate;
		c->sec++;
	}
}

uint64_t vrt_clock_ps(const struct vrt_clock *c)
{
	if (c->ps_unit != 0U) {
		return (uint64_t)c->sub * c->ps_unit;
	}
	/* sub * (q * rate + r) / rate, each part below 2^52 */
	return (uint64_t)c->sub * (PS_PER_S / c->rate) +
	       (uint64_t)c->sub * (uint32_t)(PS_PER_S % c->rate) / c->rate;
}

size_t vrt_payload_bytes(unsigned int mtu, bool ipv6)
{
	size_t fixed = (ipv6 ? 40U : 20U) + 8U + VRT_DATA_HDR_BYTES + VRT_TRAILER_BYTES;

	if (mtu <= fixed + 4U) {
		return 0;
	}
	return (mtu - fixed) & ~(size_t)3U;
}

void vrt_data_header(uint8_t *out, uint32_t stream_id, uint8_t count, uint16_t words,
		     uint32_t sec, uint64_t ps)
{
	/* type 1 (data with stream ID), trailer, TSI other (3), TSF real time (2) */
	uint32_t hdr = (1U << 28) | (1U << 26) | (3U << 22) | (2U << 20) |
		       ((uint32_t)(count & 0xfU) << 16) | words;

	put_be32(out, hdr);
	put_be32(out + 4, stream_id);
	put_be32(out + 8, sec);
	put_be32(out + 12, (uint32_t)(ps >> 32));
	put_be32(out + 16, (uint32_t)ps);
}

uint32_t vrt_trailer_word(bool agc, bool over_range, bool sample_loss)
{
	/* enables in 31..20, indicators in 19..8, same order: valid data 30, AGC/MGC 28,
	 * over-range 25, sample loss 24
	 */
	uint32_t en = (1U << 30) | (1U << 28) | (1U << 25) | (1U << 24);
	uint32_t ind = (1U << 30) | (agc ? (1U << 28) : 0U) | (over_range ? (1U << 25) : 0U) |
		       (sample_loss ? (1U << 24) : 0U);

	return en | (ind >> 12);
}

void vrt_put_trailer(uint8_t *out, uint32_t word)
{
	put_be32(out, word);
}

bool vrt_clipped(const uint8_t *payload, size_t bytes, unsigned int stride)
{
	const uint32_t *w = (const uint32_t *)__builtin_assume_aligned(payload, 4);
	size_t n = bytes / 4U;
	uint32_t hit = 0;

	if (stride == 0U) {
		stride = 1U;
	}
	for (size_t k = 0; k < n; k += stride) {
		uint32_t a = w[k] ^ 0x7f7f7f7fU, b = w[k] ^ 0x80808080U;

		/* a zero byte in a or b: the sample byte is 0x7f or 0x80 */
		hit |= ((a - 0x01010101U) & ~a & 0x80808080U) |
		       ((b - 0x01010101U) & ~b & 0x80808080U);
	}
	return hit != 0U;
}
