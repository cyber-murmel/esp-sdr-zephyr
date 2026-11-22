/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "netframe.h"

#define BE_HOST (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

static inline void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/*
 * Words are loaded as the CPU has them. On a little endian CPU that sums the bytes swapped, which
 * one's complement arithmetic carries through to the end: nf_fold() swaps back.
 */
uint64_t nf_sum(const void *p, size_t len, uint64_t acc)
{
	const uint8_t *b = p;

	if (((uintptr_t)b & 3U) == 0U) {
		const uint32_t *w = (const uint32_t *)__builtin_assume_aligned(b, 4);
		size_t n = len / 4U;

		for (size_t k = 0; k < n; k++) {
			acc += w[k];
		}
		b += n * 4U;
		len -= n * 4U;
	}
	while (len >= 2U) {
		uint16_t h;

		memcpy(&h, b, 2);
		acc += h;
		b += 2;
		len -= 2U;
	}
	if (len != 0U) {
		uint16_t h = 0;

		memcpy(&h, b, 1);
		acc += h;
	}
	return acc;
}

uint16_t nf_fold(uint64_t acc)
{
	uint32_t s;

	acc = (acc & 0xffffffffULL) + (acc >> 32);
	acc = (acc & 0xffffffffULL) + (acc >> 32);
	s = (uint32_t)acc;
	s = (s & 0xffffU) + (s >> 16);
	s = (s & 0xffffU) + (s >> 16);
	if (!BE_HOST) {
		s = ((s & 0xffU) << 8) | (s >> 8);
	}
	return (uint16_t)~s;
}

void nf_tmpl_init(struct nf_tmpl *t, bool v6, const uint8_t dmac[6], const uint8_t smac[6],
		  const uint8_t *saddr, const uint8_t *daddr, uint16_t sport, uint16_t dport)
{
	uint8_t *ip, *udp;
	size_t al = v6 ? 16U : 4U;

	memset(t, 0, sizeof(*t));
	t->v6 = v6;
	memcpy(t->b, dmac, 6);
	memcpy(t->b + 6, smac, 6);
	put16(t->b + 12, v6 ? 0x86ddU : 0x0800U);
	ip = t->b + NF_ETH_LEN;
	if (v6) {
		ip[0] = 0x60;
		ip[6] = 17; /* next header: UDP */
		ip[7] = 64; /* hop limit */
		memcpy(ip + 8, saddr, 16);
		memcpy(ip + 24, daddr, 16);
		udp = ip + 40;
		t->psum = nf_sum(saddr, 16, 0);
		t->psum = nf_sum(daddr, 16, t->psum);
	} else {
		ip[0] = 0x45;
		put16(ip + 6, 0x4000); /* don't fragment: the packets fit the MTU */
		ip[8] = 64;
		ip[9] = 17;
		memcpy(ip + 12, saddr, 4);
		memcpy(ip + 16, daddr, 4);
		udp = ip + 20;
	}
	(void)al;
	put16(udp, sport);
	put16(udp + 2, dport);
	t->len = (uint8_t)(udp + NF_UDP_LEN - t->b);
}

void nf_seal(struct nf_tmpl *t, uint8_t *hdr, size_t vrt_hdr, const uint8_t *body,
	     size_t body_len)
{
	nf_seal2(t, hdr, vrt_hdr, body, body_len, 0, 0);
}

void nf_seal2(struct nf_tmpl *t, uint8_t *hdr, size_t vrt_hdr, const uint8_t *body,
	      size_t body_len, uint64_t pre_sum, size_t pre_len)
{
	uint8_t *ip = hdr + NF_ETH_LEN;
	size_t udp_len = NF_UDP_LEN + vrt_hdr + body_len;

	if (t->v6 && t->zero_csum) {
		uint8_t *udp = ip + 40;

		put16(ip + 4, (uint16_t)udp_len);
		put16(udp + 4, (uint16_t)udp_len);
		put16(udp + 6, 0);
	} else if (t->v6) {
		uint8_t *udp = ip + 40;
		uint64_t acc;
		uint8_t pseudo[8];
		uint16_t c;

		put16(ip + 4, (uint16_t)udp_len);
		put16(udp + 4, (uint16_t)udp_len);
		put16(udp + 6, 0);
		/* length (32 bit) and next header, as the pseudo header has them */
		pseudo[0] = 0;
		pseudo[1] = 0;
		put16(pseudo + 2, (uint16_t)udp_len);
		pseudo[4] = 0;
		pseudo[5] = 0;
		pseudo[6] = 0;
		pseudo[7] = 17;
		acc = nf_sum(pseudo, 8, t->psum);
		acc = nf_sum(udp, NF_UDP_LEN + vrt_hdr, acc);
		acc += pre_sum;
		acc = nf_sum(body + pre_len, body_len - pre_len, acc);
		c = nf_fold(acc);
		put16(udp + 6, c == 0U ? 0xffffU : c);
	} else {
		uint8_t *udp = ip + 20;

		put16(ip + 2, (uint16_t)(20U + udp_len));
		t->id++;
		put16(ip + 4, t->id);
		put16(ip + 10, 0);
		put16(ip + 10, nf_fold(nf_sum(ip, 20, 0)));
		put16(udp + 4, (uint16_t)udp_len);
		put16(udp + 6, 0);
	}
}
