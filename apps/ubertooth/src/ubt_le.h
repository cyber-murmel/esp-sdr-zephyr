/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * BLE (LE 1M) packet finder on the demodulated symbol stream, and the promiscuous
 * mode helpers (AA, CRCInit, hop interval and increment recovery). Replaces the
 * CC2400 sync word detector and the DMA byte stream of the Ubertooth firmware
 * (bluetooth_rxtx.c bt_le_sync(), le_phy.c, bt_promisc_le()); pure logic, no
 * Zephyr dependency, all state in caller allocated structs.
 *
 * LE_PACKET layout (usb_pkt_rx, verified against libbtbb lib/src/bluetooth_le_packet.c,
 * lell_allocate_and_decode() and lell_print(), which the host's cb_btle() calls with
 * rx->data, rx->channel + 2402 and rx->clk100ns):
 *
 *   pkt_type   LE_PACKET (1)
 *   status     0
 *   channel    MHz - 2402
 *   clkn_high  0
 *   clk100ns   start of the packet (first preamble bit), see UBT_LE_START_BACK
 *   rssi_max/min/avg  raw RSSI (dBm + 54) from the symbol powers, rssi_count 0
 *   data[0..3] access address, little endian (the configured one, as upstream; the
 *              bits actually received are in struct ubt_le_info.aa_rx)
 *   data[4]    PDU header byte 0 (advertising: type, ChSel bit 5, TxAdd bit 6, RxAdd bit 7;
 *              data: LLID, NESN, SN, MD)
 *   data[5]    PDU length (libbtbb masks it with 0x3f on advertising and 0x1f on data
 *              channels, the bytes themselves are the full 8 bit field)
 *   data[6..]  payload, then the three CRC bytes, then zeros; cut at data[49]
 *
 * All bytes after the access address are dewhitened and in over-the-air byte order, bit 0
 * of each byte is the first bit on air (so a CONNECT_REQ has InitA at data[6..11], AdvA
 * data[12..17], AA data[18..21], CRCInit data[22..24], WinSize data[25], WinOffset
 * data[26..27], Interval data[28..29], Latency data[30..31], Timeout data[32..33], ChM
 * data[34..38], Hop/SCA data[39], the offsets lell_print() uses). The CRC
 * bytes are the CRC register in transmission order (btle_crcgen_lut() result, byte 0 first).
 * libbtbb never checks the CRC and prints bytes 6 + length .. 8 + length as the CRC, so a
 * packet with more than 41 payload bytes loses its CRC in the USB packet (the host shows
 * those bytes as zero); the finder still checks the CRC over the whole packet.
 *
 * The logic is ported from the Project Ubertooth firmware (bluetooth_rxtx.c, Copyright
 * 2010-2013 Michael Ossmann and 2011-2013 Dominic Spill; le_phy.c, Copyright 2017 Mike Ryan),
 * released under the GPL version 2 or later.
 */

#ifndef UBT_LE_H_
#define UBT_LE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfsk_sym.h"
#include "ubt_proto.h"

#define UBT_LE_ADV_AA       0x8E89BED6U
#define UBT_LE_ADV_CRC_INIT 0x555555U

/* Bytes of PDU and CRC the USB packet carries after the 4 AA bytes. */
#define UBT_LE_USB_BODY (DMA_SIZE - 4U)

/* The correlator tolerates up to this many AA bit errors. */
#define UBT_LE_MAX_TOLERANCE 2U

/*
 * Packet start convention: clk100ns is the time of the first preamble bit (the spec's start
 * of a packet, what T_IFS and the connection anchor are measured from). That bit begins 8
 * preamble symbols, 31 AA symbols and half a symbol before the decision instant of the last
 * AA symbol (where the correlator fires): 158 samples at 4 per symbol. The upstream firmware
 * stamps its DMA interrupt minus 40 us, about 8 us later (the AA start); the host only uses
 * differences. Set another value with ubt_le_set_start_back() to fold in the demodulator's
 * group delay.
 */
#define UBT_LE_START_BACK 158

/* A jump of the symbol timestamp above this (samples, nominal 4) is a gap: the squelch closed. */
#define UBT_LE_GAP_SAMPLES 12U

/* Default PDU payload bounds: legacy advertising 37, extended primary PDUs up to 64. */
#define UBT_LE_MAX_LEN_ADV  64U
#define UBT_LE_MAX_LEN_DATA 255U

