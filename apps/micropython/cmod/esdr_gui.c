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
#include <zephyr/kernel.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_place.h>
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#include <esp_sdr/esp_sdr.h>
#endif

#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_board.h"
#include "esdr_defs.h"
#include "esdr_gui.h"
#include "esdr_rx.h"
#include "esdr_scan.h"
#include "esdr_wf.h"

#define FULL_SCALE 16384.0f

static ESP_SDR_DATA(BULK, fft) struct esdr_fft fft;
static uint32_t tw_n, win_n;
static int win_kind = -1;

struct esdr_fft *esdr_fft_tables(uint32_t n, int kind)
{
	if (tw_n != n) {
		esp_sdr_fft_twiddles(fft.tw_cos, fft.tw_sin, n);
		tw_n = n;
	}
	if (win_n != n || win_kind != kind) {
		enum esp_sdr_window shape = kind >= 0 && kind <= ESP_SDR_WINDOW_RECT
						    ? (enum esp_sdr_window)kind
						    : ESP_SDR_WINDOW_BLACKMAN_HARRIS;

		fft.win_sum = 0.0f;
		for (uint32_t k = 0; k < n; k++) {
			fft.win[k] = esp_sdr_window(shape, k, n - 1U);
			fft.win_sum += fft.win[k];
		}
		win_n = n;
		win_kind = kind;
	}
	return &fft;
}

/* rx_latest(buf) -> int: the latest samples of a monitored capture into buf (int16 I, Q). */
static mp_obj_t esdr_rx_latest_fn(mp_obj_t buf_in)
{
	size_t len;
	int16_t *buf = esdr_buffer(buf_in, &len, MP_BUFFER_WRITE, 4);

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
	const int16_t *src = esdr_buffer(args[0], &len, MP_BUFFER_READ, 2);
	float *out = esdr_buffer(args[1], &cols, MP_BUFFER_WRITE, 4);
	float *state = esdr_buffer(args[2], &slen, MP_BUFFER_WRITE, 4);
	float alpha = (float)mp_obj_get_float(args[3]);
	int kind = (int)mp_obj_get_int(args[4]);
	uint32_t n = (uint32_t)(len / 4U);

	ARG_UNUSED(n_args);
	cols /= 4U;
	if (n < 16U || n > ESDR_FFT_MAX || (n & (n - 1U)) != 0U || slen / 4U < n || cols == 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("psd: 16..4096 samples, a power of two"));
	}
	if (!(alpha > 0.0f) || alpha > 1.0f) {
		alpha = 1.0f;
	}
	struct esdr_fft *f = esdr_fft_tables(n, kind);

	for (uint32_t k = 0; k < n; k++) {
		f->re[k] = (float)src[2U * k] * f->win[k] / FULL_SCALE;
		f->im[k] = (float)src[2U * k + 1U] * f->win[k] / FULL_SCALE;
	}
	esp_sdr_fft(f->re, f->im, n, f->tw_cos, f->tw_sin, n);

	float norm = 1.0f / (f->win_sum * f->win_sum);

	for (uint32_t k = 0; k < n; k++) {
		uint32_t m = (k + n / 2U) & (n - 1U);
		float p = (f->re[k] * f->re[k] + f->im[k] * f->im[k]) * norm;

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

/*
 * scan_hop(src, segs, recent, maxh, avg, floor, alpha, rise=1.0, corr=None, frames=0,
 *          clip_max=-1) -> (clipped, peak_max, floor_mean)
 * The Scan app's hop (esdr_scan.c): src's spectrum (int16 I, Q, 4-byte aligned; frames of 256 at
 * most, 0: all, Blackman-Harris, each frame's centre out), plus corr (float32[256] dB, the filter's
 * roll-off) where given, folded into the panorama's float32 arrays along segs (array('H') of
 * triples: first bin of the hop's 256, first panorama bin, bins): recent the larger of itself and
 * the mean, and unless alpha is 0 or the hop clipped (more than clip_max samples, -1: never):
 * maxh the strongest frame held, avg the mean averaged in power (weight alpha), floor the quietest
 * readings rising rise dB a hop at most. A bin at -200 or below is empty. Returns the samples with I or Q at 16320 or beyond, the hop's strongest peak and its
 * mean in dB.
 */
static ESP_SDR_DATA(BULK, scan) struct esdr_scan scan;
static ESP_SDR_DATA(BULK, scan_mean) float scan_mean[ESDR_SCAN_N], scan_peak[ESDR_SCAN_N];

/*
 * scan_start(src, frames=0) -> bool
 * scan_wait(src=None)
 * With two CPUs a hop's spectrum on the second (the worker): scan_start() hands it src, a capture
 * done, and returns at once; MicroPython tunes the next hop on the first meanwhile (and on the P4
 * captures it: on the S3 and the S31 the capture runs on the second CPU, ahead of the worker),
 * and scan_hop() of the same src and frames folds the result (waiting for it where it is not
 * ready). False where there is no second CPU: scan_hop() makes the spectrum then, as before.
 * scan_wait() waits until the worker is done with src (with any, without one): before a capture
 * goes into it again. One job at a time; a newer one waits for the last and drops its result.
 * A continuous capture on the second CPU (a ring, a monitor, a gapless run) holds the worker until
 * it stops: the waits let Ctrl-C through.
 */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_scan_src);

