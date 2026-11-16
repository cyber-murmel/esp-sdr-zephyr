/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The packet records of apps/pcap: the payload of a pcapng packet on each of the stream's
 * interfaces (pcap_proto.h) built from a decoder's packet (decode.h), and the metadata comment
 * that goes with it (opt_comment). Pure functions, buffers in and lengths out; plain C, no
 * Zephyr: unit tested on native_sim (tests/unit/pcapng).
 */

#ifndef RECORDS_H_
#define RECORDS_H_

#include <stddef.h>
#include <stdint.h>

#include "decode.h"
#include "pcap_proto.h"

/* ---- LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256) ---- */

/*
 * The header before the on-air bytes: u8 RF channel, i8 signal power dBm, i8 noise power dBm,
 * u8 access address offenses, u32 reference access address, u16 flags
 * (https://www.tcpdump.org/linktypes/LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR.html).
 */
#define REC_BLE_HDR 10U
#define REC_BLE_MAX (REC_BLE_HDR + DEC_BLE_AIR_MAX)

/* The flags. The power fields are dBm, which the uncalibrated radio cannot give: never valid. */
#define REC_BLE_F_DEWHITENED     0x0001U
#define REC_BLE_F_SIGNAL_VALID   0x0002U
#define REC_BLE_F_NOISE_VALID    0x0004U
#define REC_BLE_F_REF_AA_VALID   0x0010U
#define REC_BLE_F_OFFENSES_VALID 0x0020U
#define REC_BLE_F_PDU_TYPE_MASK  0x0380U
#define REC_BLE_F_CRC_CHECKED    0x0400U
#define REC_BLE_F_CRC_VALID      0x0800U
#define REC_BLE_F_PHY_MASK       0xC000U
#define REC_BLE_F_PHY_SHIFT      14U
/* PHY values in REC_BLE_F_PHY_MASK. */
#define REC_BLE_PHY_1M    0U
#define REC_BLE_PHY_2M    1U
#define REC_BLE_PHY_CODED 2U

/* RF channels of the header: 0 at 2402 MHz to 39 at 2480 MHz. */
#define REC_BLE_RF_CHANNEL_MAX 39

/*
 * The record of a packet: the header with @p rf_channel (0 to REC_BLE_RF_CHANNEL_MAX), the
 * power fields 0 and flagged invalid, the matched access address and its bit errors (both
 * flagged valid), dewhitened always, CRC checked and CRC valid as the decoder says, the PHY of
 * the packet, the PDU type unspecified; then p->len bytes of p->air (access address, PDU, CRC).
 * Returns the length, -ENOSPC when @p cap is short (nothing written), -EINVAL for a channel off
 * the grid or a packet longer than its buffer.
 */
int rec_ble(uint8_t *buf, size_t cap, const struct dec_ble_pkt *p, int rf_channel);

/* ---- LINKTYPE_IEEE802_15_4_TAP (283) ---- */

/*
 * The TAP header (https://gitlab.com/exegin/ieee802-15-4-tap): u8 version, u8 reserved, u16
 * length of the header with its TLVs, then TLVs {u16 type, u16 length of the value, value,
 * zero padding to 4}; the PSDU follows the header.
 */
#define REC_TAP_VERSION 0U
#define REC_TAP_TLV_FCS_TYPE           0U
#define REC_TAP_TLV_RSS                1U
#define REC_TAP_TLV_BIT_RATE           2U
#define REC_TAP_TLV_CHANNEL_ASSIGNMENT 3U
#define REC_TAP_TLV_LQI                10U
#define REC_TAP_TLV_CHANNEL_FREQUENCY  11U
/* FCS type values: none, 16-bit CRC, 32-bit CRC. */
#define REC_TAP_FCS_NONE  0U
#define REC_TAP_FCS_CRC16 1U
#define REC_TAP_FCS_CRC32 2U
/* O-QPSK 2.4 GHz: 250 kbit/s. */
#define REC_TAP_BIT_RATE_OQPSK 250000U
/* The header written here: 4 bytes and up to five 8-byte TLVs. */
#define REC_TAP_HDR_MAX 44U
#define REC_TAP_MAX     (REC_TAP_HDR_MAX + 127U)