struct ubt_le_stats {
	uint32_t hits;         /* AA correlator hits (including restarts) */
	uint32_t packets;      /* packets completely received */
	uint32_t emitted;      /* packets passed to the emit callback */
	uint32_t crc_bad;      /* packets with a failing CRC (dropped only with crc_verify) */
	uint32_t filtered;     /* advertising packets dropped by the target filter */
	uint32_t len_rejected; /* hits whose length field exceeded the bound */
	uint32_t gaps;         /* stream gaps (t jumps) seen */
	uint32_t aborted;      /* packets cut short by a gap */
	uint32_t restarts;     /* packets abandoned because another AA appeared inside them */
	uint32_t truncated;    /* emitted packets longer than the USB packet */
};

/* Side information of the packet being emitted, valid during the emit callback only. */
struct ubt_le_info {
	uint32_t t_start; /* sample index of the packet start (preamble) */
	uint32_t t_hit;   /* sample index of the last AA symbol */
	uint32_t aa_rx;   /* access address as received (differs when tolerance > 0) */
	uint16_t pdu_len; /* header + payload bytes of the packet, not cut at the USB size */
	bool crc_ok;
};

typedef void (*ubt_le_emit_fn)(void *ctx, const usb_pkt_rx *pkt);
/* Sample index (4 MS/s) to clk100ns (wraps at UBT_CLK100NS_WRAP), supplied by the clock glue. */
typedef uint32_t (*ubt_le_clk_fn)(void *ctx, uint32_t sample_index);
/* Symbol power (0.5 dB steps) to the raw RSSI byte (dBm + 54), supplied by ubt_rssi. */
typedef int8_t (*ubt_le_rssi_fn)(uint8_t pwr);

struct ubt_le {
	/* configuration, through the ubt_le_set_* functions */
	uint32_t aa;
	uint32_t aa_rev;     /* bit reversed: the shift register holds the first bit in bit 31 */
	uint32_t crc_init;   /* CRCInit as the CONNECT_REQ carries it */
	uint32_t crc_state0; /* the same bit reversed, start state of the table CRC */
	uint16_t mhz;
	uint8_t chan_idx;
	uint8_t w0; /* whitening sequence position at the first PDU bit */
	uint8_t tolerance;
	bool crc_verify;
	bool target_set;
	uint8_t max_len_adv;
	uint8_t max_len_data;
	int16_t start_back;
	uint8_t target[6]; /* address in over-the-air octet order */
	uint8_t target_mask[6];
	void *ctx;
	ubt_le_emit_fn emit;
	ubt_le_clk_fn clk;
	ubt_le_rssi_fn rssi;

	/* run state */
	uint8_t state;
	uint8_t warm; /* symbols to shift in before the correlator result is valid */
	uint8_t nb;   /* symbols of the byte in progress */
	uint8_t wpos;
	bool have_t;
	uint32_t sr; /* last 32 symbols, newest in bit 0 */
	uint32_t t_last;
	uint32_t t_hit;
	uint32_t aa_rx;
	uint16_t idx;     /* bytes after the AA received so far */
	uint16_t pdu_end; /* header + payload bytes, known after byte 1 */
	uint32_t crc;     /* running CRC register */
	uint32_t crc_rx;
	uint32_t bsum; /* symbol power sum of the byte in progress */
	uint32_t pmin, pmax, ptotal, nbytes;
	usb_pkt_rx pkt;
	struct ubt_le_info info;
	struct ubt_le_stats stats;
};

/* Population count without libgcc (SWAR). */
static inline unsigned int ubt_popcount32(uint32_t v)
{
	v = v - ((v >> 1) & 0x55555555U);
	v = (v & 0x33333333U) + ((v >> 2) & 0x33333333U);
	v = (v + (v >> 4)) & 0x0f0f0f0fU;
	return (v * 0x01010101U) >> 24;
}

/* Bit reversal of the low 24 or 32 bits. */
static inline uint32_t ubt_rev32(uint32_t v)
{
	v = ((v >> 1) & 0x55555555U) | ((v & 0x55555555U) << 1);
	v = ((v >> 2) & 0x33333333U) | ((v & 0x33333333U) << 2);
	v = ((v >> 4) & 0x0f0f0f0fU) | ((v & 0x0f0f0f0fU) << 4);
	v = ((v >> 8) & 0x00ff00ffU) | ((v & 0x00ff00ffU) << 8);
	return (v >> 16) | (v << 16);
}

static inline uint32_t ubt_rev24(uint32_t v)
{
	return ubt_rev32(v & 0xffffffU) >> 8;
}

/* The access address rules of Core Vol 6 Part B 2.1.2 for data channels. */
bool ubt_le_aa_valid(uint32_t aa);

/* Channel index (0..36 data, 37..39 advertising) of a frequency in MHz, 40 if not a BLE channel. */
unsigned int ubt_le_chan_idx(unsigned int mhz);

