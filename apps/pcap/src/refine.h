/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Refinement of a narrow burst (doc/pcap.md, "Refinement"): the stream mixed to the signal's
 * centre and decimated by a power of two through half-band stages, so that a signal much
 * narrower than its bin gets a rate fitting its bandwidth and loses the noise outside it. The
 * generic decoder applies it recursively: measure, refine while the signal fills less than a
 * quarter of the rate, measure again. Plain C, no Zephyr: unit tested on native_sim.
 */

#ifndef REFINE_H_
#define REFINE_H_

#include <stddef.h>
#include <stdint.h>

#include "burst.h"

/* Decimation at most, in all: 4 MS/s down to 62.5 kS/s. */
#define REFINE_DECIM_MAX 64U

/*
 * The decimation a signal of @p bandwidth_hz (occupied, 90 % of the power) in a stream of
 * @p rate_hz calls for: the largest power of two up to REFINE_DECIM_MAX that keeps
 * rate / decim >= 4 bandwidth (the occupied band twice oversampled with room for the half-band
 * transition); 1 when the signal fills the rate or the bandwidth is unknown (0).
 */
unsigned int refine_decim(uint32_t rate_hz, uint32_t bandwidth_hz);

/* Scratch bytes refine_run() needs for a stream of @p n samples. */
size_t refine_scratch(uint32_t n);

/*
 * @p b mixed down by @p offset_hz (the signal's centre against b->freq_hz; the mixer's phase
 * starts at 0 at the first sample) and decimated by @p decim (1, 2, 4 .. REFINE_DECIM_MAX;
 * -EINVAL otherwise), one half-band stage per factor of two, into @p out (room for b->n / decim
 * interleaved pairs; samples saturate to int16). @p view describes the result: rate_hz / decim,
 * freq_hz + offset_hz, m0 and n, loud_from and loud_to scaled down, iq = out, the rest copied
 * (chan, channels, floor and power scaled by the stages' noise bandwidth: the floor of the
 * narrower stream). 0 or a negative errno.
 */
int refine_run(const struct burst *b, int32_t offset_hz, unsigned int decim, int16_t *out,
	       void *scratch, size_t scratch_len, struct burst *view);

#endif /* REFINE_H_ */