/*
 * The record of a frame: the TAP header with FCS_TYPE 1 (the 16-bit CRC is part of the PSDU:
 * Wireshark checks it), BIT_RATE 250000, CHANNEL_ASSIGNMENT (p->channel, page 0) when
 * p->channel is not 0, LQI p->lqi and CHANNEL_FREQUENCY (@p freq_hz as a float in kHz), then
 * p->len bytes of p->psdu with the FCS. No RSS: the radio is not calibrated in dBm. Returns the
 * length, -ENOSPC when @p cap is short, -EINVAL for a PSDU longer than its buffer.
 */
int rec_wpan(uint8_t *buf, size_t cap, const struct dec_wpan_pkt *p, uint64_t freq_hz);

/* ---- LINKTYPE_USER0 (147): the burst record ---- */

#define REC_BURST_MAX (sizeof(struct pcap_burst_hdr) + PCAP_BURST_BITS_MAX / 8U)

/*
 * The record of a burst: @p h with version PCAP_BURST_VERSION and hdr_len sizeof(*h) set, then
 * (h->nbits + 7) / 8 bytes of @p bits (MSB first, the last byte zero padded by the caller).
 * Returns the length, -ENOSPC when @p cap is short, -EINVAL for more than PCAP_BURST_BITS_MAX
 * bits or NULL bits with a count.
 */
int rec_burst(uint8_t *buf, size_t cap, const struct pcap_burst_hdr *h, const uint8_t *bits);

/* ---- the metadata comment ---- */

/* rec_meta.gain for automatic gain (ESDR_SEG_GAIN_AUTO of esdr_proto.h). */
#define REC_GAIN_AUTO 255U
/* A buffer that holds every comment whose phy name is at most 20 bytes, the NUL included. */
#define REC_COMMENT_MAX 192U

struct rec_meta {
	/* The channel's centre frequency, Hz. */
	uint64_t freq_hz;
	/* The signal's carrier offset against it, Hz. */
	int32_t offset_hz;
	/* Mean power of the packet and the channel's floor, dBFS times 10. */
	int16_t power_dbfs_x10, floor_dbfs_x10;
	/* Receive gain index, REC_GAIN_AUTO for automatic. */
	uint8_t gain;
	/* The channelizer's channel index and count. */
	uint16_t channel, channels;
	/* The channel's sample rate, Hz; the packet's duration, us. */
	uint32_t rate_hz, duration_us;
	/* The PHY's name ("LE 1M", "LE 2M", "O-QPSK"), NULL for none. */
	const char *phy;
};

/*
 * The packet comment (pcapng opt_comment, Wireshark's packet comment, filterable as
 * frame.comment): one line, no newline, NUL terminated in @p s but written to the stream
 * without it, of this exact shape:
 *
 *   esdr: 2426.000 MHz, ch 12/40 at 4.000 MS/s, LE 1M, offset +12.3 kHz, power -42.1 dBFS,
 *   floor -78.0 dBFS, snr 35.9 dB, gain 30, 376 us
 *
 * in this order: the channel's centre frequency (MHz, three decimals), its index and the bank's
 * channel count, the channel's sample rate (MS/s, three decimals), the PHY name (the item is
 * left out when phy is NULL), the carrier offset (kHz, one decimal, always signed: "+0.0" for
 * zero), the packet's power and the channel's floor (dBFS, one decimal), their difference as
 * the snr (dB), the receive gain index or "auto", the duration (us). Values are rounded half
 * away from zero to the last digit. Integer arithmetic only. Returns the length without the
 * NUL, or -ENOSPC when @p cap (the NUL counted) is short: @p s then holds the truncated text.
 */
int rec_comment(char *s, size_t cap, const struct rec_meta *m);

#endif /* RECORDS_H_ */
