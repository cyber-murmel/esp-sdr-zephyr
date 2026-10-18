/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drawing for the qtgui stand-ins (modules/gnuradio/qtgui.py) on the board's display:
 * spectra (window, FFT, averaging, one value per screen column), I/Q columns, traces,
 * waterfall rows and scrolling in an RGB565 framebuffer (a bytearray whose colours the caller
 * stores in LVGL's native order, for an lv.canvas). The work buffers are PSRAM.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <esp_attr.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_rx.h"

#define FFT_MAX 4096U
#define FULL_SCALE 16384.0f

static EXT_RAM_BSS_ATTR float fft_re[FFT_MAX];
static EXT_RAM_BSS_ATTR float fft_im[FFT_MAX];
static EXT_RAM_BSS_ATTR float tw_cos[FFT_MAX / 2];
static EXT_RAM_BSS_ATTR float tw_sin[FFT_MAX / 2];
static EXT_RAM_BSS_ATTR float win[FFT_MAX];
static uint32_t tw_n, win_n;
static int win_kind = -1;
static float win_sum;

/* gnuradio.fft.window kinds: 0 Hamming, 1 Hann, 2 Blackman, 3 rectangular, others Blackman-Harris. */
static void tables(uint32_t n, int kind)
{
	if (tw_n != n) {
		esp_sdr_fft_twiddles(tw_cos, tw_sin, n);
		tw_n = n;
	}
	if (win_n != n || win_kind != kind) {
		enum esp_sdr_window shape = kind >= 0 && kind <= ESP_SDR_WINDOW_RECT
						    ? (enum esp_sdr_window)kind
						    : ESP_SDR_WINDOW_BLACKMAN_HARRIS;

		win_sum = 0.0f;
		for (uint32_t k = 0; k < n; k++) {
			win[k] = esp_sdr_window(shape, k, n - 1U);
			win_sum += win[k];
		}
		win_n = n;
		win_kind = kind;
	}
}

static void *buffer(mp_obj_t obj, size_t *len, mp_uint_t flags, size_t align)
{
	mp_buffer_info_t bi;

	mp_get_buffer_raise(obj, &bi, flags);
	if (((uintptr_t)bi.buf & (align - 1U)) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("unaligned buffer"));
	}
	*len = bi.len;
	return bi.buf;
}

/* rx_latest(buf) -> int: the latest samples of a monitored capture into buf (int16 I, Q). */
static mp_obj_t esdr_rx_latest_fn(mp_obj_t buf_in)
{
	size_t len;
	int16_t *buf = buffer(buf_in, &len, MP_BUFFER_WRITE, 4);

	return mp_obj_new_int_from_ull(esdr_rx_latest(buf, len / 4U));
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_rx_latest_obj, esdr_rx_latest_fn);

/*
 * psd(src, out, state, alpha, window): power spectrum of src (int16 I, Q; a power of two of
 * samples up to 4096) in dB relative to a full-scale tone, DC centred, one value per entry of
 * out (float32; the strongest bin each covers). state (float32, at least as many entries as
 * samples) holds the linear average, alpha its weight of the new spectrum (1: none).
 */
