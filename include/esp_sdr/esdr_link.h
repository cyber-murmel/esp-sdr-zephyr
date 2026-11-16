/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief The framed serial link between a host and apps/osmosdr_serial.
 *
 * What esdr_proto.h's serial transport carries (requests, replies, receive blocks), in frames
 * over a byte stream that may lose bytes and has no flow control wires: a UART, through the MCH2022
 * badge's RP2040. A frame says what it is, how many came before it and whether it is whole, and a
 * lost byte costs one frame, not the stream's alignment.
 *
 * Before encoding: type, flags (0), sequence number (little endian; blocks and all other frames count
 * apart, as they may take different ways), the payload, CRC-32 (zlib's, little endian) over all
 * of these. Then COBS, so that no zero byte is left, and a
 * zero byte ends the frame. Empty frames (zeros in a row) mean nothing, except that
 * ESDR_LINK_FALLBACK_ZEROS of them, as a host of the older transport sends on opening, go back to
 * that transport.
 *
 * Portable C and C++, no dependencies: the firmware, apps/osmosdr/soapy and the unit tests use it.
 * apps/osmosdr_serial/tools/esdr_serial.py has its twin. Tools read the numbers from this file:
 * keep every constant on its own line.
 */

#ifndef ESP_SDR_ESDR_LINK_H_
#define ESP_SDR_ESDR_LINK_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESDR_LINK_VERSION 1U
/* Type, flags, sequence number. */
#define ESDR_LINK_HDR_BYTES 4U
#define ESDR_LINK_CRC_BYTES 4U
/* A device goes back to the older transport after this many empty frames in a row. */
#define ESDR_LINK_FALLBACK_ZEROS 64U
/* The bytes on the wire of a payload of this length at most: COBS' overhead and the zero. */
#define ESDR_LINK_ENCODED_MAX(len)                                                                 \
	((len) + ESDR_LINK_HDR_BYTES + ESDR_LINK_CRC_BYTES +                                       \
	 ((len) + ESDR_LINK_HDR_BYTES + ESDR_LINK_CRC_BYTES) / 254U + 2U)

enum esdr_link_type {
	/*
	 * Host to device: struct esdr_link_hello, from here on it is frames. The device answers
	 * with one, whatever it was doing (a stream stops, as for any request).
	 */
	ESDR_LINK_HELLO = 1,
	/* Host to device: struct esdr_ser_hdr and its data, as the older transport's frames. */
	ESDR_LINK_REQUEST = 2,
	/* Device to host: its reply, the same. */
	ESDR_LINK_REPLY = 3,
	/* Device to host: struct esdr_block_hdr and its samples, no padding. */
	ESDR_LINK_BLOCK = 4,
	/* Device to host: text, from a console. */
	ESDR_LINK_TEXT = 5,
	/* Host to device: uint32_t, wire bytes of blocks the device may send on top of what it may yet. */
	ESDR_LINK_CREDIT = 6,
	/* Host to device: back to the older transport. */
	ESDR_LINK_BYE = 7,
};

/*
 * struct esdr_link_hello.flags. Host to device: the RP2040 reads the FPGA's fast line
 * (COPROC_REQ_FPGA_FASTLINK), blocks may go that way. Device to host: they will.
 */
#define ESDR_LINK_HELLO_F_FAST 0x01U

/*
 * The MCH2022 badge's FPGA with the link bitstream (gateware/esdr_fpga/link.py): an SPI frame with
 * this opcode holds link bytes, and bit 1 of its register CAPS says it is there.
 */
#define ESDR_LINK_FPGA_OP   0x20U
#define ESDR_LINK_FPGA_CAPS 0x0002U
/* The FPGA's FIFO, bytes: the host's credit stays below it. */
#define ESDR_LINK_FPGA_FIFO 32768U

/* ESDR_LINK_HELLO's payload. */
struct esdr_link_hello {
	uint8_t version;
	/* ESDR_LINK_HELLO_F_* */
	uint8_t flags;
	/* Device to host: the longest frame it takes, encoded. */
	uint16_t max_frame;
	/*
	 * Host to device: the wire bytes of receive blocks it may send until more credit comes; 0
	 * no limit. Device to host: what it took.
	 */
	uint32_t credit;
	/* Device to host: the highest baud rate of ESDR_REQ_SET_LINK_BAUD. */
	uint32_t baud_max;
} __attribute__((packed));

struct esdr_link_frame {
	uint8_t type;
	uint8_t flags;
	uint16_t seq;
	const uint8_t *payload;
	size_t len;
};

/* CRC-32 (reflected 0xEDB88320, as zlib's) a nibble at a time: 16 words of table. */
static inline uint32_t esdr_link_crc_add(uint32_t crc, const uint8_t *p, size_t n)
{
	static const uint32_t table[16] = {
		0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
		0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
		0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
		0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
	};

	while (n-- > 0U) {
		crc ^= *p++;
		crc = (crc >> 4) ^ table[crc & 0xFU];
		crc = (crc >> 4) ^ table[crc & 0xFU];
	}
	return crc;
}

/* The encoder's state: where the current COBS block's code byte goes. */
struct esdr_link_cobs {
	uint8_t *out;
	size_t at, code_at, max;
	uint8_t code;
	int overflow;
};

static inline void esdr_link_cobs_start(struct esdr_link_cobs *c, uint8_t *out, size_t max)
{
	c->out = out;
	c->max = max;
	c->at = 1U;
	c->code_at = 0U;
	c->code = 1U;
	c->overflow = max < 2U;
}

static inline void esdr_link_cobs_put(struct esdr_link_cobs *c, const uint8_t *p, size_t n)
{
	while (n-- > 0U) {
		const uint8_t b = *p++;

		if (c->at >= c->max) {
			c->overflow = 1;
			return;
		}
		if (b != 0U) {
			c->out[c->at++] = b;
			c->code++;
		}
		if (b == 0U || c->code == 0xFFU) {
			c->out[c->code_at] = c->code;
			c->code_at = c->at++;
			c->code = 1U;
		}
	}
}

/*
 * One frame on the wire into @p out: the payload is @p a then @p b (either may be empty). The bytes
 * written, the ending zero included; 0 if @p max is too small.
 */
static inline size_t esdr_link_encode(uint8_t *out, size_t max, uint8_t type, uint16_t seq,
				      const void *a, size_t a_len, const void *b, size_t b_len)
{
	const uint8_t hdr[ESDR_LINK_HDR_BYTES] = {type, 0U, (uint8_t)seq, (uint8_t)(seq >> 8)};
	struct esdr_link_cobs c;
	uint32_t crc = 0xFFFFFFFFU;
	uint8_t tail[ESDR_LINK_CRC_BYTES];

	esdr_link_cobs_start(&c, out, max);
	crc = esdr_link_crc_add(crc, hdr, sizeof(hdr));
	esdr_link_cobs_put(&c, hdr, sizeof(hdr));
	if (a_len > 0U) {
		crc = esdr_link_crc_add(crc, (const uint8_t *)a, a_len);
		esdr_link_cobs_put(&c, (const uint8_t *)a, a_len);
	}
	if (b_len > 0U) {
		crc = esdr_link_crc_add(crc, (const uint8_t *)b, b_len);
		esdr_link_cobs_put(&c, (const uint8_t *)b, b_len);
	}
	crc = ~crc;
	tail[0] = (uint8_t)crc;
	tail[1] = (uint8_t)(crc >> 8);
	tail[2] = (uint8_t)(crc >> 16);
	tail[3] = (uint8_t)(crc >> 24);
	esdr_link_cobs_put(&c, tail, sizeof(tail));
	if (c.overflow || c.at >= c.max) {
		return 0U;
	}
	c.out[c.code_at] = c.code;
	c.out[c.at++] = 0U;
	return c.at;
}

/*
 * The frame in @p buf, @p n bytes without the ending zero, decoded in place. 0 and @p f filled (its
 * payload inside @p buf), or -1: not COBS, too short, or the CRC does not match.
 */
static inline int esdr_link_decode(uint8_t *buf, size_t n, struct esdr_link_frame *f)
{
	size_t in = 0U, to = 0U;
	uint32_t crc;

	while (in < n) {
		const uint8_t code = buf[in++];

		if (code == 0U || in + code - 1U > n) {
			return -1;
		}
		for (uint8_t k = 1U; k < code; k++) {
			buf[to++] = buf[in++];
		}
		if (code != 0xFFU && in < n) {
			buf[to++] = 0U;
		}
	}
	if (to < ESDR_LINK_HDR_BYTES + ESDR_LINK_CRC_BYTES) {
		return -1;
	}
	crc = ~esdr_link_crc_add(0xFFFFFFFFU, buf, to - ESDR_LINK_CRC_BYTES);
	if ((uint8_t)crc != buf[to - 4U] || (uint8_t)(crc >> 8) != buf[to - 3U] ||
	    (uint8_t)(crc >> 16) != buf[to - 2U] || (uint8_t)(crc >> 24) != buf[to - 1U]) {
		return -1;
	}
	f->type = buf[0];
	f->flags = buf[1];
	f->seq = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
	f->payload = buf + ESDR_LINK_HDR_BYTES;
	f->len = to - ESDR_LINK_HDR_BYTES - ESDR_LINK_CRC_BYTES;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESDR_LINK_H_ */