#if defined(CONFIG_SMP) && CONFIG_MP_MAX_NUM_CPUS > 1
#define SCAN_WORKER 1
/*
 * On the other CPU from the interpreter, at or below its priority (the S3's and S31's at 0, the
 * P4's at 10): this CPU's radio and engine threads first.
 */
#define SCAN_PRIO  K_PRIO_PREEMPT(10)
/* frame32()'s two int32 arrays (2 KB), the 16-bit kind's 1 KB frame. */
#define SCAN_STACK 3584

/*
 * The stack in PSRAM, which a flash write turns off with the cache: the other CPU waits in an
 * interrupt meanwhile, so the worker does not run. The kernel objects in internal RAM, standalone;
 * a multiapp extension's bss, these among it, is PSRAM, whose atomics the pinned Zephyr emulates
 * (doc/dev/code-placement.md), as for the Pcap engine's.
 */
static Z_KERNEL_STACK_DEFINE_IN(scan_stack, SCAN_STACK, ESP_SDR_DATA_F(STACK, scan_stack, NOINIT));
/* On the S3 above the capture bank: the standalone Communicator's DRAM is full. */
static ESP_SDR_DATA_F2(STATE, sk, NOINIT, ATOMIC) struct {
	struct k_thread thread;
	struct k_sem go, done;
} sk;

/* The job: written by MicroPython while the worker waits, got and clipped by the worker. */
static ESP_SDR_DATA(BULK, job) struct {
	const int16_t *src;
	uint32_t n, frames, got, clipped;
	/* MicroPython's: the thread made; a job handed over and not yet waited for; its result. */
	bool up, pending, ready;
} sw;

static void scan_worker_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&sk.go, K_FOREVER);
		sw.got = esdr_scan_spectrum(&scan, sw.src, sw.n, sw.frames, scan_mean, scan_peak,
					    &sw.clipped);
		k_sem_give(&sk.done);
	}
}

static void scan_worker_up(void)
{
	if (sw.up) {
		return;
	}
	k_sem_init(&sk.go, 0, 1);
	k_sem_init(&sk.done, 0, 1);
	k_thread_create(&sk.thread, scan_stack, K_KERNEL_STACK_SIZEOF(scan_stack), scan_worker_fn,
			NULL, NULL, NULL, SCAN_PRIO, K_FP_REGS, K_FOREVER);
	k_thread_name_set(&sk.thread, "esdr_scan");
#if defined(CONFIG_SCHED_CPU_MASK)
	/* Not runnable yet (K_FOREVER): the pin takes. */
	(void)k_thread_cpu_pin(&sk.thread, CONFIG_MP_MAX_NUM_CPUS - 1);
#endif
	k_thread_start(&sk.thread);
	sw.up = true;
}

/* The job handed over, done: its result ready. */
static void scan_sync(void)
{
	if (sw.pending) {
		(void)k_sem_take(&sk.done, K_FOREVER);
		sw.pending = false;
		sw.ready = true;
	}
}

/*
 * As scan_sync(), for Python: a continuous capture on the radio's CPU (a ring, a monitor, a
 * gapless run) holds the worker until it stops, so Ctrl-C and the scheduler run meanwhile. An
 * exception leaves the job pending: esdr_scan_deinit() waits for it after the capture stops.
 */
static void scan_sync_py(void)
{
	if (!sw.pending) {
		return;
	}
	while (k_sem_take(&sk.done, K_MSEC(10)) != 0) {
		mp_handle_pending(true);
	}
	sw.pending = false;
	sw.ready = true;
}
#endif /* SCAN_WORKER */

void esdr_scan_deinit(void)
{
#if defined(SCAN_WORKER)
	/*
	 * Before the heap is used again (and after the captures stopped, which may hold the
	 * worker): it may be reading a buffer there.
	 */
	scan_sync();
	sw.ready = false;
#endif
	MP_STATE_PORT(esdr_scan_src) = MP_OBJ_NULL;
}

