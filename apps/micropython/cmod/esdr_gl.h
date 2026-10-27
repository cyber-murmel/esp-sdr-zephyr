/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gapless stream's channelizer on the WHY2025 badge's ESP32-P4 (esdr_gl.c): the radio's
 * segment blocks (esdr_remote_gl_hook()) through the fixed-point filterbank and burst detector
 * of ../../common/include/esdr_pfbq.h in a thread of its own; per channel the power, floor,
 * peak and how often it was open since the last look, the bursts that ended, and rows of a
 * spectrum of the band made of the bursts alone.
 *
 * Channels are reported in display order, the lowest frequency first: entry i is channel
 * (i + M / 2) mod M, offset (i - M / 2) * 80 MHz / M from the centre. Powers are in dB of a
 * full-scale carrier (512 raw units) at the channel's centre.
 */

#ifndef ESDR_GL_H_
#define ESDR_GL_H_

#include <stdint.h>

/* Up to this many bursts wait for esdr_gl_bursts(); older ones are dropped (counted). */
#define ESDR_GL_BURSTS 64U

/*
 * Starts the channelizer on @p channels (16, 20, 32, 40, 64 or 80) with the detector opening at
 * @p open_db over a channel's floor; the radio's stream must be started too
 * (esdr_remote_gl_start()). 0, -EINVAL for an unsupported @p channels.
 */
int esdr_gl_start(unsigned int channels, float open_db);
/* Stops it (what it collected stays readable). */
void esdr_gl_stop(void);
unsigned int esdr_gl_channels(void);

/*
 * Per channel since the last call: mean power, the floor now, the largest block power (dB), the
 * share of blocks it was open; any pointer may be NULL, each takes esdr_gl_channels() floats.
 * Returns the blocks the detector decided in that time.
 */
uint32_t esdr_gl_power(float *power, float *floor, float *peak, float *open);

struct esdr_gl_burst {
	/* Channel offset from the centre, Hz; start (s on the stream's axis since the start) and
	 * length (s) from the block that opened to the end of the last loud one (detector blocks
	 * of 16 frames); loudest block, dB. */
	float offset_hz, start_s, length_s, peak_db;
};

/* The bursts that ended since the last call, oldest first, up to @p max. Returns the count. */
unsigned int esdr_gl_bursts(struct esdr_gl_burst *out, unsigned int max);

/* Bins of a spectrum row a channel (esdr_gl_spectrum()), and at most. */
#define ESDR_GL_SPEC_BINS     8U
#define ESDR_GL_SPEC_BINS_MAX (80U * ESDR_GL_SPEC_BINS)

/*
 * A row of the bursts' spectrum since the last call to @p out, n = ESDR_GL_SPEC_BINS M bins,
 * dB of a full-scale carrier, the lowest frequency first: bin j at (j - n / 2) * 80 MHz / n
 * from the centre (250 kHz at M = 40). Each burst the detector found reads at its channel's
 * place in the band: the mean over its loud blocks of their 16-frame transforms (Hann), the
 * largest of the row's bursts per bin. The other bins read their channel's floor, a few dB
 * either side at random: the time the radio kept nothing and the channels without a burst
 * look quiet. Returns n, -ENODEV if the channelizer does not run, -EINVAL if @p max is short.
 * The first call starts the transforms; they stop a second after the last.
 */
int esdr_gl_spectrum(float *out, unsigned int max);

struct esdr_gl_stats {
	/* Segment blocks taken and dropped (the thread fell behind), pairs channelized, CPU cycles
	 * that took; bursts ended, and dropped from a full ring; the thread's stack never used. */
	uint32_t blocks, dropped, bursts, bursts_lost, stack_free;
	uint64_t pairs, cycles;
};

void esdr_gl_stats(struct esdr_gl_stats *st);

#endif /* ESDR_GL_H_ */