static mp_obj_t esdr_psd(size_t n_args, const mp_obj_t *args)
{
	size_t len, cols, slen;
	const int16_t *src = buffer(args[0], &len, MP_BUFFER_READ, 2);
	float *out = buffer(args[1], &cols, MP_BUFFER_WRITE, 4);
	float *state = buffer(args[2], &slen, MP_BUFFER_WRITE, 4);
	float alpha = (float)mp_obj_get_float(args[3]);
	int kind = (int)mp_obj_get_int(args[4]);
	uint32_t n = (uint32_t)(len / 4U);

	ARG_UNUSED(n_args);
	cols /= 4U;
	if (n < 16U || n > FFT_MAX || (n & (n - 1U)) != 0U || slen / 4U < n || cols == 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("psd: 16..4096 samples, a power of two"));
	}
	if (!(alpha > 0.0f) || alpha > 1.0f) {
		alpha = 1.0f;
	}
	tables(n, kind);
	for (uint32_t k = 0; k < n; k++) {
		fft_re[k] = (float)src[2U * k] * win[k] / FULL_SCALE;
		fft_im[k] = (float)src[2U * k + 1U] * win[k] / FULL_SCALE;
	}
	esp_sdr_fft(fft_re, fft_im, n, tw_cos, tw_sin, n);

	float norm = 1.0f / (win_sum * win_sum);

	for (uint32_t k = 0; k < n; k++) {
		uint32_t m = (k + n / 2U) & (n - 1U);
		float p = (fft_re[k] * fft_re[k] + fft_im[k] * fft_im[k]) * norm;

		state[m] = alpha >= 1.0f ? p : alpha * p + (1.0f - alpha) * state[m];
	}
	for (size_t c = 0; c < cols; c++) {
		uint32_t b0 = (uint32_t)(c * n / cols);
		uint32_t b1 = (uint32_t)((c + 1U) * n / cols);
		float v = 0.0f;

		if (b1 <= b0) {
			b1 = b0 + 1U;
		}
		for (uint32_t b = b0; b < b1; b++) {
			v = MAX(v, state[b]);
		}
		out[c] = 10.0f * log10f(v + 1e-20f);
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_psd_obj, 5, 5, esdr_psd);