static mp_obj_t esdr_scan_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	static const mp_arg_t allowed[] = {
		{MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_frames, MP_ARG_INT, {.u_int = 0}},
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	size_t slen;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
	const int16_t *src = esdr_buffer(a[0].u_obj, &slen, MP_BUFFER_READ, 4);

	if (a[1].u_int < 0 || slen / 4U < ESDR_SCAN_N) {
		mp_raise_ValueError(MP_ERROR_TEXT("scan_start: 256 samples at least"));
	}
#if defined(SCAN_WORKER)
	scan_sync_py();
	sw.ready = false;
	if (scan.ref_db == 0.0f) {
		esdr_scan_init(&scan);
	}
	scan_worker_up();
	sw.src = src;
	sw.n = (uint32_t)(slen / 4U);
	sw.frames = (uint32_t)a[1].u_int;
	MP_STATE_PORT(esdr_scan_src) = a[0].u_obj;
	sw.pending = true;
	k_sem_give(&sk.go);
	return mp_const_true;
#else
	ARG_UNUSED(src);
	return mp_const_false;
#endif
}
MP_DEFINE_CONST_FUN_OBJ_KW(esdr_scan_start_obj, 1, esdr_scan_start);

static mp_obj_t esdr_scan_wait(size_t n_args, const mp_obj_t *args)
{
#if defined(SCAN_WORKER)
	if (n_args == 0 || args[0] == mp_const_none || args[0] == MP_STATE_PORT(esdr_scan_src)) {
		scan_sync_py();
	}
#else
	ARG_UNUSED(n_args);
	ARG_UNUSED(args);
#endif
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_scan_wait_obj, 0, 1, esdr_scan_wait);

static float *scan_floats(mp_obj_t o, size_t *n)
{
	size_t len;
	float *v = esdr_buffer(o, &len, MP_BUFFER_WRITE, 4);

	*n = len / 4U;
	return v;
}

static mp_obj_t esdr_scan_hop(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum {
		ARG_src, ARG_segs, ARG_recent, ARG_maxh, ARG_avg, ARG_floor, ARG_alpha, ARG_rise,
		ARG_corr, ARG_frames, ARG_clip_max
	};
	static const mp_arg_t allowed[] = {
		{MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_segs, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_recent, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_maxh, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_avg, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_floor, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_alpha, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_rise, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_corr, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_frames, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_clip_max, MP_ARG_INT, {.u_int = -1}},
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	size_t slen, glen, n, nm, na, nf, nc = 0;
	uint32_t clipped = 0;
	float peak_max = ESDR_SCAN_EMPTY, floor_mean = ESDR_SCAN_EMPTY;
	const float *corr = NULL;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
	const int16_t *src = esdr_buffer(a[ARG_src].u_obj, &slen, MP_BUFFER_READ, 4);
	const uint16_t *segs = esdr_buffer(a[ARG_segs].u_obj, &glen, MP_BUFFER_READ, 2);
	float *recent = scan_floats(a[ARG_recent].u_obj, &n);
	float *maxh = scan_floats(a[ARG_maxh].u_obj, &nm);
	float *avg = scan_floats(a[ARG_avg].u_obj, &na);
	float *floor_db = scan_floats(a[ARG_floor].u_obj, &nf);
	float alpha = (float)mp_obj_get_float(a[ARG_alpha].u_obj);
	float rise = a[ARG_rise].u_obj != MP_OBJ_NULL ? (float)mp_obj_get_float(a[ARG_rise].u_obj)
						       : 1.0f;

	if (a[ARG_corr].u_obj != mp_const_none) {
		corr = scan_floats(a[ARG_corr].u_obj, &nc);
	}
	if (a[ARG_frames].u_int < 0 || nm != n || na != n || nf != n || glen % 6U != 0U ||
	    (corr != NULL && nc != ESDR_SCAN_N) || !(alpha >= 0.0f) || alpha > 1.0f) {
		mp_raise_ValueError(MP_ERROR_TEXT("scan_hop: buffers"));
	}
	uint32_t got = 0;

#if defined(SCAN_WORKER)
	/* The worker's result where it made this one (scan_start()), else made here. */
	scan_sync_py();
	if (sw.ready && MP_STATE_PORT(esdr_scan_src) == a[ARG_src].u_obj && sw.src == src &&
	    sw.n == (uint32_t)(slen / 4U) && sw.frames == (uint32_t)a[ARG_frames].u_int) {
		got = sw.got;
		clipped = sw.clipped;
	}
	sw.ready = false;
#endif
	MP_STATE_PORT(esdr_scan_src) = MP_OBJ_NULL;
	/* Made at the first hop (ref_db is some 75 or 81 dB then: never 0). No flag in internal
	 * RAM: the standalone Communicator has 40 bytes of it to spare. */
	if (scan.ref_db == 0.0f) {
		esdr_scan_init(&scan);
	}
	if (got == 0U) {
		const uint32_t frames = (uint32_t)a[ARG_frames].u_int;

		got = esdr_scan_spectrum(&scan, src, (uint32_t)(slen / 4U), frames, scan_mean,
					 scan_peak, &clipped);
	}
	if (got == 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("scan_hop: 256 samples at least"));
	}
	/* Clipping spreads over the whole hop: only the history row (recent) takes it. */
	if (a[ARG_clip_max].u_int >= 0 && clipped > (uint32_t)a[ARG_clip_max].u_int) {
		alpha = 0.0f;
	}
	if (esdr_scan_fold(scan_mean, scan_peak, corr, segs, (uint32_t)(glen / 6U), recent, maxh,
			   avg, floor_db, (uint32_t)n, alpha, rise, &peak_max, &floor_mean) != 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("scan_hop: a segment outside"));
	}
	mp_obj_t out[3] = {
		mp_obj_new_int_from_uint(clipped),
		mp_obj_new_float(peak_max),
		mp_obj_new_float(floor_mean),
	};
	return mp_obj_new_tuple(3, out);
}
MP_DEFINE_CONST_FUN_OBJ_KW(esdr_scan_hop_obj, 7, esdr_scan_hop);

