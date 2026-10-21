/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * channelize() of the esdr module on the ESP32-P4: a triggered snapshot (esdr.trig_get(), 80
 * MS/s of int8 I, Q) as a bank of FFT channels, their power over the snapshot and over time.
 * Some 8 ms per snapshot of 16320 samples (nfft 64) on the 360 MHz core, its buffers in PSRAM
 * as in SRAM (the SRAM is LVGL's draw buffers').
 */

/* The per-sample loops: -Os leaves them a third slower. */
#pragma GCC optimize("O3")

#include <math.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_board.h"
#include "esdr_defs.h"
#include "esdr_gui.h"

#define NFFT_MAX 1024U

/* Per channel: the sum and the largest over the snapshot, the largest over the image row. */
static ESDR_PSRAM float sum[NFFT_MAX], big[NFFT_MAX], rowbig[NFFT_MAX];

static void image_row(uint8_t *px, uint32_t n, float lo, float span)
{
	for (uint32_t m = 0; m < n; m++) {
		float v = (10.0f * log10f(rowbig[m] + 1e-20f) - lo) * 255.0f / span;

		px[m] = v <= 0.0f ? 0U : v >= 255.0f ? 255U : (uint8_t)v;
		rowbig[m] = 0.0f;
	}
}

/*
 * channelize(snap, nfft, mean=None, peak=None, image=None, lo=-100, hi=-20) -> frames: snap
 * (int8 I, Q, full scale 128), its mean taken off, cut into frames of nfft samples (a power of
 * two, 16 to 1024), each Hann windowed (psd()'s window) and transformed: nfft channels, DC
 * centred. mean and peak (float32, nfft entries) get each channel's mean and largest power over
 * the frames in dB relative to a full-scale tone; image (bytes, rows of nfft) one row per group
 * of frames, the largest power, lo..hi dB as 0..255. Returns the number of frames.
 */
static mp_obj_t esdr_channelize(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
	enum { ARG_snap, ARG_nfft, ARG_mean, ARG_peak, ARG_image, ARG_lo, ARG_hi };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_snap, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_nfft, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_mean, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_peak, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_image, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_lo, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_hi, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	size_t len, mlen = 0, plen = 0, ilen = 0;
	float *mean = NULL, *peak = NULL;
	uint8_t *image = NULL;

	mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);

	const int8_t *src = esdr_buffer(a[ARG_snap].u_obj, &len, MP_BUFFER_READ, 1);
	uint32_t n = (uint32_t)a[ARG_nfft].u_int;
	uint32_t samples = (uint32_t)(len / 2U);
	uint32_t frames = samples / (n != 0U ? n : 1U), rows = 0;
	/* Any number (a GUI slider gives floats). */
	float lo = -100.0f, hi = -20.0f, span;

	if (a[ARG_lo].u_obj != MP_OBJ_NULL) {
		lo = (float)mp_obj_get_float(a[ARG_lo].u_obj);
	}
	if (a[ARG_hi].u_obj != MP_OBJ_NULL) {
		hi = (float)mp_obj_get_float(a[ARG_hi].u_obj);
	}
	span = hi - lo;

	if (n < 16U || n > NFFT_MAX || (n & (n - 1U)) != 0U || frames == 0U || !(span > 0.0f)) {
		mp_raise_ValueError(MP_ERROR_TEXT("channelize: nfft 16..1024, a power of two"));
	}
	if (a[ARG_mean].u_obj != mp_const_none) {
		mean = esdr_buffer(a[ARG_mean].u_obj, &mlen, MP_BUFFER_WRITE, 4);
	}
	if (a[ARG_peak].u_obj != mp_const_none) {
		peak = esdr_buffer(a[ARG_peak].u_obj, &plen, MP_BUFFER_WRITE, 4);
	}
	if (a[ARG_image].u_obj != mp_const_none) {
		image = esdr_buffer(a[ARG_image].u_obj, &ilen, MP_BUFFER_WRITE, 1);
		rows = (uint32_t)MIN(ilen / n, frames);
		if (rows == 0U) {
			image = NULL;
		}
	}
	if ((mean != NULL && mlen / 4U < n) || (peak != NULL && plen / 4U < n)) {
		mp_raise_ValueError(MP_ERROR_TEXT("channelize: mean, peak of nfft floats"));
	}
	/* The FFT's buffers and tables are esdr_gui.c's, as psd() uses them. */
	struct esdr_fft *ft = esdr_fft_tables(n, ESDR_FFT_HANN);

	/* The receiver's DC (the raw samples keep it): the snapshot's mean. */
	int32_t si = 0, sq = 0;

	for (uint32_t k = 0; k < frames * n; k++) {
		si += src[2U * k];
		sq += src[2U * k + 1U];
	}

	float di = (float)si / (float)(frames * n), dq = (float)sq / (float)(frames * n);
	float norm = 1.0f / (ft->win_sum * ft->win_sum * 128.0f * 128.0f);
	uint32_t row = 0;

	memset(sum, 0, n * sizeof(float));
	memset(big, 0, n * sizeof(float));
	memset(rowbig, 0, n * sizeof(float));
	for (uint32_t f = 0; f < frames; f++) {
		const int8_t *p = src + 2U * f * n;

		for (uint32_t k = 0; k < n; k++) {
			ft->re[k] = ((float)p[2U * k] - di) * ft->win[k];
			ft->im[k] = ((float)p[2U * k + 1U] - dq) * ft->win[k];
		}
		esp_sdr_fft(ft->re, ft->im, n, ft->tw_cos, ft->tw_sin, n);
		for (uint32_t k = 0; k < n; k++) {
			uint32_t m = (k + n / 2U) & (n - 1U);
			float pw = (ft->re[k] * ft->re[k] + ft->im[k] * ft->im[k]) * norm;

			sum[m] += pw;
			big[m] = MAX(big[m], pw);
			rowbig[m] = MAX(rowbig[m], pw);
		}
		/* Frames spread evenly over the rows: row r takes frames r * frames / rows on. */
		if (image != NULL && (f + 1U) * rows >= (row + 1U) * frames) {
			image_row(image + (size_t)row * n, n, lo, span);
			row++;
		}
	}
	for (uint32_t m = 0; m < n; m++) {
		if (mean != NULL) {
			mean[m] = 10.0f * log10f(sum[m] / (float)frames + 1e-20f);
		}
		if (peak != NULL) {
			peak[m] = 10.0f * log10f(big[m] + 1e-20f);
		}
	}
	return mp_obj_new_int_from_uint(frames);
}
MP_DEFINE_CONST_FUN_OBJ_KW(esdr_channelize_obj, 2, esdr_channelize);

