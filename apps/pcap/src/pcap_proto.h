/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * apps/pcap's wire formats beyond esdr_proto.h (doc/pcap.md): the vendor requests the host tool
 * (tools/esdr_pcap.py) sends this chip itself, and the record of a burst no decoder claimed,
 * the payload of the pcapng packets on the LINKTYPE_USER0 interface (tools/esdr_burst.lua
 * dissects it). Little endian throughout, as esdr_proto.h. The host tool and its tests read the
 * numbers from this file: keep every constant on its own line, as "#define NAME value" or an
 * enumerator "NAME = value".
 */

#ifndef PCAP_PROTO_H_
#define PCAP_PROTO_H_

#include <stdint.h>

/* pcapng link types (https://www.tcpdump.org/linktypes.html). */
#define PCAP_LINKTYPE_BLE_LL_WITH_PHDR 256U
#define PCAP_LINKTYPE_IEEE802_15_4_TAP 283U
#define PCAP_LINKTYPE_USER0            147U

/* The stream's interfaces (pcapng interface ids, in the order their IDBs are written). */
#define PCAP_IF_BLE   0U
#define PCAP_IF_WPAN  1U
#define PCAP_IF_BURST 2U
#define PCAP_IF_COUNT 3U

/*
 * Vendor requests answered by this chip (esdr_proto.h's, below 0x40, go to the C6; among them
 * ESDR_REQ_SET_CHANNELIZER, which this chip answers with the bank it runs, as the bridge).
 */
enum pcap_req {
	/*
	 * OUT, no data: start a capture. The bank, the detector and the assembler start anew,
	 * the section header and interface blocks go out, the C6 goes to ESDR_MODE_GAPLESS
	 * with the frequency, gain and gapless settings sent before (esdr_proto.h requests).
	 * Fails (stall) without a channelizer set.
	 */
	PCAP_REQ_START = 0x40,
	/* OUT, no data: the C6 to ESDR_MODE_OFF, a last statistics block per interface. */
	PCAP_REQ_STOP = 0x41,
	/*
	 * OUT 8 bytes: the host's time, nanoseconds since the Unix epoch (uint64_t). Packet
	 * timestamps count from it (doc/pcap.md, "Output").
	 */
	PCAP_REQ_SET_TIME = 0x42,
	/* OUT struct pcap_decoders: what runs over the bursts. Applies to the next burst. */
	PCAP_REQ_SET_DECODERS = 0x43,
	/* IN struct pcap_stats. */
	PCAP_REQ_GET_STATS = 0x44,
	/*
	 * OUT struct pcap_aa: a Bluetooth LE access address to look for on every channel, with
	 * its CRCInit (0 for unknown: the CRC is not checked). wValue 0 adds it, 1 removes it,
	 * 2 (no data) removes all but the advertising one.
	 */
	PCAP_REQ_ADD_AA = 0x45,
	/*
	 * OUT struct pcap_mode_cfg: what the next capture runs (default: the survey alone).
	 * Applies at the next PCAP_REQ_START.
	 */
	PCAP_REQ_SET_MODE = 0x46,
};

/*
 * Capture modes (doc/pcap.md, "Modes"). The survey runs the C6's gapless 80 MHz (bursts the
 * channelizer finds, decoded where they fit: the C6 keeps some 50 us of a burst whole, three
 * times that at 40 MS/s, PCAP_MODE_F_HALF). The channel mode streams one channel without a gap
 * at the C6's paced rate (2 MS/s): complete packets of that channel (Bluetooth LE 1M).
 */
enum pcap_mode {
	PCAP_MODE_SURVEY = 0,
	PCAP_MODE_CHANNEL = 1,
};

/* struct pcap_mode_cfg.flags */
/* The C6 sorts the windows it keeps by width: wide ones (Wi-Fi) come as records only. */
#define PCAP_MODE_F_C6_WIDE 0x0001U
/*
 * The survey at 40 MS/s (ESDR_GAPLESS_F_HALF): 40 MHz around the centre instead of 80, bursts
 * kept whole some three times as long. The channels keep their spacing (the bank runs M / 2 of
 * them, so 16 channels cannot: PCAP_REQ_START fails); the bin at +-20 MHz, where both edges
 * fold, is left out; the C6 does not sort by width (PCAP_MODE_F_C6_WIDE is ignored).
 */
