/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esdr_gui.c's FFT work buffers and tables, which esdr_chan.c's channelize() borrows: both run
 * on the MicroPython thread only.
 */

#ifndef ESDR_GUI_H
#define ESDR_GUI_H

#include <stdint.h>

#define ESDR_FFT_MAX 4096U
/* gnuradio.fft.window kinds: 0 Hamming, 1 Hann, 2 Blackman, 3 rectangular, others
 * Blackman-Harris. */
#define ESDR_FFT_HANN 1

struct esdr_fft {
	float re[ESDR_FFT_MAX], im[ESDR_FFT_MAX];
	float tw_cos[ESDR_FFT_MAX / 2], tw_sin[ESDR_FFT_MAX / 2], win[ESDR_FFT_MAX];
	/* The window's sum. */
	float win_sum;
};

/*
 * The work buffers with the twiddles and the window @p kind of an @p n point transform (a power
 * of two up to ESDR_FFT_MAX), made again only when they change. The window spans n - 1, as
 * gnuradio's.
 */
struct esdr_fft *esdr_fft_tables(uint32_t n, int kind);

#endif /* ESDR_GUI_H */