/*
 * fb_image(fb, stride, x, y, w, h, image, cols, palette): image (bytes, rows of cols, 0..255,
 * channelize()'s) stretched over the rectangle of an RGB565 framebuffer through the palette.
 */
static mp_obj_t esdr_fb_image(size_t n_args, const mp_obj_t *args)
{
	size_t flen, ilen, plen;
	uint16_t *px = esdr_buffer(args[0], &flen, MP_BUFFER_WRITE, 2);
	int stride = mp_obj_get_int(args[1]), x = mp_obj_get_int(args[2]);
	int y = mp_obj_get_int(args[3]), w = mp_obj_get_int(args[4]), h = mp_obj_get_int(args[5]);
	const uint8_t *img = esdr_buffer(args[6], &ilen, MP_BUFFER_READ, 1);
	int cols = mp_obj_get_int(args[7]);
	const uint16_t *pal = esdr_buffer(args[8], &plen, MP_BUFFER_READ, 2);
	int rows = cols > 0 ? (int)(ilen / (size_t)cols) : 0;

	ARG_UNUSED(n_args);
	plen /= 2U;
	if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > stride ||
	    (size_t)(y + h) * (size_t)stride * 2U > flen || rows == 0 || plen == 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("fb_image: rectangle, image, palette"));
	}
	for (int r = 0; r < h; r++) {
		const uint8_t *src = img + (size_t)(r * rows / h) * (size_t)cols;
		uint16_t *dst = px + (size_t)(y + r) * (size_t)stride + x;

		for (int c = 0; c < w; c++) {
			dst[c] = pal[src[c * cols / w] * plen / 256U];
		}
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fb_image_obj, 9, 9, esdr_fb_image);

/* stretch(src, dst): src's floats spread over dst's (one per screen column), each the largest
 * of the entries it covers. */
static mp_obj_t esdr_stretch(mp_obj_t src_in, mp_obj_t dst_in)
{
	size_t slen, dlen;
	const float *src = esdr_buffer(src_in, &slen, MP_BUFFER_READ, 4);
	float *dst = esdr_buffer(dst_in, &dlen, MP_BUFFER_WRITE, 4);
	size_t n = slen / 4U, w = dlen / 4U;

	if (n == 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("stretch: empty source"));
	}
	for (size_t c = 0; c < w; c++) {
		size_t a = c * n / w, b = MAX((c + 1U) * n / w, a + 1U);
		float v = src[a];

		for (size_t k = a + 1U; k < b; k++) {
			v = MAX(v, src[k]);
		}
		dst[c] = v;
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(esdr_stretch_obj, esdr_stretch);