#define PCAP_MODE_F_HALF    0x0002U

struct pcap_mode_cfg {
	/* enum pcap_mode: the mode to start in. */
	uint16_t mode;
	/* PCAP_MODE_F_* */
	uint16_t flags;
	/* The channel mode's rate (0: 2 MS/s) and frequency (0: the one ESDR_REQ_SET_FREQ set). */
	uint32_t channel_rate_hz;
	uint64_t channel_freq_hz;
	/*
	 * Both non-zero: the capture alternates, survey_ms in the survey, channel_ms in the
	 * channel mode (each switch costs some 15 ms of the C6's time).
	 */
	uint32_t survey_ms, channel_ms;
} __attribute__((packed));

/* Decoders (struct pcap_decoders.flags). */
/* Bluetooth LE 1M and 2M on every channel of a 4 MS/s bank (LE 1M alone at 2 MS/s). */
#define PCAP_DEC_BLE      0x0001U
/* IEEE 802.15.4 O-QPSK 2.4 GHz on every channel of a 4 MS/s bank, bin-centred and combined. */
#define PCAP_DEC_WPAN     0x0002U
/* A burst record for bursts no decoder claimed. */
#define PCAP_DEC_GENERIC  0x0004U
/* A burst record (no samples, no bits) for signals wider than several channels (Wi-Fi). */
#define PCAP_DEC_WIDEBAND 0x0008U
/* Packets whose CRC or FCS failed go out too (flagged). */
#define PCAP_DEC_BAD_CRC  0x0010U
/* Access addresses of CONNECT_IND PDUs seen are followed on the data channels. */
#define PCAP_DEC_BLE_DATA 0x0020U
/* A comment with the signal metadata on every BLE and 802.15.4 packet. */
#define PCAP_DEC_COMMENTS 0x0040U

#define PCAP_DEC_DEFAULT                                                                           \
	(PCAP_DEC_BLE | PCAP_DEC_WPAN | PCAP_DEC_GENERIC | PCAP_DEC_WIDEBAND | PCAP_DEC_BLE_DATA | \
	 PCAP_DEC_COMMENTS)

struct pcap_decoders {
	uint32_t flags;
	/* A generic record needs a burst at least this long, microseconds (0: 20). */
	uint16_t min_burst_us;
	/* Bits a generic record carries at most (0: 2048, the cap PCAP_BURST_BITS_MAX). */
	uint16_t max_bits;
} __attribute__((packed));

struct pcap_aa {
	uint32_t aa;
	uint32_t crc_init;
} __attribute__((packed));

struct pcap_stats {
	/* Segment blocks taken from the C6, dropped (the channelizer fell behind), other blocks. */
	uint32_t seg_blocks, seg_dropped, other_blocks;
	/* Input pairs channelized, CPU cycles that took (bank, detector and assembler). */
	uint64_t pairs, chan_cycles;
	/* Bursts opened, closed and queued, lost (no buffer, too many open, queue full), wideband. */
	uint32_t bursts, bursts_lost, bursts_wide;
	/* Bursts decoded, CPU cycles the decoders took. */
	uint32_t decoded;
	uint64_t dec_cycles;
	/* Packets per interface (PCAP_IF_*): written, dropped (ring full, no host). */
	uint32_t packets[PCAP_IF_COUNT], dropped[PCAP_IF_COUNT];
	/* Bytes of pcapng written to USB; output ring's highest fill. */
	uint64_t usb_bytes;
	uint32_t ring_peak;
	/* The CPU's clock, MHz. */
	uint32_t cpu_mhz;
	/* 1 while a capture runs; the mode running now (enum pcap_mode). */
	uint32_t running, mode;
	/* Channel mode: samples taken, blocks with a gap before them. */
	uint64_t channel_samples;
	uint32_t channel_gaps;
	/* Width records the C6 sent (PCAP_MODE_F_C6_WIDE). */
	uint32_t c6_wide;
	/*
	 * The generic analysis: bursts analysed, recorded without it (it was busy), recorded as
	 * filling the channel (nothing to analyse); CPU cycles it took.
	 */
	uint32_t generic, generic_skipped, generic_wide;
	uint64_t generic_cycles;
} __attribute__((packed));

/* ---- the burst record (LINKTYPE_USER0 payload) ---- */

#define PCAP_BURST_VERSION 1U
/* Bits a record carries at most (the bytes after the header: PCAP_BURST_BITS_MAX / 8). */
#define PCAP_BURST_BITS_MAX 2048U