/* iq_cols(src, out_i, out_q): src (int16 I, Q) spread over the entries of out_i, out_q (±1). */
static mp_obj_t esdr_iq_cols(mp_obj_t src_in, mp_obj_t i_in, mp_obj_t q_in)
{
	size_t len, cols, qlen;
	const int16_t *src = buffer(src_in, &len, MP_BUFFER_READ, 2);
	float *oi = buffer(i_in, &cols, MP_BUFFER_WRITE, 4);
	float *oq = buffer(q_in, &qlen, MP_BUFFER_WRITE, 4);
	size_t n = len / 4U;

	cols = MIN(cols, qlen) / 4U;
	for (size_t c = 0; c < cols && n > 0U; c++) {
		size_t k = c * n / cols;

		oi[c] = (float)src[2U * k] / FULL_SCALE;
		oq[c] = (float)src[2U * k + 1U] / FULL_SCALE;
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(esdr_iq_cols_obj, esdr_iq_cols);

/* The framebuffer and a rectangle in it, checked against its size. */
struct fb_rect {
	uint16_t *px;
	int stride, x, y, w, h;
};

static void fb_rect(struct fb_rect *r, const mp_obj_t *args)
{
	size_t len;

	r->px = buffer(args[0], &len, MP_BUFFER_WRITE, 2);
	r->stride = (int)mp_obj_get_int(args[1]);
	r->x = (int)mp_obj_get_int(args[2]);
	r->y = (int)mp_obj_get_int(args[3]);
	r->w = (int)mp_obj_get_int(args[4]);
	r->h = (int)mp_obj_get_int(args[5]);
	if (r->stride <= 0 || r->x < 0 || r->y < 0 || r->w <= 0 || r->h <= 0 ||
	    r->x + r->w > r->stride ||
	    (size_t)(r->y + r->h) * (size_t)r->stride * 2U > len) {
		mp_raise_ValueError(MP_ERROR_TEXT("rectangle outside the framebuffer"));
	}
}

static int scale(float v, float vmin, float vmax, int steps)
{
	float f = (v - vmin) / (vmax - vmin);

	if (!(f > 0.0f)) {
		return 0;
	}
	if (f >= 1.0f) {
		return steps - 1;
	}
	return (int)(f * (float)(steps - 1) + 0.5f);
}

/* fb_trace(fb, stride, x, y, w, h, values, vmin, vmax, colour): a trace across the rectangle. */
static mp_obj_t esdr_fb_trace(size_t n_args, const mp_obj_t *args)
{
	struct fb_rect r;
	size_t len;
	const float *v;
	float vmin = (float)mp_obj_get_float(args[7]);
	float vmax = (float)mp_obj_get_float(args[8]);
	uint16_t colour = (uint16_t)mp_obj_get_int(args[9]);
	int prev = -1;

	ARG_UNUSED(n_args);
	fb_rect(&r, args);
	v = buffer(args[6], &len, MP_BUFFER_READ, 4);
	if (len / 4U < (size_t)r.w || !(vmax > vmin)) {
		mp_raise_ValueError(MP_ERROR_TEXT("fb_trace: a value per column, vmax > vmin"));
	}
	for (int c = 0; c < r.w; c++) {
		int py = r.y + r.h - 1 - scale(v[c], vmin, vmax, r.h);
		int a = prev < 0 ? py : MIN(prev, py);
		int b = prev < 0 ? py : MAX(prev, py);

		/* Join the previous column's point vertically. */
		for (int row = a; row <= b; row++) {
			r.px[row * r.stride + r.x + c] = colour;
		}
		prev = py;
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fb_trace_obj, 10, 10, esdr_fb_trace);

/* fb_wf_row(fb, stride, x, y, w, 1, values, vmin, vmax, palette): one waterfall row. */
static mp_obj_t esdr_fb_wf_row(size_t n_args, const mp_obj_t *args)
{
	struct fb_rect r;
	size_t len, plen;
	const float *v;
	const uint16_t *pal;
	float vmin = (float)mp_obj_get_float(args[7]);
	float vmax = (float)mp_obj_get_float(args[8]);

	ARG_UNUSED(n_args);
	fb_rect(&r, args);
	v = buffer(args[6], &len, MP_BUFFER_READ, 4);
	pal = buffer(args[9], &plen, MP_BUFFER_READ, 2);
	plen /= 2U;
	if (len / 4U < (size_t)r.w || plen == 0U || !(vmax > vmin)) {
		mp_raise_ValueError(MP_ERROR_TEXT("fb_wf_row: a value per column, a palette"));
	}
	for (int c = 0; c < r.w; c++) {
		r.px[r.y * r.stride + r.x + c] = pal[scale(v[c], vmin, vmax, (int)plen)];
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fb_wf_row_obj, 10, 10, esdr_fb_wf_row);

/* fb_scroll(fb, stride, x, y, w, h): the rectangle's rows one down, the top row stays. */
static mp_obj_t esdr_fb_scroll(size_t n_args, const mp_obj_t *args)
{
	struct fb_rect r;

	ARG_UNUSED(n_args);
	fb_rect(&r, args);
	for (int row = r.y + r.h - 1; row > r.y; row--) {
		memmove(&r.px[row * r.stride + r.x], &r.px[(row - 1) * r.stride + r.x],
			(size_t)r.w * 2U);
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fb_scroll_obj, 6, 6, esdr_fb_scroll);

/*
 * fb_points(fb, stride, x, y, w, h, samples, xmin, xmax, ymin, ymax, colour): a constellation,
 * one dot per sample (int16 I, Q pairs, in sample units) inside the rectangle.
 */
static mp_obj_t esdr_fb_points(size_t n_args, const mp_obj_t *args)
{
	struct fb_rect r;
	size_t len;
	const int16_t *v;
	float xmin = (float)mp_obj_get_float(args[7]);
	float xmax = (float)mp_obj_get_float(args[8]);
	float ymin = (float)mp_obj_get_float(args[9]);
	float ymax = (float)mp_obj_get_float(args[10]);
	uint16_t colour = (uint16_t)mp_obj_get_int(args[11]);

	ARG_UNUSED(n_args);
	fb_rect(&r, args);
	v = buffer(args[6], &len, MP_BUFFER_READ, 2);
	if (!(xmax > xmin) || !(ymax > ymin)) {
		mp_raise_ValueError(MP_ERROR_TEXT("fb_points: xmax > xmin, ymax > ymin"));
	}
	float sx = (float)(r.w - 1) / (xmax - xmin);
	float sy = (float)(r.h - 1) / (ymax - ymin);

	for (size_t k = 0; k + 1U < len / 2U; k += 2U) {
		int px = (int)(((float)v[k] - xmin) * sx);
		int py = r.h - 1 - (int)(((float)v[k + 1U] - ymin) * sy);

		if (px >= 0 && px < r.w && py >= 0 && py < r.h) {
			r.px[(r.y + py) * r.stride + r.x + px] = colour;
		}
	}
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fb_points_obj, 12, 12, esdr_fb_points);
