/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 1 Msym/s GFSK demodulator (Bluetooth BR and LE) for the 4 MS/s planar stream of the ring
 * receiver (ESP_SDR_RING_DECIM_BOX4 with shift_fs4: sums of 4, or every 4th pair) as the sample
 * ring stores it: 8 bits, the int16 stream >> 6. The wanted channel sits at DC in that stream,
 * the LO 4 MHz below it (at +1 MHz until 2026-11-12, see gfsk_ref.h).
 * If the hardware turns out to mirror the spectrum (the deviation inverted), pass the q plane as
 * @p i and the i plane as @p q: that conjugates the signal at no cost.
 *
 * It decides bit for bit as the reference gfsk_ref does on the int16 stream (x << 6) + 32, each
 * 8-bit sample at the middle of the floor's step that stored it: no bias at DC, where the channel
 * sits, and no zero level (rounding instead left the quiet in-channel noise of gain 40 all zeros)
 * (the demodulator as of commit 0918f8f and the changes to the algorithm gfsk_ref.h lists), cut into
 * the same pushes: its sample loop computes the same discriminator and powers in int8 units,
 * scaled back exactly.
 *
 * Signal path, per sample: the sum of 4 samples (computed ahead of the paths for a stretch of
 * the input, on the ESP32-S3 by the vector unit) puts nulls on every multiple of 1 MHz from the
 * channel: the neighbours at +-1 MHz (BR) or +-2 MHz (LE) fall into them. A delay-2
 * discriminator follows; its output is summed over
 * 2 samples around the symbol centre and sliced against the middle of the two levels it takes
 * (so a carrier offset is absorbed). tools/gfsk_sim.c holds the numbers behind these choices.
 *
 * Symbol timing: when the signal power rises (a burst), the sampling phase is picked from the
 * four candidates by the opening of the eye over the alternating preamble; afterwards an
 * early/late comparison moves it by one sample when the eye is better on one side (it follows
 * 100 ppm over a 3 ms packet). The decisions run GFSK_LAT_SYMS symbols behind the newest
 * sample, so the phase and the slicing levels of a packet's first symbols are estimated from
 * the symbols that follow them: the preamble is not lost to a lock time.
 *
 * Power: every symbol carries the smoothed in-channel power in the 0.5 dB steps of gfsk_sym.h,
 * relative to a complex tone of amplitude 8192 at the channel centre (0 dBFS, pwr 200). The
 * boxcar gain of 4 (power 16) is divided out; a modulated carrier reads up to 0.9 dB lower
 * (the boxcar rolls off towards the deviation), which PWR_COMP_BLE compensates.
 *
 * Squelch: an energy detector on the same power. Closed, only a cheap detector runs (one
 * FIR output per 4 samples) and no symbols are produced; when it opens, the last
 * GFSK_PRE_SAMPLES samples are replayed through the full path so that no preamble is lost.
 * "Always emit" keeps the full path running and produces a symbol per symbol period; the
 * threshold then only decides when the sampling phase and slicing levels are acquired anew (a
 * new burst).
 */

#ifndef GFSK_DEMOD_H_
#define GFSK_DEMOD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfsk_sym.h"

/* Decisions trail the newest sample by this many symbols (look-ahead of the estimates). */
#define GFSK_LAT_SYMS    12U
/* Samples replayed when the squelch opens (multiple of 4). */
#define GFSK_PRE_SAMPLES 40U
/* Rings: samples (power of two >= 4 * (LAT + chunk) + 12) and group powers (>= LAT + chunk + 4). */
#define GFSK_CHUNK       8U
#define GFSK_RING        128U
#define GFSK_PRING       32U
/*
 * The input of gfsk_demod_push(): GFSK_IN_HIST readable samples before the first (the replay
 * reaches 42 back, the FIR's block of 16 holding it up to 15 more and the vector unit's the 16
 * before that; a multiple of 16 keeps the reader's copy aligned), and on the ESP32-S3 GFSK_IN_TAIL
 * after the last (the vector unit computes whole blocks of 16).
 */
#define GFSK_IN_HIST     80U
#define GFSK_IN_TAIL     15U

