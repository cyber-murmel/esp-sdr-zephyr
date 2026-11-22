/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, the checks of what a shell command may set (doc/dev/txs.md): the RF band with
 * the tone's offset, and the limits of the tuning knobs. Plain C, header only, runs on native_sim.
 */

#ifndef TXS_LIMITS_H_
#define TXS_LIMITS_H_

#include <stdbool.h>
#include <stdint.h>

/* The ISM band in kHz, and the guard kept to both of its edges for the carrier and the tone's image. */
#define TXS_RF_BAND_LO_KHZ 2400000U
#define TXS_RF_BAND_HI_KHZ 2483500U
#define TXS_RF_GUARD_KHZ   1000U
/* The tone of the stream: k cycles per this many samples */
#define TXS_TONE_SAMPLES   351U

/** The tone's frequency in kHz (rounded up): k cycles per 351 samples at msps MS/s. */
static inline uint32_t txs_tone_khz(uint32_t k, uint32_t msps)
{
	return (k * msps * 1000U + TXS_TONE_SAMPLES - 1U) / TXS_TONE_SAMPLES;
}

/**
 * Whether an RF run may go on air: the carrier is the tuned MHz plus the LO offset (kHz), the tone
 * lies on one side of it and its image on the other, so both centre + tone and centre - tone must
 * be inside the band, a guard from either edge.
 */
static inline bool txs_rf_ok(uint32_t centre_mhz, int32_t lo_ofs_khz, uint32_t k, uint32_t msps)
{
	const int64_t c = (int64_t)centre_mhz * 1000 + lo_ofs_khz;
	const int64_t t = txs_tone_khz(k, msps);

	return centre_mhz != 0U && c - t >= (int64_t)(TXS_RF_BAND_LO_KHZ + TXS_RF_GUARD_KHZ) &&
	       c + t <= (int64_t)(TXS_RF_BAND_HI_KHZ - TXS_RF_GUARD_KHZ);
}

/* `tx cfg`: the ownership write latency in cycles (0 measures it), the late tolerance in words, the
 * step overhead in cycles; `tx inject stall` in ms.
 */
#define TXS_LAT_MIN   20U
#define TXS_LAT_MAX   200U
#define TXS_OVH_MAX   2000U
#define TXS_TOL_MAX   1024U
/* a stall beyond about 100 ms at 20 MS/s ends the run by design: 64 zero passes are the hang rule */
#define TXS_STALL_MAX 50U

static inline bool txs_lat_ok(uint32_t v)
{
	return v == 0U || (v >= TXS_LAT_MIN && v <= TXS_LAT_MAX);
}

static inline bool txs_ovh_ok(uint32_t v)
{
	return v <= TXS_OVH_MAX;
}

static inline bool txs_tol_ok(uint32_t v)
{
	return v <= TXS_TOL_MAX;
}

static inline bool txs_stall_ok(uint32_t ms)
{
	return ms <= TXS_STALL_MAX;
}

#endif