/*
 * Defaults after init: advertising AA and CRCInit, channel 2402, tolerance 0, crc_verify off,
 * no target. clk may be NULL (sample index * 2.5, modulo the clk100ns wrap), rssi may be NULL
 * (dBFS taken as dBm). The callbacks may use ubt_le_set_crc_init/crc_verify/target but not
 * set_channel/set_aa/reset (do those after ubt_le_feed returns).
 */
void ubt_le_init(struct ubt_le *le, ubt_le_emit_fn emit, ubt_le_clk_fn clk, ubt_le_rssi_fn rssi,
		 void *ctx);
/*
 * Listen on a frequency (MHz, 2402..2480, even): whitening, the channel field, and restarts
 * the finder. -1 and no change for anything else.
 */
int ubt_le_set_channel(struct ubt_le *le, uint16_t mhz);
void ubt_le_set_aa(struct ubt_le *le, uint32_t aa);
/* CRCInit, 24 bit, as in the CONNECT_REQ (advertising: 0x555555). */
void ubt_le_set_crc_init(struct ubt_le *le, uint32_t crc_init);
/* Drop packets with a bad CRC (default off: forwarded like upstream). */
void ubt_le_set_crc_verify(struct ubt_le *le, bool on);
/* Tolerated AA bit errors, 0..UBT_LE_MAX_TOLERANCE; -1 if out of range. */
int ubt_le_set_tolerance(struct ubt_le *le, unsigned int errors);
/*
 * BTLE_SET_TARGET: mac as the control request sends it (most significant octet first),
 * mask_bits 0..48 of its leading bits (0 clears the filter); -1 for more than 48. Applies to
 * advertising channel packets only, data channel packets always pass.
 */
int ubt_le_set_target(struct ubt_le *le, const uint8_t mac[6], unsigned int mask_bits);
/*
 * The same filter already in over-the-air octet order, as ubt_ctrl keeps it (address and mask,
 * least significant octet first); set = false clears it.
 */
void ubt_le_set_target_raw(struct ubt_le *le, bool set, const uint8_t addr[6],
			   const uint8_t mask[6]);
/* Length bounds of the PDU payload per channel kind, packets above are rejected. */
void ubt_le_set_max_len(struct ubt_le *le, uint8_t adv, uint8_t data);
void ubt_le_set_start_back(struct ubt_le *le, int samples);
/* Forget the packet in progress and the symbol history (retune, mode change). */
void ubt_le_reset(struct ubt_le *le);
/* True while a packet is being received (do not retune or end a window now). */
static inline bool ubt_le_busy(const struct ubt_le *le)
{
	return le->state != 0U;
}

/*
 * Run the finder over n symbols. Symbols must be in time order; a jump of t by more than
 * UBT_LE_GAP_SAMPLES (also from the previous call) restarts the search. Returns the number
 * of packets passed to the emit callback.
 */
size_t ubt_le_feed(struct ubt_le *le, const struct gfsk_sym *s, size_t n);

/* clk100ns (wraps at UBT_CLK100NS_WRAP) to a monotonic 64 bit count in 100 ns units. */
struct ubt_clk_unwrap {
	uint64_t base;
	uint32_t prev;
	bool init;
};

static inline uint64_t ubt_clk_unwrap(struct ubt_clk_unwrap *u, uint32_t clk)
{
	if (u->init && clk < u->prev && u->prev - clk > UBT_CLK100NS_WRAP / 2U) {
		u->base += UBT_CLK100NS_WRAP;
	}
	u->prev = clk;
	u->init = true;
	return u->base + clk;
}

/* ---- promiscuous mode ---- */

#define UBT_PROMISC_AA_SLOTS  32U
#define UBT_PROMISC_AA_ADOPT  4U /* adopt an AA seen more often than 3 times */
#define UBT_PROMISC_INTERVALS 5U /* consecutive agreeing hop interval estimates */
#define UBT_PROMISC_CRC_AGREE 2U /* agreeing CRCInit recoveries */

enum ubt_promisc_phase {
	UBT_PM_IDLE = 0,
	UBT_PM_AA,       /* scanning the raw stream for empty data PDUs, collecting AA candidates */
	UBT_PM_CRC,      /* synced on the AA, waiting for an empty PDU to run the CRC backwards */
	UBT_PM_INTERVAL, /* CRC verified, measuring the time between visits of the channel */
	UBT_PM_HOP_A,    /* listening on 2404 (channel 0) for the first packet */
	UBT_PM_HOP_B,    /* listening on 2406 (channel 1) for the second packet */
	UBT_PM_DONE,     /* AA, CRCInit, interval and increment known: hand over to le_conn */
};

