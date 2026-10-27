/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the parts of apps/pcap share (doc/pcap.md): the capture's settings, the time base, the
 * burst pool and the decoder's queue, the counters. main.c owns the settings and changes them
 * only while no capture runs.
 */

#ifndef APP_H_
#define APP_H_

#include <stdbool.h>
#include <stdint.h>

#include "burst.h"
#include "esdr_proto.h"
#include "pcap_proto.h"

/* The capture's settings: what the host or the shell set, taken at each start. */
struct app_cfg {
	/* enum pcap_mode and PCAP_MODE_F_*; the alternation's dwell times (both 0: none). */
	uint16_t mode, mode_flags;
	uint32_t survey_ms, channel_ms;
	/* The survey's centre and the channel mode's frequency and rate, as asked for. */
	uint64_t survey_hz, channel_hz;
	uint32_t channel_rate_hz;
	/* Receive gain index or ESDR_GAIN_AUTO. */
	uint16_t rx_gain;
	/* The channelizer: M, the detector's opening ratio, the channels kept. */
	uint16_t channels;
	float open_ratio;
	uint32_t mask[3];
	/* The C6's gapless settings (ESDR_REQ_SET_GAPLESS as the host sent it). */
	struct esdr_gapless_cfg gapless;
	/* What runs over the bursts. */
	struct pcap_decoders dec;
};

extern struct app_cfg app_cfg;

/* ---- time ---- */

/* The host's time: nanoseconds since the Unix epoch now (PCAP_REQ_SET_TIME). */
void timebase_set(uint64_t epoch_ns);
/* A run's first block: its time on the stream's axis (ns) is now. */
void timebase_anchor(uint64_t axis_ns);
/* The host time of a time on the stream's axis (the radio's clock: 80 or 40 MS/s, or the
 * channel).
 */
uint64_t timebase_ns(uint64_t axis_ns);
/* The host time now. */
uint64_t timebase_now_ns(void);

/* Sample @p m of a stream at @p rate_hz on the axis, ns (the rates divide 10^9). */
static inline uint64_t axis_ns(uint64_t m, uint32_t rate_hz)
{
	return m * (1000000000U / rate_hz);
}

/* ---- the burst pool (burst.c) ---- */

/* A free burst with its buffer, NULL when none; back with burst_put(). Any thread. */
struct burst *burst_get(void);
void burst_put(struct burst *b);
uint32_t burst_free_count(void);

/* ---- the decoder (decoder.c) ---- */

void decoder_init(void);
/* A closed burst for the decoder: 0, or -ENOSPC (the caller still owns it). */
int decoder_put(struct burst *b);
/* A wideband burst the C6 reported (a run of its width records): its records' sums. */
struct wide_run {
	/* The radio's centre, and the run's first and last pair on its 80 MS/s axis. */
	uint64_t centre_hz;
	uint64_t index, end;
	float r1_i, r1_q;
	int16_t power_dbfs_x10;
	uint8_t gain;
	uint16_t records;
};
int decoder_put_wide(const struct wide_run *w);
/* Bursts queued and waiting. */
uint32_t decoder_backlog(void);
/*
 * Bluetooth LE access addresses the decoder looks for besides the advertising one
 * (PCAP_REQ_ADD_AA): @p op 0 adds @p aa with @p crc_init, 1 removes it, 2 forgets all (the
 * learnt ones too). 0, -ENOSPC or -EINVAL.
 */
int decoder_aa(uint16_t op, uint32_t aa, uint32_t crc_init);

/* ---- the stream thread and its modes (survey.c, narrow.c) ---- */

/*
 * The stream thread to the survey of the gapless segments around @p centre_hz (the radio's
 * centre) at @p in_rate_hz (the C6's rate: ESDR_GAPLESS_RATE_HZ, or half with
 * ESDR_GAPLESS_F_HALF), to the channel mode at @p freq_hz and @p rate_hz, or to idle; what was
 * open goes to the decoder, cut. Called from the control thread: they wait for the stream
 * thread. 0 or a negative errno.
 */
int survey_start(uint64_t centre_hz, uint32_t in_rate_hz);
/* Whether the survey can run its bank at @p in_rate_hz: 0, -EINVAL or -ENOMEM. */
int survey_check(uint32_t in_rate_hz);
int stream_narrow(uint64_t freq_hz, uint32_t rate_hz);
void survey_stop(void);
/* The radio's sink (radio.h) while a mode runs. */
void survey_block(uint8_t *blk);

/* The channel mode's parts, run by the stream thread (narrow.c). */
int narrow_start(uint64_t freq_hz, uint32_t rate_hz);
void narrow_stop(void);
void narrow_block(const uint8_t *blk);

/* ---- counters ---- */

struct app_stats {
	uint32_t seg_blocks, seg_dropped, other_blocks;
	uint64_t pairs, chan_cycles;
	uint32_t bursts, bursts_lost, bursts_wide;
	uint32_t decoded;
	uint64_t dec_cycles;
	uint64_t channel_samples;
	uint32_t channel_gaps, c6_wide;
	uint32_t generic, generic_skipped, generic_wide;
	uint64_t generic_cycles;
};

extern struct app_stats app_stats;

#endif /* APP_H_ */
