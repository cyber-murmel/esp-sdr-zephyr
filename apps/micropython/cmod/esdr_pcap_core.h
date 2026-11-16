/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Pcap app's packet capture (eggs/pcap): apps/pcap's channel mode (doc/pcap.md, "Modes")
 * on the stream of the board's own radio. A power detector cuts one channel's samples into
 * bursts as apps/pcap's src/narrow.c does, and every burst goes through apps/pcap's decoders
 * in the order of its src/decoder.c: Bluetooth LE, IEEE 802.15.4, then the generic analysis
 * for what neither claimed (a band filling the channel goes out unanalysed, a burst beyond the
 * call's analysis budget as its metadata). Each packet comes out as the payload of a pcapng
 * packet on apps/pcap's interfaces (pcap_proto.h, PCAP_IF_*), byte for byte apps/pcap's
 * records, with its metadata comment.
 *
 * Plain C, no Zephyr and no MicroPython: the caller owns the memory (esdr_pcap_size()), feeds
 * samples in order and collects the packets of each call from esdr_pcap_next(); the pcapng
 * blocks of a saved capture come from here too. Built with the host compiler by
 * tests/host/test_pcap_egg.py.
 *
 * Two ways to run it. esdr_pcap_feed() detects and decodes in one call. Or the detector and the
 * decoders run on two threads: esdr_pcap_detect() closes bursts into a pool of
 * esdr_pcap_cfg.pool buffers, esdr_pcap_decode() takes them from there (a queue each way, one
 * thread at each end); a burst that finds no free buffer is lost (pool_full).
 */

#ifndef ESDR_PCAP_CORE_H_
#define ESDR_PCAP_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes of packets one call keeps at most (esdr_pcap_next()); later ones are counted lost. */
#define ESDR_PCAP_OUT_BYTES 24576U

struct esdr_pcap_cfg {
	/* The channel's centre (a channel of the Bluetooth LE or 802.15.4 grid), Hz. */
	uint64_t freq_hz;
	/* Samples a second: 2 or 4 MS/s (the decoders' rates). */
	uint32_t rate_hz;
	/* Left shift of the samples to the decoders' full scale (16384): 0 for an int16 stream of
	 * that full scale, 7 for 8-bit items. */
	uint8_t shift;
	/* Receive gain index for the comments, 255 automatic. */
	uint8_t gain;
	/* A block louder than the floor this many times opens a burst (power; 0: 4, 6 dB). */
	float ratio;
	/* PCAP_DEC_* (pcap_proto.h). */
	uint32_t decoders;
	/* Samples a burst holds at most (0: 5 ms); longer ones are cut (BURST_F_TRUNCATED). */
	uint32_t burst_max;
	/* A cycle counter for the decoders' times in the statistics (NULL: none kept). */
	uint32_t (*clock)(void);
	/* Burst buffers between the detector and the decoders, 2 to ESDR_PCAP_POOL_MAX (0: 2,
	 * enough for esdr_pcap_feed(), which decodes each burst as it closes). */
	uint32_t pool;
};

#define ESDR_PCAP_POOL_MAX 128U

/* A packet of the last call (esdr_pcap_next()). */
struct esdr_pcap_pkt {
	/* PCAP_IF_* */
	uint8_t iface;
	/* CRC or FCS good (Bluetooth LE, 802.15.4); a burst record: always. */
	bool ok;
	/* Power over the packet and the channel's floor, dBFS times 10. */
	int16_t power_x10, floor_x10;
	/* The stream sample index (esdr_pcap_feed()'s @p at axis) of its first sample. */
	int64_t at;
	int32_t offset_hz;
	uint32_t duration_us;
	/* The pcapng packet's payload and the comment (NUL terminated, NULL for none). */
	const uint8_t *rec;
	uint16_t len;
	const char *comment;
};

struct esdr_pcap_stats {
	/* Samples fed; gaps in their index (a burst open there ends cut). */
	uint64_t samples;
	uint32_t gaps;
	/* Bursts closed and decoded; too short (fewer than 3 loud blocks, unseen); cut at the
	 * buffer's end; lost for want of a free buffer in the pool. */
	uint32_t bursts, blips, truncated, pool_full;
	/* Packets by the decoders, good and with a failing CRC or FCS (sent with PCAP_DEC_BAD_CRC). */
	uint32_t ble, ble_bad, wpan, wpan_bad;
	/* Bursts analysed, recorded as a band filling the channel, recorded unanalysed (the
	 * budget), records not kept (a call's output full). */
	uint32_t generic, wide, skipped, lost;
	/* Cycles (esdr_pcap_cfg.clock) in the Bluetooth LE and 802.15.4 decoders and the generic
	 * analysis. */
	uint64_t cyc_ble, cyc_wpan, cyc_generic;
};

struct esdr_pcap;

/* Bytes esdr_pcap_init() needs for @p cfg (0: an invalid configuration). */
size_t esdr_pcap_size(const struct esdr_pcap_cfg *cfg);

/* The state in @p mem (8-byte aligned, esdr_pcap_size() bytes); NULL on a bad configuration. */
struct esdr_pcap *esdr_pcap_init(void *mem, size_t size, const struct esdr_pcap_cfg *cfg);

/* A new channel: an open burst ends cut, the floor is learnt again. */
void esdr_pcap_tune(struct esdr_pcap *c, uint64_t freq_hz, uint8_t gain);

/*
 * @p n samples (int16 I, Q interleaved, 4-byte aligned) whose first has index @p at on the
 * stream's axis; an index other than the one after the last call's is a gap. Closed bursts are
 * decoded at once; at most @p analyse of them go through the generic analysis (the rest as
 * records of their metadata). Forgets the packets of the call before. Returns the number of
 * packets.
 */
uint32_t esdr_pcap_feed(struct esdr_pcap *c, const int16_t *iq, uint32_t n, int64_t at,
			uint32_t analyse);

/* An open burst ends cut and is decoded (the capture stops), as esdr_pcap_feed(). */
uint32_t esdr_pcap_flush(struct esdr_pcap *c, uint32_t analyse);

/*
 * The detector's thread: as esdr_pcap_feed(), but each closed burst goes to the pool's queue for
 * esdr_pcap_decode(). esdr_pcap_end(): the open burst ends cut (the detector stops).
 */
void esdr_pcap_detect(struct esdr_pcap *c, const int16_t *iq, uint32_t n, int64_t at);
void esdr_pcap_end(struct esdr_pcap *c);

/*
 * The decoders' thread: up to @p max queued bursts decoded (at most @p analyse of them through
 * the generic analysis), their buffers back to the detector. Forgets the packets of the call
 * before; returns the number of packets (esdr_pcap_next()). esdr_pcap_pending(): bursts queued.
 */
uint32_t esdr_pcap_decode(struct esdr_pcap *c, uint32_t max, uint32_t analyse);

/* Of the last call's @p analyse, the generic analyses not used. */
uint32_t esdr_pcap_analyses_left(const struct esdr_pcap *c);
uint32_t esdr_pcap_pending(const struct esdr_pcap *c);

/* The packets of the last call in order: @p k from 0; false past the last. */
bool esdr_pcap_next(const struct esdr_pcap *c, uint32_t k, struct esdr_pcap_pkt *p);

void esdr_pcap_get_stats(const struct esdr_pcap *c, struct esdr_pcap_stats *st);

/* The rate the state was made for, Hz. */
uint32_t esdr_pcap_rate(const struct esdr_pcap *c);

/*
 * Bluetooth LE connections the decoders follow (PCAP_DEC_BLE_DATA): each state learns the access
 * address and CRC init of the CONNECT_IND PDUs it decodes, the newest few. esdr_pcap_learn() adds
 * one from elsewhere (another channel's state), esdr_pcap_learnt() reads the @p k th (from 0;
 * false past the last).
 */
void esdr_pcap_learn(struct esdr_pcap *c, uint32_t aa, uint32_t crc_init);
bool esdr_pcap_learnt(const struct esdr_pcap *c, uint32_t k, uint32_t *aa, uint32_t *crc_init);

/*
 * pcapng (apps/pcap/src/pcapng.c) as apps/pcap writes it: a section header with the names given
 * and apps/pcap's three interfaces (PCAP_IF_*, nanoseconds), or a packet's enhanced packet
 * block (inbound, @p comment NULL for none). Return the length, -ENOSPC when @p cap is short.
 * esdr_pcap_epb_size() is the block's length.
 */
int esdr_pcap_head(uint8_t *buf, size_t cap, const char *hardware, const char *os,
		   const char *appl);
int esdr_pcap_epb(uint8_t *buf, size_t cap, uint32_t iface, uint64_t t_ns, const void *rec,
		  uint32_t len, const char *comment);
size_t esdr_pcap_epb_size(uint32_t len, const char *comment);

#endif /* ESDR_PCAP_CORE_H_ */