/*
 * Symbols one gfsk_demod_push() of n samples can emit at most: one per 4 samples, plus the
 * pipeline (look-ahead) flushed when a squelch closes or after a gap, plus the pre-roll replayed
 * when the squelch opens. The caller's out_cap must be at least this; gfsk_demod_flush() needs
 * GFSK_LAT_SYMS + GFSK_CHUNK + 2. Symbols beyond out_cap are dropped.
 */
#define GFSK_OUT_CAP(n) \
	((size_t)(n) / 4U + GFSK_LAT_SYMS + GFSK_CHUNK + GFSK_PRE_SAMPLES / 4U + 4U)

struct gfsk_demod {
	/* Configuration, kept by gfsk_demod_reset(). */
	uint8_t mod;
	uint8_t always;
	uint8_t pwr_thr;
	uint8_t pad;
	uint16_t hold;
	uint32_t s_thr, s_close;

	/* Stream. */
	uint32_t n;      /* index of the next sample the caller will push */
	uint32_t f;      /* index of the next sample to run through the active path */
	uint32_t lg;     /* first sample of the next group the group logic handles */
	uint32_t replay_end;
	uint32_t start;  /* first sample of the current run of the full path */
	uint16_t hist_n; /* samples of the input before n since the last reset or gap, up to 64 */
	uint8_t open;    /* the full path runs */
	uint8_t flushing;/* a gap happened: emit what is pending first */
	uint32_t flush_f; /* ... from the samples before it: f where the gap began */
	uint8_t front_ok;

	uint32_t cs;          /* running sum of the discriminator, ring entry of f - 1 */

	/* Detector. */
	uint32_t savg, floor;
	uint32_t s_gate;
	uint16_t hold_cnt;
	uint8_t s_init;

	/*
	 * Burst edge: two groups above the gate after a pause or a step up start a new acquisition
	 * (a step up inside a run starts the run over).
	 */
	uint8_t off, run, pw_c;
	uint32_t pend;        /* first sample of the group that may start a burst */
	uint32_t sbase;       /* the smoothed power before it */
	uint32_t edge;        /* burst start estimate */

	/* Sampling phase: window ends at index & 3 == ph; the early/late sums. */
	uint8_t ph;
	uint8_t el_cnt, el_tog;
	int32_t el_e;
	uint32_t el_a;

	/* Slicing levels: the mean window sums decided as 0 and as 1. */
	int32_t m0, m1;
	uint8_t seeded;

	/* Decision cursor. */
	uint32_t cur;         /* window end index of the next symbol */
	uint8_t cur_valid, need_cursor, sliced;

	/* Histories. */
	uint32_t cring[GFSK_RING];
	uint32_t pring[GFSK_PRING]; /* power of each group, raw */
	uint8_t wring[GFSK_PRING];  /* the smoothed power of each group, pwr units */
};

/** Set up for @p mod; automatic lock threshold, always emitting (see gfsk_demod_set_squelch). */
void gfsk_demod_init(struct gfsk_demod *d, enum gfsk_modulation mod);

/** Forget the stream: the sample counter restarts at 0, the configuration is kept. */
void gfsk_demod_reset(struct gfsk_demod *d);

/**
 * @brief Squelch and lock threshold.
 *
 * @param pwr_threshold In-channel power (0.5 dB steps, gfsk_sym.h) the detector must reach to
 *        open; it closes after @p hold_syms symbols below 3 dB under it. 0: no squelch, always
 *        emit, with the lock threshold 8 dB above the noise floor the demodulator tracks itself.
 * @param hold_syms Symbols of power below the close level before the squelch closes (>= 1).
 * @param always_emit Emit a symbol every symbol period whether the detector is open or not
 *        (RX_SYMBOLS with carrier sense off); the threshold then only restarts the acquisition
 *        for a new burst (at pwr_threshold - 3 dB; with 0 at 8 dB above the noise floor).
 */
void gfsk_demod_set_squelch(struct gfsk_demod *d, uint8_t pwr_threshold, uint16_t hold_syms,
			    bool always_emit);