/* struct pcap_burst_hdr.flags */
/* The burst was cut at the buffer's cap while the channel stayed open. */
#define PCAP_BF_TRUNCATED  0x0001U
/* Several adjacent channels opened together: a signal wider than a channel; no bits. */
#define PCAP_BF_WIDEBAND   0x0002U
/* Frames of the filterbank's transient (a gap in the input) are among the samples. */
#define PCAP_BF_TRANSIENT  0x0004U
/* A neighbouring channel was open at the same time: this may be a wider signal's skirt. */
#define PCAP_BF_NEIGHBOUR  0x0008U
/* The C6 dropped windows before this burst (the link was full): its start may be missing. */
#define PCAP_BF_DROPPED    0x0010U
/* Samples reached the receiver's full scale. */
#define PCAP_BF_CLIPPED    0x0020U
/* The bits were sliced at the estimated symbol rate and are a best effort. */
#define PCAP_BF_BITS       0x0040U
/* The burst was still open when the capture stopped. */
#define PCAP_BF_CUT        0x0080U
/* The burst ended at a hole in the C6's stream (it kept nothing for a while, its windows were
 * quiet or did not fit the link): the signal may go on in the next record. */
#define PCAP_BF_HOLE       0x0100U
/* Not analysed, the generic analysis was busy: the record holds the detector's metadata only
 * (power is the loudest block's). */
#define PCAP_BF_SKIPPED    0x0200U

/* struct pcap_burst_hdr.modulation */
enum pcap_mod {
	PCAP_MOD_UNKNOWN = 0,
	/* An unmodulated carrier. */
	PCAP_MOD_CW = 1,
	PCAP_MOD_OOK = 2,
	/* Two tones (GFSK and GMSK included): deviation_hz is half their spacing. */
	PCAP_MOD_2FSK = 3,
	PCAP_MOD_4FSK = 4,
	/* Phase steps of +-pi/2 a symbol by a rectangular frequency pulse: MSK, O-QPSK with
	 * half-sine pulses (the Gaussian pulse of GMSK and GFSK counts as 2FSK). */
	PCAP_MOD_MSK = 5,
	PCAP_MOD_BPSK = 6,
	PCAP_MOD_QPSK = 7,
	/* Noise-like: spread spectrum or OFDM, or a wideband signal's part in this channel. */
	PCAP_MOD_WIDEBAND = 8,
};

/*
 * The record: this header, then nbits bits, most significant bit of each byte first, the last
 * byte zero padded. Fields may be appended in later versions: a reader goes by hdr_len.
 */
struct pcap_burst_hdr {
	uint8_t version;
	/* Bytes of this header. */
	uint8_t hdr_len;
	/* PCAP_BF_* */
	uint16_t flags;
	/* The channel's centre frequency, Hz. */
	uint64_t freq_hz;
	/* The signal's centre against freq_hz (its carrier offset), Hz. */
	int32_t offset_hz;
	/* Occupied bandwidth (90 % of the power above the noise, at sample_rate_hz), Hz; 0
	 * unknown. */
	uint32_t bandwidth_hz;
	/* From the first to the last loud sample, microseconds. */
	uint32_t duration_us;
	/* Symbols a second; 0 unknown. */
	uint32_t symbol_rate;
	/* 2FSK, 4FSK, MSK: half the spacing of the outer tones (the symbols' mean frequencies),
	 * Hz; 0 unknown or other. */
	uint32_t deviation_hz;
	/* enum pcap_mod */
	uint8_t modulation;
	/* Receive gain index the radio ran at (ESDR_SEG_GAIN_AUTO: automatic). */
	uint8_t rx_gain;
	/* The channelizer's channel index and its channel count M. */
	uint8_t channel, channels;
	/* Mean power over the loud part, the channel's noise floor and the peak sample power,
	 * dB relative to the receiver's full scale, times 10 (negative). */
	int16_t power_dbfs_x10, floor_dbfs_x10, peak_dbfs_x10;
	uint16_t reserved;
	/* The rate of the stream the analysis ended at (the channel's, or lower after refining), Hz. */
	uint32_t sample_rate_hz;
	/* Bits that follow. */
	uint32_t nbits;
} __attribute__((packed));

#endif /* PCAP_PROTO_H_ */