/* scan_q8(src, dst): src's floats (dB) as bytes in 0.5 dB steps (esdr_scan_q8()) into dst. */
static mp_obj_t esdr_scan_q8_fn(mp_obj_t src_in, mp_obj_t dst_in)
{
	size_t n, dlen;
	const float *src = scan_floats(src_in, &n);
	uint8_t *dst = esdr_buffer(dst_in, &dlen, MP_BUFFER_WRITE, 1);

	if (dlen < n) {
		mp_raise_ValueError(MP_ERROR_TEXT("scan_q8: dst shorter than src"));
	}
	esdr_scan_q8(src, dst, (uint32_t)n);
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(esdr_scan_q8_obj, esdr_scan_q8_fn);

/* iq_cols(src, out_i, out_q): src (int16 I, Q) spread over the entries of out_i, out_q (±1). */
static mp_obj_t esdr_iq_cols(mp_obj_t src_in, mp_obj_t i_in, mp_obj_t q_in)
{
	size_t len, cols, qlen;
	const int16_t *src = esdr_buffer(src_in, &len, MP_BUFFER_READ, 2);
	float *oi = esdr_buffer(i_in, &cols, MP_BUFFER_WRITE, 4);
	float *oq = esdr_buffer(q_in, &qlen, MP_BUFFER_WRITE, 4);
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

	r->px = esdr_buffer(args[0], &len, MP_BUFFER_WRITE, 2);
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
	v = esdr_buffer(args[6], &len, MP_BUFFER_READ, 4);
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
	v = esdr_buffer(args[6], &len, MP_BUFFER_READ, 4);
	pal = esdr_buffer(args[9], &plen, MP_BUFFER_READ, 2);
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
 * fb_wf_compose(fb, stride, x, y, w, h, img, head, col0, marks, view, fill, lines, coarse, fine):
 * the waterfall's rows out of their ring, with the grid over them (esdr_wf.h): img holds h rows
 * of w pixels, col0 (int32) and marks (bytes) an entry per row, lines (uint16) the columns.
 */
static mp_obj_t esdr_fb_wf_compose(size_t n_args, const mp_obj_t *args)
{
	struct fb_rect r;
	struct esdr_wf_src s;
	size_t ilen, clen, mlen, llen;

	ARG_UNUSED(n_args);
	fb_rect(&r, args);
	s.img = esdr_buffer(args[6], &ilen, MP_BUFFER_READ, 2);
	s.head = (int)mp_obj_get_int(args[7]);
	s.col0 = esdr_buffer(args[8], &clen, MP_BUFFER_READ, 4);
	s.marks = esdr_buffer(args[9], &mlen, MP_BUFFER_READ, 1);
	s.view = (int32_t)mp_obj_get_int(args[10]);
	s.fill = (uint16_t)mp_obj_get_int(args[11]);
	s.lines = esdr_buffer(args[12], &llen, MP_BUFFER_READ, 2);
	s.coarse = (uint16_t)mp_obj_get_int(args[13]);
	s.fine = (uint16_t)mp_obj_get_int(args[14]);
	s.w = r.w;
	s.rows = r.h;
	s.nlines = (int)(llen / 2U);
	if (ilen < (size_t)r.w * (size_t)r.h * 2U || clen / 4U < (size_t)r.h ||
	    mlen < (size_t)r.h || s.head < 0 || s.head >= r.h) {
		mp_raise_ValueError(MP_ERROR_TEXT("fb_wf_compose: a ring of h rows of w, head in it"));
	}
	esdr_wf_compose(&r.px[r.y * r.stride + r.x], r.stride, &s);
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fb_wf_compose_obj, 15, 15, esdr_fb_wf_compose);

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
	v = esdr_buffer(args[6], &len, MP_BUFFER_READ, 2);
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
