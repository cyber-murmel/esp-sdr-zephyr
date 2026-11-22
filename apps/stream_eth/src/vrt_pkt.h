/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VITA 49 signal data packets for the Ethernet stream: the header and trailer words, the packet
 * count and the time of a packet from the sample index. No Zephyr in here, so it runs on the host.
 *
 * Items are 8 bit link-efficient complex signed: I then Q per sample, MSB first across 32-bit
 * words, so the bytes on the wire are I0 Q0 I1 Q1 ..., the order of the diag16 ring's bytes.
 */

#ifndef STREAM_ETH_VRT_PKT_H_
#define STREAM_ETH_VRT_PKT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Header, stream ID, integer seconds, fractional seconds (two words). */
#define VRT_DATA_HDR_BYTES 20U
#define VRT_TRAILER_BYTES  4U

/** Time of the next sample, from the sample index and the exact sample rate. */
struct vrt_clock {
	uint32_t rate;
	/** Picoseconds a sample lasts when that is a whole number (rates dividing 10^12), else 0. */
	uint32_t ps_unit;
	/** Index of the next sample. */
	uint64_t index;
	/** index / rate and index % rate, kept up to date without dividing. */
	uint32_t sec;
	uint32_t sub;
};

/** Start the clock at a sample index (one 64 bit division). */
void vrt_clock_seek(struct vrt_clock *c, uint32_t rate, uint64_t index);

/** Move on n samples, n below the rate. */
void vrt_clock_advance(struct vrt_clock *c, uint32_t n);

/** Picoseconds past the last whole second, exact: floor(sub * 10^12 / rate). */
uint64_t vrt_clock_ps(const struct vrt_clock *c);

/**
 * Bytes of sample data in a packet for a link MTU: whole 32-bit words, so whole pairs of samples,
 * all that fits next to the IP and UDP headers, the VRT header and the trailer.
 */
size_t vrt_payload_bytes(unsigned int mtu, bool ipv6);

/** Packet size in 32-bit words for a payload of this many bytes, trailer included. */
static inline uint16_t vrt_packet_words(size_t payload_bytes)
{
	return (uint16_t)((VRT_DATA_HDR_BYTES + payload_bytes + VRT_TRAILER_BYTES) / 4U);
}

/** Write the 20 header bytes of a signal data packet with stream ID and trailer. */
void vrt_data_header(uint8_t *out, uint32_t stream_id, uint8_t count, uint16_t words,
		     uint32_t sec, uint64_t ps);

/** The trailer word (big endian bytes with vrt_put_trailer) for these indicators. */
uint32_t vrt_trailer_word(bool agc, bool over_range, bool sample_loss);

void vrt_put_trailer(uint8_t *out, uint32_t word);

/**
 * Does any byte of the payload sit at full scale (0x7f or 0x80)? Looks at one 32-bit word in
 * every `stride` (1 for all of them).
 */
bool vrt_clipped(const uint8_t *payload, size_t bytes, unsigned int stride);

/**
 * The settings epoch: 8 bits that go up by one at every change of a setting's value (one change at
 * a time, A to B to A is two) and stay where they are when a command sets what is already set. A
 * client reads it from the context packet of a run (CIF0 bit 16, user bits 7..0) and from the
 * query-state acknowledge, to know which settings the data belongs to. Each returns whether the
 * value changed.
 */
static inline bool vrt_set_u64(uint64_t *field, uint64_t v, uint8_t *epoch)
{
	if (*field == v) {
		return false;
	}
	*field = v;
	(*epoch)++;
	return true;
}

static inline bool vrt_set_u32(uint32_t *field, uint32_t v, uint8_t *epoch)
{
	if (*field == v) {
		return false;
	}
	*field = v;
	(*epoch)++;
	return true;
}

static inline bool vrt_set_int(int *field, int v, uint8_t *epoch)
{
	if (*field == v) {
		return false;
	}
	*field = v;
	(*epoch)++;
	return true;
}

#endif /* STREAM_ETH_VRT_PKT_H_ */