/**
 * @brief Run n samples (planar int8, the stream >> 6) through the demodulator.
 *
 * Symbols appear GFSK_LAT_SYMS symbols late; gfsk_sym.t counts samples from the last reset
 * across all calls and stamps the sample at which the symbol was on the air.
 *
 * @param i, q Stream sample gfsk_demod_now() and the n after it; the GFSK_IN_HIST bytes before
 *        them must be readable (the samples pushed before, where there are any: the history;
 *        after a reset or a gap they are garbage and never used). On the ESP32-S3 the vector
 *        unit computes the FIR when the address of every sample is its index modulo 16 (as the
 *        ring reader lays the planes out) and the GFSK_IN_TAIL bytes after the n samples are
 *        readable too; otherwise C does, bit for bit the same.
 * @param out_cap At least GFSK_OUT_CAP(n); symbols beyond it are dropped.
 * @return Symbols written.
 */
size_t gfsk_demod_push(struct gfsk_demod *d, const int8_t *i, const int8_t *q, size_t n,
		       struct gfsk_sym *out, size_t out_cap);

/**
 * The FIR and the discriminator as the vector unit computes them (ESP32-S3) against the same in
 * C, over 8 planes of pseudo-random and extreme samples from @p seed: the samples that differ
 * (0 without the unit).
 * Not while a push runs.
 */
uint32_t gfsk_demod_fir_check(uint32_t seed);

/**
 * The per group logic as the S3 runs it (gfsk_groups.S) against its C, over 4000 random states
 * from @p seed: the cases that differ (0 elsewhere). Not while a push runs.
 */
uint32_t gfsk_demod_groups_check(uint32_t seed);

/**
 * The slicer as the S3 runs an acquired burst (gfsk_slice.S) against its C, over 4000 random
 * states from @p seed: the cases that differ (0 elsewhere). Not while a push runs.
 */
uint32_t gfsk_demod_slice_check(uint32_t seed);

/** Emit the symbols still in the look-ahead (end of a stream, mode change). */
size_t gfsk_demod_flush(struct gfsk_demod *d, struct gfsk_sym *out, size_t out_cap);

/**
 * The ring dropped @p n_lost samples: the counter jumps over them so t stays right, the pending
 * symbols are emitted by the next push, and the filter and the estimates restart.
 */
void gfsk_demod_gap(struct gfsk_demod *d, uint32_t n_lost);

/** Smoothed in-channel power now, pwr units (noise floor when idle). */
uint8_t gfsk_demod_level(const struct gfsk_demod *d);

/** Tracked noise floor (the automatic lock threshold minus 8 dB), pwr units. */
uint8_t gfsk_demod_noise_floor(const struct gfsk_demod *d);

/**
 * Carrier offset of the last burst in Hz (positive: the signal is above its nominal frequency),
 * from the middle of the two levels against their distance; rough (+-25 %), for diagnostics
 * and an offset correction of the LO. 0 before the first burst.
 */
int32_t gfsk_demod_cfo_hz(const struct gfsk_demod *d);

/** Samples consumed so far (the index of the next one). */
static inline uint32_t gfsk_demod_now(const struct gfsk_demod *d)
{
	return d->n;
}

/**
 * Index (in the units of gfsk_sym.t) before which no further symbol will be emitted: all the
 * symbols with t below it have been emitted or never will be. For filling the idle time between
 * bursts when the squelch is closed.
 */
uint32_t gfsk_demod_horizon(const struct gfsk_demod *d);

/** Power measure S (8 A^2 for a tone of amplitude A) to pwr units, and back. */
uint8_t gfsk_pwr_from_s(uint32_t s);
uint32_t gfsk_s_from_pwr(uint8_t pwr);

#ifdef GFSK_PROF
/* Cycles in the parts of the full path (Xtensa CCOUNT), for profiling builds (ubt rx bench). */
struct gfsk_prof {
	uint32_t hot, groups, slice, front;
	/* Samples through the full path, and times the squelch closed. */
	uint32_t open, opens;
};
extern struct gfsk_prof gfsk_prof;
#endif

#endif /* GFSK_DEMOD_H_ */
