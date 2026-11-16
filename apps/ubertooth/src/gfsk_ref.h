/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The reference demodulator: gfsk_demod as of commit 0918f8f with its names prefixed (gfsk_ref_,
 * GFSK_REF_), frozen but for changes to the algorithm, each priced with tools/gfsk_sim.c and made
 * here and in gfsk_demod alike (2026-11-12: the squelch's gate kept over a reset; a step up
 * inside a run of the gate starts the run over, `restep`; the channel at DC, the FIR the sum of 4
 * without the derotation by 1 MHz). The faster gfsk_demod must produce the
 * same symbols, bit for bit, from the same input cut into the same pushes; the bench (ubt rx
 * bench) and the unit tests compare them. Tuning experiments (tools/gfsk_sim.c,
 * GFSK_REF_TUNABLE) run here. Do not optimise it.
 *
 * 1 Msym/s GFSK demodulator (Bluetooth BR and LE) for the 4 MS/s planar int16 stream of the
 * ring receiver (ESP_SDR_RING_DECIM_BOX4 with shift_fs4: the sums of 4; full scale 8192 per
 * axis). The wanted channel sits at DC in that stream, the LO 4 MHz below it (until 2026-11-12
 * at +1 MHz: the aliases of the bands 3 and 5 MHz off then missed the sums' nulls, 1 dB). If the
 * hardware turns out to mirror the spectrum (the deviation inverted), pass the q array as @p i
 * and the i array as @p q: that conjugates the signal at no cost.
 *
 * Signal path, per sample: the sum of 4 samples (run as z[n] = z[n-1] + x[n] - x[n-4]) puts
 * nulls on every multiple of 1 MHz from the channel: the neighbours at +-1 MHz (BR) or +-2 MHz
 * (LE) fall into them; the ring's own sums cancel the LO's leakage. A delay-2 discriminator
 * follows, -Im(z z[n-2]*) (negative for a positive deviation); its output is summed over
 * 2 samples around the symbol centre and sliced against the middle of the two levels it takes
 * (so a carrier offset is absorbed). tools/gfsk_sim.c holds the numbers behind these choices.
 *
 * Symbol timing: when the signal power rises (a burst), the sampling phase is picked from the
 * four candidates by the opening of the eye over the alternating preamble; afterwards an
 * early/late comparison moves it by one sample when the eye is better on one side (it follows
 * 100 ppm over a 3 ms packet). The decisions run GFSK_REF_LAT_SYMS symbols behind the newest
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
 * GFSK_REF_PRE_SAMPLES samples are replayed through the full path so that no preamble is lost.
 * "Always emit" keeps the full path running and produces a symbol per symbol period; the
 * threshold then only decides when the sampling phase and slicing levels are acquired anew (a
 * new burst).
 */

#ifndef GFSK_REF_H_
#define GFSK_REF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfsk_sym.h"

/* Decisions trail the newest sample by this many symbols (look-ahead of the estimates). */
#define GFSK_REF_LAT_SYMS    12U
/* Samples replayed when the squelch opens (multiple of 4); history kept for it. */
#define GFSK_REF_PRE_SAMPLES 40U
#define GFSK_REF_HIST        64U
/* Rings: samples (power of two >= 4 * (LAT + chunk) + 12) and group powers (>= LAT + chunk + 4). */
#define GFSK_REF_CHUNK       8U
#define GFSK_REF_RING        128U
#define GFSK_REF_PRING       32U

/*
 * Symbols one gfsk_ref_push() of n samples can emit at most: one per 4 samples, plus the
 * pipeline (look-ahead) flushed when a squelch closes or after a gap, plus the pre-roll replayed
 * when the squelch opens. The caller's out_cap must be at least this; gfsk_ref_flush() needs
 * GFSK_REF_LAT_SYMS + GFSK_REF_CHUNK + 2. Symbols beyond out_cap are dropped.
 */
#define GFSK_REF_OUT_CAP(n) \
	((size_t)(n) / 4U + GFSK_REF_LAT_SYMS + GFSK_REF_CHUNK + GFSK_REF_PRE_SAMPLES / 4U + 4U)

struct gfsk_ref {
	/* Configuration, kept by gfsk_ref_reset(). */
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
	uint16_t hist_n; /* valid raw samples in hi/hq (the ones before n) */
	uint8_t open;    /* the full path runs */
	uint8_t flushing;/* a gap happened: emit what is pending first */
	uint32_t flush_f; /* ... from the samples before it: f where the gap began */
	uint8_t front_ok;

	/* FIR and discriminator: z[f - 1], and z >> 2 at f - 1 and f - 2 (by parity of the index). */
	int32_t zi, zq, a1i, a1q, a2i, a2q;
#ifdef GFSK_REF_TUNABLE
	int32_t ah_i[4], ah_q[4];
#endif
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
	uint32_t cring[GFSK_REF_RING];
	uint32_t pring[GFSK_REF_PRING]; /* power of each group, raw */
	uint8_t wring[GFSK_REF_PRING];  /* the smoothed power of each group, pwr units */
	int16_t hi[GFSK_REF_HIST], hq[GFSK_REF_HIST];
};

/** Set up for @p mod; automatic lock threshold, always emitting (see gfsk_ref_set_squelch). */
void gfsk_ref_init(struct gfsk_ref *d, enum gfsk_modulation mod);

/** Forget the stream: the sample counter restarts at 0, the configuration is kept. */
void gfsk_ref_reset(struct gfsk_ref *d);

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
void gfsk_ref_set_squelch(struct gfsk_ref *d, uint8_t pwr_threshold, uint16_t hold_syms,
			    bool always_emit);

/**
 * @brief Run n samples (planar int16, full scale 8192) through the demodulator.
 *
 * Symbols appear GFSK_REF_LAT_SYMS symbols late; gfsk_sym.t counts samples from the last reset
 * across all calls and stamps the sample at which the symbol was on the air.
 *
 * @param out_cap At least GFSK_REF_OUT_CAP(n); symbols beyond it are dropped.
 * @return Symbols written.
 */
size_t gfsk_ref_push(struct gfsk_ref *d, const int16_t *i, const int16_t *q, size_t n,
		       struct gfsk_sym *out, size_t out_cap);

/** Emit the symbols still in the look-ahead (end of a stream, mode change). */
size_t gfsk_ref_flush(struct gfsk_ref *d, struct gfsk_sym *out, size_t out_cap);

/**
 * The ring dropped @p n_lost samples: the counter jumps over them so t stays right, the pending
 * symbols are emitted by the next push, and the filter and the estimates restart.
 */
void gfsk_ref_gap(struct gfsk_ref *d, uint32_t n_lost);

/** Smoothed in-channel power now, pwr units (noise floor when idle). */
uint8_t gfsk_ref_level(const struct gfsk_ref *d);

/** Tracked noise floor (the automatic lock threshold minus 8 dB), pwr units. */
uint8_t gfsk_ref_noise_floor(const struct gfsk_ref *d);

/**
 * Carrier offset of the last burst in Hz (positive: the signal is above its nominal frequency),
 * from the middle of the two levels against their distance; rough (+-25 %), for diagnostics
 * and an offset correction of the LO. 0 before the first burst.
 */
int32_t gfsk_ref_cfo_hz(const struct gfsk_ref *d);

/** Samples consumed so far (the index of the next one). */
static inline uint32_t gfsk_ref_now(const struct gfsk_ref *d)
{
	return d->n;
}

/**
 * Index (in the units of gfsk_sym.t) before which no further symbol will be emitted: all the
 * symbols with t below it have been emitted or never will be. For filling the idle time between
 * bursts when the squelch is closed.
 */
uint32_t gfsk_ref_horizon(const struct gfsk_ref *d);

/** Power measure S (8 A^2 for a tone of amplitude A) to pwr units, and back. */
uint8_t gfsk_ref_pwr_from_s(uint32_t s);
uint32_t gfsk_ref_s_from_pwr(uint8_t pwr);


#ifdef GFSK_REF_TUNABLE
/* Parameters swept by tools/gfsk_sim.c; the defaults are the shipped ones. */
struct gfsk_ref_tune {
	int disc;       /* 1 to 4: discriminator delay in samples */
	int win;        /* samples of the discriminator summed per symbol (1 to 4) */
	int dd_k;       /* time constant of the per level means, log2 symbols */
	int nseed;      /* symbols the acquisition looks at */
	int seed_skip;  /* ... starting this many samples after the burst start estimate */
	int el_period;  /* early/late samples (every other symbol) per decision (0: phase fixed) */
	int el_stride;  /* early/late on every el_stride-th symbol (1, 2 or 4) */
	int el_shift;   /* a step needs |E| > A >> el_shift */
	int sq_shift;   /* power smoothing */
	int toff;       /* t = window end - toff */
	int floor_up;   /* rise shift per group of the noise floor tracker */
	int restep;     /* a group 6 dB over the smoothed power starts the gate's run over */
};
extern struct gfsk_ref_tune gfsk_ref_tune;
/* Calls of the acquisition so far (a measure of its cost for the simulator). */
extern unsigned long gfsk_ref_acquisitions;
#endif

#endif /* GFSK_REF_H_ */
