/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The decoders that run over a closed burst (doc/pcap.md, "Decoders"): Bluetooth LE
 * (dec_ble.c), IEEE 802.15.4 (dec_wpan.c) and the generic analysis (dec_generic.c). Plain C, no
 * Zephyr: each is unit tested on native_sim. Each takes a burst (burst.h), a scratch area the
 * caller owns and hands its packets to a callback; none keeps a pointer into the burst or the
 * scratch after it returns.
 */

#ifndef DECODE_H_
#define DECODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "burst.h"
#include "pcap_proto.h"

/* ---- Bluetooth LE ---- */

#define DEC_BLE_ADV_AA       0x8E89BED6U
#define DEC_BLE_ADV_CRC_INIT 0x555555U
/* Access addresses looked for at once: the advertising one and those added. */
#define DEC_BLE_AA_MAX 8U
/* Longest on-air packet: access address, header, 255 bytes of payload, CRC. */
#define DEC_BLE_AIR_MAX (4U + 2U + 255U + 3U)

enum dec_ble_phy {
	DEC_BLE_PHY_1M = 0,
	DEC_BLE_PHY_2M = 1,
};

struct dec_ble_aa {
	uint32_t aa;
	/* 0: unknown, the CRC is not checked. */
	uint32_t crc_init;
};

struct dec_ble_cfg {
	struct dec_ble_aa aa[DEC_BLE_AA_MAX];
	uint8_t aa_count;
	/* Access address bit errors tolerated (0 to 2). */
	uint8_t max_errors;
	/* Try LE 2M too (only at 4 MS/s); skip LE 1M (a burst too wide for it, tried for LE 2M). */
	bool phy_2m, skip_1m;
	/* Packets with a failing CRC are emitted too. */
	bool keep_bad;
};

struct dec_ble_pkt {
	enum dec_ble_phy phy;
	/* The access address matched (as configured) and its bit errors. */
	uint32_t aa;
	uint8_t aa_errors;
	bool crc_checked, crc_ok;
	/* On-air bytes from the access address on: aa, header, payload, CRC. */
	uint16_t len;
	uint8_t air[DEC_BLE_AIR_MAX];
	/* The first preamble sample's index in the burst (may be negative: before it). */
	int32_t start;
	/* Carrier offset, Hz. */
	int32_t offset_hz;
	/* Mean power over the packet, output units squared. */
	float power;
	/* A CONNECT_IND: the access address and CRCInit it announces (0, 0 otherwise). */
	uint32_t conn_aa, conn_crc_init;
};

typedef void (*dec_ble_emit)(const struct dec_ble_pkt *pkt, void *user);

/* Scratch bytes dec_ble_run() needs for a burst of @p n samples. */
size_t dec_ble_scratch(uint32_t n);

/*
 * Every packet in the burst (rate 4 MS/s or 2 MS/s: else -ENOTSUP). Returns the count, or a
 * negative errno (-ENOMEM if @p scratch is short).
 */
int dec_ble_run(const struct burst *b, const struct dec_ble_cfg *cfg, void *scratch,
		size_t scratch_len, dec_ble_emit emit, void *user);

/* The RF channel of the LINKTYPE 256 header (0 at 2402 MHz, 39 at 2480) of a frequency. */
int dec_ble_rf_channel(uint64_t freq_hz);

/* ---- IEEE 802.15.4 ---- */

struct dec_wpan_cfg {
	bool keep_bad;
};

struct dec_wpan_pkt {
	/* The PSDU with its FCS. */
	uint8_t len;
	uint8_t psdu[127];
	bool fcs_ok;
	/* 0 to 255 from the chip errors, as the PHY reports it. */
	uint8_t lqi;
	/* The first preamble sample's index in the burst (may be negative). */
	int32_t start;
	int32_t offset_hz;
	float power;
	/* The 802.15.4 channel (11 to 26) of the frequency decoded on, 0 off the grid. */
	uint8_t channel;
	/* Decoded from the combination of two neighbouring channels. */
	bool combined;
};

typedef void (*dec_wpan_emit)(const struct dec_wpan_pkt *pkt, void *user);

size_t dec_wpan_scratch(uint32_t n);

/*
 * Every frame in the burst (rate 4 MS/s, or 2 MS/s read at 4 MS/s, the odd samples interpolated:
 * else -ENOTSUP). Returns the count or a negative errno.
 */
int dec_wpan_run(const struct burst *b, const struct dec_wpan_cfg *cfg, void *scratch,
		 size_t scratch_len, dec_wpan_emit emit, void *user);

/*
 * The band between two neighbouring channels (@p lo's index one below @p hi's, both at
 * 4 MS/s) over the frames both hold, combined as doc/pcap.md describes, through the PHY.
 * Packets carry the middle frequency. Returns the count or a negative errno.
 */
int dec_wpan_run_pair(const struct burst *lo, const struct burst *hi,
		      const struct dec_wpan_cfg *cfg, void *scratch, size_t scratch_len,
		      dec_wpan_emit emit, void *user);

/* The 802.15.4 channel of a frequency on the 2.4 GHz grid (2405 + 5 (c - 11) MHz), else 0. */
int dec_wpan_channel(uint64_t freq_hz);

/* ---- generic ---- */

struct dec_generic_cfg {
	/* Bits emitted at most (0, or above PCAP_BURST_BITS_MAX: PCAP_BURST_BITS_MAX). */
	uint16_t max_bits;
	/*
	 * Samples of the (trimmed) loud part analysed at most, from its start (0: all of them);
	 * duration_us, power and peak still cover the whole loud part.
	 */
	uint32_t max_samples;
};

struct dec_generic_out {
	/*
	 * The record's header, every field but version and hdr_len (left 0: the writer's) filled
	 * in: rx_gain, channel and channels copied from the burst, its BURST_F_* flags mapped to
	 * PCAP_BF_*, PCAP_BF_BITS added when bits were sliced. bandwidth_hz and offset_hz are 0
	 * when nothing stood clear of the noise.
	 */
	struct pcap_burst_hdr hdr;
	/*
	 * hdr.nbits bits, MSB first, the rest zero: one a symbol (FSK and MSK: 1 above the centre;
	 * OOK: 1 on; DBPSK: 1 for a phase step of pi), two a symbol of 4FSK (the sign, then 1 for
	 * an inner tone) and of DQPSK (steps of 0, pi / 2, pi, -pi / 2 as 00, 01, 11, 10).
	 */
	uint8_t bits[PCAP_BURST_BITS_MAX / 8U];
	/* The loud part's first sample's index in the burst (trimmed: the record's time). */
	uint32_t start;
};

/* Scratch bytes dec_generic_run() needs for a burst of @p n samples (some 4 n + 41 KiB). */
size_t dec_generic_scratch(uint32_t n);

/*
 * The analysis of a burst at any rate (dec_generic.c); @p cfg may be NULL. A burst without
 * samples (a wideband record's) gets its metadata only. 0, -EINVAL without a burst or @p out,
 * -ENOMEM when @p scratch is short.
 */
int dec_generic_run(const struct burst *b, const struct dec_generic_cfg *cfg, void *scratch,
		    size_t scratch_len, struct dec_generic_out *out);

#endif /* DECODE_H_ */
