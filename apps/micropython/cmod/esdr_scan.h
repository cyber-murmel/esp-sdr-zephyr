/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Scan app's measurement (eggs/scan): an 80 MS/s burst's spectrum, averaged over its frames,
 * folded into the panorama of the whole tuning range. Plain C (no MicroPython, no Zephyr), built
 * with the host compiler by tests/host/test_scan_egg.py, which holds it to numpy.
 */

#ifndef ESDR_SCAN_H_
#define ESDR_SCAN_H_

#include <stdint.h>

/* Points of a frame: at 80 MS/s bins 312.5 kHz apart, 16 of them 5 MHz. */
#define ESDR_SCAN_N 256U
/* A sample counts as clipped from here (10-bit I or Q << 5 tops out at 16352). */
#define ESDR_SCAN_CLIP 16320
/* What a panorama bin holds before anything was measured there (and below which it is empty). */
#define ESDR_SCAN_EMPTY (-200.0f)

struct esdr_scan {
	/*
	 * The twiddles, Q15, for k < N / 2: cos, sin of -tau k / N (the int32 FFT), or esp-dsp's
	 * table, cos, sin of tau r / N for r the bit reversal of k (the S3's 16-bit one: its vector
	 * loads want 16-byte alignment). Blackman-Harris, Q15.
	 */
	int16_t tw[ESDR_SCAN_N] __attribute__((aligned(16)));
	/* The window as the vector unit takes it, each value twice (I, Q). */
	int16_t win2[2U * ESDR_SCAN_N] __attribute__((aligned(16)));
	int16_t win[ESDR_SCAN_N];
	uint8_t rev[ESDR_SCAN_N];
	/* dB of a full-scale tone's bin (|x| = 16384 at a bin's centre, after the window). */
	float ref_db;
	float acc[ESDR_SCAN_N];
	float peak[ESDR_SCAN_N];
	/* The 16-bit FFT's rounding bias per output (I, Q), taken off each frame. */
	int8_t bias[2U * ESDR_SCAN_N];
};

void esdr_scan_init(struct esdr_scan *s);

/*
 * The S3's FFT of ESDR_SCAN_N points (int16 I, Q interleaved, 16-byte aligned) in place:
 * esp-dsp's dsps_fft2r_sc16_aes3_() with @p tw (esdr_scan_pie.c), or the same arithmetic in C
 * (ESDR_SCAN_FFT16_MODEL, the host tests). Natural order in, bit reversed out, each stage halved:
 * DFT / N.
 */
void esdr_scan_fft16(int16_t *data, const int16_t *tw);

/*
 * The S3's frame passes before it, as esdr_scan_vec.S does them on the vector unit
 * (esdr_scan_pie.c; *_c the same arithmetic in C, which the host tests and an unaligned frame
 * run). @p x a frame (ESDR_SCAN_N pairs). esdr_scan_minmax(): @p mm gets each of 8 lanes'
 * largest, then their smallest (I in the even lanes, Q in the odd). esdr_scan_window(): @p d gets
 * ((x - c) saturated to int16) times @p w2, shifted down @p sar (floor), with c's I in bits 15:0
 * and its Q in bits 31:16. Buffers 16-byte aligned for the vector unit.
 */
void esdr_scan_minmax(const int16_t *x, int16_t *mm);
void esdr_scan_window(const int16_t *x, int16_t *d, const int16_t *w2, uint32_t c, uint32_t sar);
void esdr_scan_minmax_c(const int16_t *x, int16_t *mm);
void esdr_scan_window_c(const int16_t *x, int16_t *d, const int16_t *w2, uint32_t c, uint32_t sar);

/*
 * The spectrum of @p n samples at @p iq (int16 I, Q): frames of ESDR_SCAN_N that do not overlap
 * (@p frames_max at most, 0: all), each with its own centre taken out (the LO's DC and its slow
 * wander: the mean, with the S3's 16-bit FFT the middle of the range in I and in Q, which differs
 * in the bins within 3 of DC only) and windowed. @p mean_db gets the power averaged over the
 * frames, @p peak_db (may be NULL) the strongest frame's, per bin, in dB of a full-scale tone,
 * the lowest frequency first (DC at ESDR_SCAN_N / 2). @p clipped (may be NULL) gets the samples
 * with I or Q at ESDR_SCAN_CLIP or beyond. Returns the frames used (0: fewer than ESDR_SCAN_N
 * samples).
 */
uint32_t esdr_scan_spectrum(struct esdr_scan *s, const int16_t *iq, uint32_t n,
			    uint32_t frames_max, float *mean_db, float *peak_db, uint32_t *clipped);

/*
 * Folds a hop's spectrum into the panorama (@p len bins each): @p segs holds @p nsegs triples
 * (first bin of the spectrum, first panorama bin, bins). @p corr_db (may be NULL) is added to the
 * hop's bins first (the analog filter's roll-off, per bin of the spectrum). Then per bin:
 * @p recent takes the larger of itself and the mean (a history row, emptied by the caller);
 * unless @p alpha is 0 (a clipped hop: recent only), @p maxh the larger of itself and the peak,
 * @p avg the mean averaged in power with weight @p alpha, @p floor_db the smaller of the mean and
 * itself plus @p rise (a floor that follows the quietest readings and rises slowly). An empty bin
 * (at or below ESDR_SCAN_EMPTY) takes the first value. @p peak_max gets the hop's strongest peak,
 * @p floor_mean its mean in dB (either may be NULL). Returns -1 if a segment does not fit, else 0.
 */
int esdr_scan_fold(const float *mean_db, const float *peak_db, const float *corr_db,
		   const uint16_t *segs, uint32_t nsegs, float *recent, float *maxh, float *avg,
		   float *floor_db, uint32_t len, float alpha, float rise, float *peak_max,
		   float *floor_mean);

/*
 * A row of @p n values for the history (and Save) in 0.5 dB steps: 0 for an empty bin or one
 * below -127.25 dB, else 255 + 2 dB rounded, within 1 to 255 (0 dB and above: 255).
 */
void esdr_scan_q8(const float *db, uint8_t *q, uint32_t n);

#endif /* ESDR_SCAN_H_ */
