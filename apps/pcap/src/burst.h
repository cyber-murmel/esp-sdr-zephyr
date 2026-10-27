/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A burst: one channel's samples from the detector's opening to its closing (doc/pcap.md).
 * The assembler (burst.c, the channelizer thread) fills one from the detector's events; the
 * decoder thread reads it. Plain C: the decoders and their tests on native_sim use this header
 * without Zephyr.
 */

#ifndef BURST_H_
#define BURST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frames (complex samples) a burst holds at most: 8.2 ms at 4 MS/s. */
#define BURST_MAX_SAMPLES 32768U
/* Adjacent channels open at once that make a wideband signal (the pool's size is
 * CONFIG_APP_PCAP_BURSTS). */
#define BURST_WIDE_MIN 6U

/* struct burst.flags */
/* Cut at BURST_MAX_SAMPLES while the channel stayed open. */
#define BURST_F_TRUNCATED 0x0001U
/* Part of a wideband signal: no samples kept (n is 0), only the metadata. */
#define BURST_F_WIDEBAND  0x0002U
/* Frames of the filterbank's transient among the samples (a gap in the input). */
#define BURST_F_TRANSIENT 0x0004U
/* A neighbouring channel was open at the same time. */
#define BURST_F_NEIGHBOUR 0x0008U
/* The C6 dropped windows before this burst (ESDR_SEG_DROPPED). */
#define BURST_F_DROPPED   0x0010U
/* Samples at the receiver's full scale. */
#define BURST_F_CLIPPED   0x0020U
/* Still open when the capture stopped. */
#define BURST_F_CUT       0x0040U
/* Ended at a hole in the radio's stream (the C6 kept nothing for a while): what follows is
 * another burst. */
#define BURST_F_HOLE      0x0080U
/* A decoder emitted a packet from it (the decoder thread's: no generic analysis follows). */
#define BURST_F_CLAIMED   0x0100U
/* gate_snr and gate_narrow are set (the decoder thread's). */
#define BURST_F_GATED     0x0200U

struct burst {
	/* The channelizer's channel index and count, the channel's rate and centre frequency. */
	uint16_t chan, channels;
	uint32_t rate_hz;
	uint64_t freq_hz;
	/*
	 * The first sample's frame index on the channel's absolute grid (its pair index is
	 * m0 * D, D = channels / 2, on the C6's axis: 80 MS/s, or 40 with ESDR_GAPLESS_F_HALF): the
	 * decoders and the combination of two neighbours go by it.
	 */
	uint64_t m0;
	/* Samples in iq (frames), their first and last one that the detector found loud. */
	uint32_t n, loud_from, loud_to;
	/* The detector's floor and the highest block power of the burst, output units squared
	 * (a full-scale carrier, 512 raw units, is 16384 in the samples: 16384^2 here). */
	float floor, power;
	/* BURST_F_* */
	uint16_t flags;
	/* Receive gain index of the segment that opened it (ESDR_SEG_GAIN_AUTO: automatic). */
	uint8_t gain;
	/* Reference count, the decoder thread's (a burst held for its neighbour). */
	uint8_t ref;
	/* For wideband bursts: the lowest and highest adjacent channel open with this one. */
	uint16_t wide_lo, wide_hi;
	/* The decoder thread's look at the loud part (BURST_F_GATED): power above the floor over
	 * the floor, the correlation one sample apart over that power. */
	float gate_snr, gate_narrow;
	/* n interleaved I, Q samples, 4-byte aligned, in the pool's buffer. */
	int16_t *iq;
};

/* dB relative to the receiver's full scale, times 10, of a power in output units squared. */
int16_t burst_dbfs_x10(float power);

#endif /* BURST_H_ */
