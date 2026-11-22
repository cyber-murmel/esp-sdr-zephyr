/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, wire format: see txs_wire.h.
 */

#include <esp_sdr/esp_sdr_place.h>

#include "txs_wire.h"

static inline __attribute__((always_inline)) uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline __attribute__((always_inline)) void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* type 1 (data with stream ID), no class, no trailer, no indicators, TSI none, TSF sample count */
#define HDR_FIXED 0x10100000U

ESP_SDR_CODE(PACED_CHUNK, txs_wire_parse)
enum txs_wire_err txs_wire_parse(const struct txs_wire_cfg *cfg, uint16_t dport, const uint8_t *udp,
				 size_t len, struct txs_wire_pkt *out)
{
	uint32_t w0, bytes, words;
	uint64_t index;

	if (len < TXS_WIRE_HDR_BYTES + 4U) {
		return TXS_WIRE_SHORT;
	}
	if (dport != cfg->port) {
		return TXS_WIRE_PORT;
	}
	w0 = get32(udp);
	if ((w0 & 0xff000000U) != 0x10000000U) {
		return TXS_WIRE_TYPE;
	}
	if ((w0 & 0x00f00000U) != 0x00100000U) {
		return TXS_WIRE_TIME;
	}
	if ((len & 3U) != 0U || (w0 & 0xffffU) != len / 4U || len > 4U * 0xffffU) {
		return TXS_WIRE_SIZE;
	}
	if (get32(udp + 4) != cfg->stream_id) {
		return TXS_WIRE_STREAM;
	}
	if (((uintptr_t)(udp + TXS_WIRE_HDR_BYTES) & 1U) != 0U) {
		return TXS_WIRE_ALIGN;
	}
	bytes = (uint32_t)len - TXS_WIRE_HDR_BYTES;
	words = cfg->fmt == TXS_FMT_IQ8 ? bytes / 2U : bytes / 4U;
	if (words > cfg->max_words) {
		return TXS_WIRE_BIG;
	}
	index = ((uint64_t)get32(udp + 8) << 32) | get32(udp + 12);
	if (index + words < index) {
		return TXS_WIRE_INDEX;
	}
	out->index = index;
	out->words = words;
	out->payload = udp + TXS_WIRE_HDR_BYTES;
	out->bytes = bytes;
	out->count = (uint8_t)((w0 >> 16) & 0xfU);
	return TXS_WIRE_OK;
}

void txs_wire_header(uint8_t *out, const struct txs_wire_cfg *cfg, uint8_t count, uint64_t index,
		     uint32_t words)
{
	uint32_t bytes = cfg->fmt == TXS_FMT_IQ8 ? words * 2U : words * 4U;
	uint32_t size = (TXS_WIRE_HDR_BYTES + bytes) / 4U;

	put32(out, HDR_FIXED | ((uint32_t)(count & 0xfU) << 16) | size);
	put32(out + 4, cfg->stream_id);
	put32(out + 8, (uint32_t)(index >> 32));
	put32(out + 12, (uint32_t)index);
}
