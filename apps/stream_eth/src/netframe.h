/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ethernet, IPv4 or IPv6 and UDP headers made once per subscriber as a template, and filled per
 * packet with the lengths and checksums only. No Zephyr in here, so it runs on the host.
 *
 * IPv4 carries a UDP checksum of 0 (legal); IPv6 needs it and gets it, summed over the VRT
 * header and the body.
 */

#ifndef STREAM_ETH_NETFRAME_H_
#define STREAM_ETH_NETFRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NF_ETH_LEN  14U
#define NF_UDP_LEN  8U
/* Ethernet, IPv6, UDP: the largest of them. */
#define NF_HDR_MAX  (NF_ETH_LEN + 40U + NF_UDP_LEN)

struct nf_tmpl {
	uint8_t b[NF_HDR_MAX];
	/** Template bytes in use: Ethernet, IP and UDP headers. */
	uint8_t len;
	bool v6;
	/** IPv6: leave the UDP checksum 0 (RFC 6936, on-link, the receiver must allow it). */
	bool zero_csum;
	uint16_t id;
	/** Sum of the pseudo header's addresses and next header, in the domain of nf_sum(). */
	uint64_t psum;
};

/** The sum of 16 bit words of the bytes, big endian, as a 64 bit accumulator. */
uint64_t nf_sum(const void *p, size_t len, uint64_t acc);

/** Fold and complement an accumulator of nf_sum(): the checksum as a host number. */
uint16_t nf_fold(uint64_t acc);

/**
 * @param saddr 4 or 16 bytes.
 * @param daddr 4 or 16 bytes.
 */
void nf_tmpl_init(struct nf_tmpl *t, bool v6, const uint8_t dmac[6], const uint8_t smac[6],
		  const uint8_t *saddr, const uint8_t *daddr, uint16_t sport, uint16_t dport);

/**
 * Finish a frame: `hdr` holds the template's bytes followed by `vrt_hdr` bytes of VRT header
 * (the caller's), and the datagram goes on in `body` (sample data and the trailer), whose length
 * is `body_len`. Sets the IP and UDP lengths, the IPv4 header checksum and ID, and the IPv6 UDP
 * checksum.
 */
void nf_seal(struct nf_tmpl *t, uint8_t *hdr, size_t vrt_hdr, const uint8_t *body,
	     size_t body_len);

/**
 * nf_seal() when the sum of the first `pre_len` bytes of the body (an even number) is known:
 * pre_sum, as nf_sum() gives it (the cells sum while they copy).
 */
void nf_seal2(struct nf_tmpl *t, uint8_t *hdr, size_t vrt_hdr, const uint8_t *body, size_t body_len,
	      uint64_t pre_sum, size_t pre_len);

#endif /* STREAM_ETH_NETFRAME_H_ */