/* LE_PROMISC message kinds (data[0] of the packet, as bt_promisc_le() and cb_btle() use them). */
enum ubt_promisc_msg {
	UBT_PM_MSG_AA = 0,        /* data[1..4] access address */
	UBT_PM_MSG_CRC_INIT = 1,  /* data[1..3] */
	UBT_PM_MSG_INTERVAL = 2,  /* data[1..2] in 1.25 ms units */
	UBT_PM_MSG_INCREMENT = 3, /* data[1] */
};

struct ubt_promisc_aa {
	uint32_t aa;
	uint32_t count;
};

struct ubt_promisc {
	struct ubt_le *le;
	ubt_le_emit_fn emit;
	void *emit_ctx;
	uint8_t phase;
	uint8_t aa_validate; /* reject candidates that cannot be a data channel AA (default on) */
	uint16_t scan_mhz;
	uint16_t want_mhz; /* retune request, see ubt_promisc_take_retune() */

	/* phase 1 scanner */
	uint32_t h0, h1; /* last 64 symbols */
	uint16_t pat;    /* whitened empty PDU header, NESN and SN bits masked */
	uint8_t warm;
	uint8_t crc_left; /* CRC symbols still to collect for the candidate, 0 = none */
	uint32_t cand_aa;
	uint16_t cand_hdr; /* the 16 header bits as received */
	uint32_t cand_t;   /* sample index of the last header symbol */
	uint32_t psum, pmin, pmax;
	uint32_t t_last;
	bool have_t;
	struct ubt_promisc_aa cache[UBT_PROMISC_AA_SLOTS];

	/* results, valid from the phase that learns them */
	uint32_t aa;
	uint32_t crc_init;
	uint16_t interval; /* 1.25 ms units */
	uint8_t hop;
	uint8_t last_idx;    /* channel index of the packet that completed the hop recovery */
	uint64_t t_last_pkt; /* 100 ns time of that packet */

	/* recovery state */
	uint32_t crc_cand;
	uint8_t crc_agree;
	uint64_t prev_t;
	bool prev_valid;
	uint64_t smallest; /* smallest time between visits, 100 ns */
	uint16_t est_interval;
	uint8_t consec;
	uint64_t first_t;
	uint64_t activity_t;
	bool activity_set;
	uint32_t nemit; /* calls of emit so far */
	struct ubt_clk_unwrap unwrap;
};

/* Hooks pm into le (it takes over le->emit and le->ctx) and forwards every packet to emit. */
void ubt_promisc_init(struct ubt_promisc *pm, struct ubt_le *le, ubt_le_emit_fn emit, void *ctx);
/* Phase 1 on a data channel (MHz, even, 2404..2478 except 2426); empties the AA cache. */
void ubt_promisc_start(struct ubt_promisc *pm, uint16_t mhz);
/*
 * The same with the AA given by the host (UBERTOOTH_SET_ACCESS_ADDRESS): skips phase 1, reports
 * the AA and waits for an empty PDU to recover CRCInit.
 */
void ubt_promisc_start_aa(struct ubt_promisc *pm, uint16_t mhz, uint32_t aa);
/*
 * Run the symbol stream through the current phase. In phase UBT_PM_AA the rest of the chunk
 * after an adopted AA is already fed to the finder. Returns the packets (and state messages)
 * passed to emit.
 */
size_t ubt_promisc_feed(struct ubt_promisc *pm, const struct gfsk_sym *s, size_t n);
/*
 * One packet seen in phase UBT_PM_CRC or later (what ubt_promisc_feed() does for every
 * packet the finder emits; exposed for event driven use and tests): mhz is the channel,
 * t100 the packet start in monotonic 100 ns units. Returns the frequency to listen on next
 * (0 = stay), also left in pm->want_mhz.
 */
unsigned int ubt_promisc_packet(struct ubt_promisc *pm, unsigned int mhz, uint64_t t100,
				const usb_pkt_rx *pkt);
/*
 * The frequency the recovery wants the radio on (0 = unchanged); clears the request and
 * points the finder at it. The glue retunes (3 to 4 ms) after this call and must discard
 * symbols that were captured before the retune completed.
 */
unsigned int ubt_promisc_take_retune(struct ubt_promisc *pm);
/*
 * Call now and then (100 ns time): after a silence of 10 s plus 80 hop intervals in a later
 * phase (wrong AA, connection ended; 160 s while the interval is still unknown, a channel
 * is visited every 37 intervals) the search restarts in phase 1; returns the frequency to
 * retune to like ubt_promisc_packet().
 */
unsigned int ubt_promisc_tick(struct ubt_promisc *pm, uint64_t now100);

#endif /* UBT_LE_H_ */
