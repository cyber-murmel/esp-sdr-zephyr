/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, wire format (doc/dev/txs.md): VITA 49.2 signal data packets, type 1 (with
 * stream ID), no class ID, no trailer, TSI none, TSF 1 = sample count, a 16 byte header:
 *
 *   word 0  0001 0 0 00 00 01 cccc ssssssssssssssss   type, C, T, indicators, TSI, TSF, count, size
 *   word 1  stream ID (the session's random 32 bit value)
 *   word 2,3  64 bit index of the packet's first sample (TSF), big endian
 *
 * then the payload, size * 4 - 16 bytes. Formats (a session parameter, not in the packet):
 * IQ8, a sample a 16 bit unit I then Q, signed bytes (the diag16 ring's byte order), and WORD32,
 * ready DAC words, 32 bit little endian. With the Ethernet header (14) and an IPv4 (20) or IPv6
 * (40) and UDP (8) header the payload starts at 2 mod 4 in a buffer whose frame starts aligned:
 * the parser accepts a payload at an even address, the kernels (txs_expand.h) take 0 and 2 mod 4.
 * No Zephyr calls, no reads outside the length given: runs on native_sim and under a fuzzer.
 */

#ifndef TXS_WIRE_H_
#define TXS_WIRE_H_

#include <stddef.h>
#include <stdint.h>

#define TXS_WIRE_HDR_BYTES 16U

enum txs_fmt {
	TXS_FMT_IQ8,
	TXS_FMT_WORD32,
};

struct txs_wire_cfg {
	uint32_t stream_id;
	uint16_t port;
	enum txs_fmt fmt;
	/* Most DAC words in a packet. */
	uint32_t max_words;
};

enum txs_wire_err {
	TXS_WIRE_OK = 0,
	/* Shorter than a header and a word. */
	TXS_WIRE_SHORT,
	/* Not signal data with a stream ID, or class ID, trailer or other flags set. */
	TXS_WIRE_TYPE,
	/* TSI or TSF other than none and sample count. */
	TXS_WIRE_TIME,
	/* The size field is not the length. */
	TXS_WIRE_SIZE,
	TXS_WIRE_STREAM,
	TXS_WIRE_PORT,
	/* Payload at an odd address. */
	TXS_WIRE_ALIGN,
	/* More words than the session allows. */
	TXS_WIRE_BIG,
	/* Index plus words does not fit 64 bits. */
	TXS_WIRE_INDEX,
};

struct txs_wire_pkt {
	uint64_t index;
	/* DAC words the payload makes. */
	uint32_t words;
	const uint8_t *payload;
	uint32_t bytes;
	/* The packet count of the header. */
	uint8_t count;
};

/**
 * Check a UDP payload (the VITA packet) that arrived on dport and take it apart.
 * @retval TXS_WIRE_OK, else the first check that failed; *out is then not valid.
 */
enum txs_wire_err txs_wire_parse(const struct txs_wire_cfg *cfg, uint16_t dport, const uint8_t *udp,
				 size_t len, struct txs_wire_pkt *out);

/** Write the 16 byte header of a packet of `words` DAC words; for the host tool and the tests. */
void txs_wire_header(uint8_t *out, const struct txs_wire_cfg *cfg, uint8_t count, uint64_t index,
		     uint32_t words);

#endif /* TXS_WIRE_H_ */
