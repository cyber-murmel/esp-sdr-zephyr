/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio behind a link (esdr_remote.c): the WHY2025 badge's ESP32-C6 running
 * apps/osmosdr_serial, reached by its ESP32-P4 over the osmosdr serial protocol
 * (apps/osmosdr/src/esdr_proto.h) on SDIO, or on a UART as a fallback; the interface hides the
 * transport. On the MCH2022 badge's classic ESP32, esdr_local.c puts the chip's own radio
 * behind the same interface.
 */

#ifndef ESDR_REMOTE_H_
#define ESDR_REMOTE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esdr_defs.h"
#include "esdr_proto.h"

/* Opens the link and reads the radio's struct esdr_info; 0 or a negative errno. */
int esdr_remote_init(const struct esdr_info **info);

/*
 * One request and its reply: @p req (enum esdr_req) with @p value and @p len bytes of @p data;
 * an IN request (@p reply not NULL) takes at most @p reply_max bytes of reply data. Returns
 * the reply's data length, or the radio's negative errno, -ETIMEDOUT without a reply. A
 * request stops a stream the radio runs (it restarts after the reply).
 */
int esdr_remote_request(uint8_t req, uint16_t value, const void *data, uint16_t len,
			void *reply, uint16_t reply_max);

struct esdr_remote_rx_stats {
	/* Samples stored (or counted, without a buffer). */
	uint64_t samples;
	/* Holes in the radio's sample index (lost blocks) and their samples. */
	uint32_t gaps;
	uint64_t gap_samples;
	/* Blocks the radio flagged: it dropped samples, or the link did. */
	uint32_t overflows;
	/* Bytes the link dropped (receive ring full) and frames lost to resynchronization. */
	uint32_t ring_lost, resyncs;
};

/*
 * Receive at @p rate S/s into @p buf (int16 I, Q pairs; NULL: count only), @p cap pairs;
 * @p count pairs then stop (0: endless). @p wrap: @p buf is a ring. The monitor
 * (esdr_rx_latest(), esdr_rx.h) sees every sample.
 */
int esdr_remote_rx_start(uint32_t rate, int16_t *buf, size_t cap, uint64_t count, bool wrap);
/*
 * As esdr_remote_rx_start(), in bursts of ESDR_BURST_MAX samples at the radio's native rate
 * @p mhz (contiguous within a burst only), kept at 1 / @p decim (1, or 2 through a half-band
 * filter), one after another until stopped or @p count.
 */
int esdr_remote_rx_start_burst(uint16_t mhz, unsigned int decim, int16_t *buf, size_t cap,
			       uint64_t count);
void esdr_remote_rx_stop(void);
bool esdr_remote_rx_busy(void);
uint64_t esdr_remote_rx_count(void);
void esdr_remote_rx_stats(struct esdr_remote_rx_stats *st);

/*
 * Transmit at @p rate S/s, by a feeder thread: @p n pairs of @p buf (int16 I, Q), once or
 * @p repeat, or without a buffer the generator @p gen; @p count pairs then stop (0: until the
 * buffer ends, or endless); @p scale on each sample. The radio is in transmit mode meanwhile.
 */
int esdr_remote_tx_start(uint32_t rate, const int16_t *buf, size_t n, bool repeat,
			 const struct esdr_gen *gen, uint64_t count, float scale);
/* The generator and the scale change while the signal plays. */
void esdr_remote_tx_set_gen(const struct esdr_gen *gen);
void esdr_remote_tx_set_scale(float scale);
void esdr_remote_tx_stop(void);
bool esdr_remote_tx_busy(void);
/* Samples the radio has played: those sent, one short until the radio has played them all. */
uint64_t esdr_remote_tx_played(void);

struct esdr_remote_trig_stats {
	/* Snapshots complete, and lost to a missing block. */
	uint32_t snapshots, broken;
};

/*
 * Triggered snapshots (ESDR_MODE_TRIGGER): the radio sends ESDR_TRIG_SAMPLES samples at 80 MS/s
 * (int8 I, Q) around each burst whose power passes @p ratio_x16 / 16 times the quiet baseline,
 * @p post of them after it. Stops a receive run.
 */
int esdr_remote_trig_start(uint16_t ratio_x16, uint32_t post);
void esdr_remote_trig_stop(void);
/*
 * The newest complete snapshot into @p dst (2 * ESDR_TRIG_SAMPLES bytes) if its sequence number
 * (from 1) is not @p after: true with @p seq and @p trig (samples before the trigger).
 */
bool esdr_remote_trig_get(int8_t *dst, uint32_t after, uint32_t *seq, uint32_t *trig);
void esdr_remote_trig_stats(struct esdr_remote_trig_stats *st);

/*
 * Gapless receive (a radio with ESDR_MODE_GAPLESS: the badge's C6, gapless by default): the
 * radio keeps the windows @p cfg selects of the whole 80 MHz and sends them as segment blocks
 * (esdr_seg.h). With @p monitor their samples go to the monitor (esdr_rx_latest()) at 32 times
 * their raw units, full scale 16384 as bursts; every block goes to the hook too. -ENOTSUP (or
 * the radio's error) from a radio without the mode.
 */
struct esdr_remote_gl_stats {
	/* Segment blocks and segments taken, blocks that did not parse, holes on the axis. */
	uint32_t blocks, segments, bad, gaps;
	uint64_t pairs;
};

int esdr_remote_gl_start(const struct esdr_gapless_cfg *cfg, bool monitor);
void esdr_remote_gl_stop(void);
bool esdr_remote_gl_busy(void);
void esdr_remote_gl_stats(struct esdr_remote_gl_stats *st);
/* The radio's own counters (ESDR_REQ_GET_GAPLESS_STATS): 0 or a negative errno. */
int esdr_remote_gl_radio_stats(struct esdr_gapless_stats *st);
/*
 * Every segment block also to @p hook (NULL: none), in the parser's thread: it must copy what
 * it keeps and return soon.
 */
void esdr_remote_gl_hook(void (*hook)(const uint8_t *blk));

/* Every stream stopped (receive, transmit, snapshots), the radio's settings as it started. */
void esdr_remote_reset(void);

#endif /* ESDR_REMOTE_H_ */
