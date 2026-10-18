/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module on the ESP32-C5 (no PSRAM, no display), as modesdr.c is on the S3, for the
 * gnuradio stand-ins' packet flowgraphs: receive through the diagnostic-bus ring
 * (esp_sdr_diag.h, box16 and CIC to 2.5 MS/s / decim), transmit through the CPU-paced TX DC
 * path (esp_sdr_paced.h). Half duplex.
 *
 *   init()                     radio up (Wi-Fi must have started)
 *   tune(hz, offset=True)      LO at hz (the diagnostic bus has no mixer: offset is ignored);
 *                              returns the centre tuned
 *   rx_gain([index]), rx_gain_max(), rx_bandwidth(hz), channel_bw(mode)
 *   rx_lsb([n])                the bus's 4-bit window, I[n+3:n]: signals above 8 << n counts
 *                              of 512 wrap (default RX_LSB)
 *   RATES, BURST_RATES (none), FULL_SCALE, TX_PREBUF_S (0: bursts need no padding),
 *   RX_FSK_SLOW (2: the engine's receiver runs at half the rate, rx_start(slow=))
 *   rx_start(rate, buf=None, count=0, offset=True, monitor=False, wrap=False, fsk=None)
 *                              capture into buf (int16 I, Q; wrap: a ring) or, with fsk (an
 *                              fsk_rx_init() state), run that receiver on the samples in the
 *                              engine: fsk_frames() collects its frames. slow=2 runs the
 *                              receiver (made for half the samples per symbol) at half the
 *                              rate: the ring and a receiver at 250 kS/s do not fit the CPU
 *                              (RX_FSK_SLOW); frame positions and rx_count() stay in samples
 *                              at the rate asked for
 *   rx_stop(), rx_busy(), rx_wait([ms]), rx_count(), rx_stats(), fsk_frames()
 *   tx_start(rate, hz, gain, buf, count=0, scale=1.0) -> centre
 *                              play buf (int16 I, Q) once, count samples of it (0: all): returns
 *                              when it has played, interrupts locked meanwhile. The C5's level
 *                              follows the samples only: gain steps 0 to 17 scale them, 1 dB a
 *                              step, 17 full scale
 *   tx_stop(), tx_busy(), tx_wait([ms]), tx_stats(), tx_gain_max()
 *   fsk_rx_init(), fsk_rx(), fsk_rx_reset(), fsk_rx_state(), fsk_mod()   (esdr_link.c)
 *   first_start()              true once per hardware start (soft resets keep it false)
 *   next_app([path]), home()   the launcher (modules/launcher.py); no knob, home() is false
 *   reset_reason()
 *
 * Memory: the Python heap gets the upper half of SRAM block 1, which the dump engine owns only
 * while it captures (this module never does) and which the diagnostic ring's lower half leaves
 * free; its first HEAP_GUARD bytes stay out, as the dump engine writes there between being armed
 * and handing the block back (measured: 352 bytes). It also gets most of the libc heap, taken
 * once with malloc() and kept across soft resets: the Wi-Fi driver has the kernel heap.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>

#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag.h>
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "py/gc.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_fsk.h"

#define BYTE_RATE    40000000U
#define DECIM_RATE   (BYTE_RATE / 16U)
#define FULL_SCALE   8192
#define RX_LSB       4U
#define RX_PRIO      K_PRIO_COOP(2)
#define RX_STACK     2560
#define FRAMES       4U
#define TX_INTERP    4U
/* Input samples played after the last one, for the interpolation filter to empty. */
#define TX_TAIL      16U
#define TX_GAIN_MAX  17
#define HEAP_GUARD   8192U
/* What the libc heap keeps for others. */
#define LIBC_KEEP    4096U
/* DC removal: a running mean over about 2^DC_SHIFT samples (4 ms at 250 kS/s). */
#define DC_SHIFT     10

/* The bus pads: the header's GPIO 3 to 7 and the test points 23 to 25; the bench's RF switch
 * has GPIO 0, 1, 2 and 8, 9, 10, which the library's default set would toggle at 40 MHz. */
static const int8_t diag_pins[ESP_SDR_DIAG_PINS] = {3, 4, 5, 6, 7, 23, 24, 25};

extern uint8_t __esp_sdr_bank_start[], __esp_sdr_bank_end[];

MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_rx_buf);
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_rx_fsk);

/* ---- receive: the engine thread runs the diagnostic ring ---- */

static struct {
	int16_t *buf;
	size_t cap, wpos;
	bool wrap;
	uint64_t want;
	struct esdr_fsk_rx *fsk;
	uint8_t decim;
	/* The receiver runs at the rate over this (1 or 2): frame ends and counts scale back. */
	uint32_t slow;
} job;

static uint8_t rx_lsb = RX_LSB;
static atomic_t rx_busy_flag;
static int rx_ret;
static uint64_t rx_samples, rx_expect;
static uint32_t rx_gaps;
static int32_t dc_i, dc_q;
static int16_t peak;
static struct esp_sdr_diag_stats diag_stats;
static K_SEM_DEFINE(rx_start_sem, 0, 1);
static K_SEM_DEFINE(rx_done_sem, 0, 1);
static K_THREAD_STACK_DEFINE(esdr_rx_stack, RX_STACK);
static struct k_thread rx_thread;

/* Frames of the engine's receiver, for fsk_frames(): the engine adds, MicroPython takes. */
static struct esdr_fsk_frame frames[FRAMES];
static volatile unsigned int fr_head, fr_tail;
static uint32_t fr_dropped, fr_count;

static void emit(void *user, const struct esdr_fsk_frame *f)
{
	unsigned int next = (fr_head + 1U) % FRAMES;

	ARG_UNUSED(user);
	if (next == fr_tail) {
		fr_dropped++;
		return;
	}
	frames[fr_head] = *f;
	frames[fr_head].end = f->end * job.slow;
	barrier_dmem_fence_full();
	fr_head = next;
	fr_count++;
}

static void iq_sink(void *user, const int32_t *iq, size_t n, uint64_t index)
{
	int16_t s[2U * 24U];

	ARG_UNUSED(user);
	if (index != rx_expect && rx_samples != 0U) {
		rx_gaps++;
	}
	rx_expect = index + n;
	n = MIN(n, (size_t)24U);
	for (size_t k = 0; k < n; k++) {
		int32_t i = iq[2U * k], q = iq[2U * k + 1U];

		/* The LO's own DC sits in the band: a slow high-pass takes it out. */
		dc_i += (i * 256 - dc_i) >> DC_SHIFT;
		dc_q += (q * 256 - dc_q) >> DC_SHIFT;
		i -= dc_i / 256;
		q -= dc_q / 256;
		s[2U * k] = (int16_t)CLAMP(i, -32767, 32767);
		s[2U * k + 1U] = (int16_t)CLAMP(q, -32767, 32767);
		peak = MAX(peak, (int16_t)MAX(i < 0 ? -i : i, q < 0 ? -q : q));
	}
	if (job.fsk != NULL) {
		esdr_fsk_rx_process(job.fsk, s, n, emit, NULL);
	} else if (job.buf != NULL) {
		size_t m = n;

		if (!job.wrap) {
			m = (size_t)MIN((uint64_t)m, (uint64_t)(job.cap - job.wpos));
		}
		for (size_t k = 0; k < m; k++) {
			job.buf[2U * job.wpos] = s[2U * k];
			job.buf[2U * job.wpos + 1U] = s[2U * k + 1U];
			if (++job.wpos == job.cap) {
				job.wpos = 0;
			}
		}
		n = m;
	}
	barrier_dmem_fence_full();
	rx_samples += n * job.slow;
	if (rx_samples >= job.want || (job.buf != NULL && !job.wrap && rx_samples >= job.cap)) {
		esp_sdr_diag_stop();
	}
}

static void rx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		(void)k_sem_take(&rx_start_sem, K_FOREVER);

		/* This run's job, once it has been set. */
		struct esp_sdr_diag_cfg cfg = {
			.pins = diag_pins,
			.lsb = rx_lsb,
			.clk_div = 6,
			.seconds = 0,
			.decim = job.decim,
			.iq_sink = iq_sink,
		};

		rx_ret = esp_sdr_diag_run(&cfg, &diag_stats);
		atomic_set(&rx_busy_flag, 0);
		k_sem_give(&rx_done_sem);
	}
}

static int rx_init(void)
{
	static bool started;
	int ret;

	/* The Wi-Fi driver brings the PHY up shortly after boot. */
	for (int tries = 0; (ret = esp_sdr_init()) == -EAGAIN && tries < 100; tries++) {
		k_sleep(K_MSEC(50));
	}
	if (ret != 0) {
		return ret;
	}
	if (!started) {
		/* The PHY blobs print every transmit start through the ROM's channel, here the REPL's
		 * console: mute it (Zephyr's console does not use it). */
		esp_rom_install_channel_putc(1, NULL);
		k_thread_create(&rx_thread, esdr_rx_stack, K_THREAD_STACK_SIZEOF(esdr_rx_stack),
				rx_thread_fn, NULL, NULL, NULL, RX_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&rx_thread, "esdr_rx");
		started = true;
	}
	return 0;
}

static void rx_stop(void)
{
	if (atomic_get(&rx_busy_flag)) {
		esp_sdr_diag_stop();
		(void)k_sem_take(&rx_done_sem, K_FOREVER);
		k_sem_give(&rx_done_sem);
	}
}

/* ---- transmit: a burst through the paced TX DC path ---- */

static struct {
	const int16_t *buf;
	size_t n, pos, calls;
	int32_t scale;
	struct esp_sdr_paced_stats stats;
	int ret;
} tx;

static bool tx_input(void *user, int16_t *i, int16_t *q)
{
	ARG_UNUSED(user);
	tx.calls++;
	if (tx.pos >= tx.n) {
		return false;
	}
	*i = (int16_t)((tx.buf[2U * tx.pos] * tx.scale) >> 15);
	*q = (int16_t)((tx.buf[2U * tx.pos + 1U] * tx.scale) >> 15);
	tx.pos++;
	return true;
}

/* ---- helpers ---- */

static void check(int ret)
{
	if (ret < 0) {
		mp_raise_OSError(-ret);
	}
}

static void check_idle(void)
{
	if (atomic_get(&rx_busy_flag)) {
		mp_raise_OSError(EBUSY);
	}
}

static void tune_to(double hz, uint32_t *mhz, int32_t *khz)
{
	if (hz < ESP_SDR_FREQ_MIN_MHZ * 1e6 - 0.5e6 || hz > ESP_SDR_FREQ_MAX_MHZ * 1e6 + 0.5e6) {
		mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
	}
	*mhz = (uint32_t)llround(hz / 1e6);
	*khz = (int32_t)llround((hz - (double)*mhz * 1e6) / 1e3);
	check(esp_sdr_set_freq(*mhz));
	check(esp_sdr_set_freq_offset(*khz));
}

/* ---- the module ---- */

static mp_obj_t esdr_init(void)
{
	check(rx_init());
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_init_obj, esdr_init);

static mp_obj_t esdr_tune(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	static const mp_arg_t allowed[] = {
		{MP_QSTR_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_offset, MP_ARG_BOOL, {.u_bool = true}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	uint32_t mhz;
	int32_t khz;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	check_idle();
	tune_to(mp_obj_get_float(args[0].u_obj), &mhz, &khz);
	return mp_obj_new_float((mp_float_t)((double)mhz * 1e6 + (double)khz * 1e3));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tune_obj, 1, esdr_tune);

static mp_obj_t esdr_rx_gain(size_t n_args, const mp_obj_t *args)
{
	if (n_args == 0) {
		return mp_obj_new_int(esp_sdr_rx_get_gain());
	}
	check_idle();
	mp_int_t index = mp_obj_get_int(args[0]);

	check(esp_sdr_rx_set_gain(index < 0 ? ESP_SDR_RX_GAIN_AUTO : (int)index));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_rx_gain_obj, 0, 1, esdr_rx_gain);

static mp_obj_t esdr_rx_gain_max(void)
{
	return mp_obj_new_int(esp_sdr_rx_gain_max());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_gain_max_obj, esdr_rx_gain_max);

static mp_obj_t esdr_rx_bandwidth(mp_obj_t hz_in)
{
	mp_float_t hz = mp_obj_get_float(hz_in);

	check_idle();
	if (hz <= 0) {
		check(esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO));
	} else {
		uint32_t lo, hi, mhz = (uint32_t)ceilf((float)hz / 1e6f);

		esp_sdr_rx_bandwidth_range(&lo, &hi);
		check(esp_sdr_rx_set_bandwidth(CLAMP(mhz, lo, hi)));
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_rx_bandwidth_obj, esdr_rx_bandwidth);

static mp_obj_t esdr_channel_bw(mp_obj_t mode_in)
{
	check_idle();
	check(esp_sdr_set_channel_bw((unsigned int)mp_obj_get_int(mode_in)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_channel_bw_obj, esdr_channel_bw);

static mp_obj_t esdr_rx_lsb(size_t n_args, const mp_obj_t *args)
{
	if (n_args > 0) {
		mp_int_t v = mp_obj_get_int(args[0]);

		if (v < 0 || v > ESP_SDR_DIAG_LSB_MAX) {
			mp_raise_ValueError(MP_ERROR_TEXT("lsb 0..6"));
		}
		rx_lsb = (uint8_t)v;
	}
	return mp_obj_new_int(rx_lsb);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_rx_lsb_obj, 0, 1, esdr_rx_lsb);

static mp_obj_t esdr_rx_start_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_rate, ARG_buf, ARG_count, ARG_offset, ARG_monitor, ARG_wrap, ARG_fsk, ARG_slow };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_count, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
		{MP_QSTR_monitor, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_wrap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_fsk, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_slow, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	mp_float_t rate = mp_obj_get_float(args[ARG_rate].u_obj);
	uint32_t decim = (uint32_t)(DECIM_RATE / rate + 0.5f);

	if (decim < ESP_SDR_DIAG_DECIM_MIN || decim > ESP_SDR_DIAG_DECIM_MAX ||
	    fabsf((float)rate - (float)DECIM_RATE / (float)decim) > 0.5f) {
		mp_raise_ValueError(MP_ERROR_TEXT("rate not in esdr.RATES"));
	}
	if (args[ARG_count].u_int < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("count < 0"));
	}
	check(rx_init());
	if (atomic_get(&rx_busy_flag) || tx.buf != NULL) {
		mp_raise_OSError(EBUSY);
	}
	memset(&job, 0, sizeof(job));
	job.slow = 1;
	if (args[ARG_fsk].u_obj != mp_const_none) {
		mp_buffer_info_t bi;
		struct esdr_fsk_rx *rx;

		mp_get_buffer_raise(args[ARG_fsk].u_obj, &bi, MP_BUFFER_RW);
		rx = bi.buf;
		if (bi.len < sizeof(*rx) || ((uintptr_t)bi.buf & 7U) != 0U ||
		    bi.len < esdr_fsk_rx_size(rx->sps, rx->code_len)) {
			mp_raise_ValueError(MP_ERROR_TEXT("fsk: an fsk_rx_init() state"));
		}
		job.fsk = rx;
		job.slow = (uint32_t)args[ARG_slow].u_int;
		if ((job.slow != 1U && job.slow != 2U) || decim * job.slow > ESP_SDR_DIAG_DECIM_MAX) {
			mp_raise_ValueError(MP_ERROR_TEXT("slow: 1 or 2"));
		}
		decim *= job.slow;
	} else if (args[ARG_buf].u_obj != mp_const_none) {
		mp_buffer_info_t bi;

		mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_WRITE);
		if (((uintptr_t)bi.buf & 1U) != 0U || bi.len < 4) {
			mp_raise_ValueError(MP_ERROR_TEXT("buf: 16-bit aligned, one sample at least"));
		}
		job.buf = bi.buf;
		job.cap = bi.len / 4U;
		job.wrap = args[ARG_wrap].u_bool;
	}
	job.want = args[ARG_count].u_int != 0 ? (uint64_t)args[ARG_count].u_int : UINT64_MAX;
	job.decim = (uint8_t)decim;
	rx_samples = 0;
	rx_expect = 0;
	rx_gaps = 0;
	peak = 0;
	dc_i = 0;
	dc_q = 0;
	fr_head = fr_tail = 0;
	MP_STATE_PORT(esdr_rx_buf) = args[ARG_buf].u_obj;
	MP_STATE_PORT(esdr_rx_fsk) = args[ARG_fsk].u_obj;
	atomic_set(&rx_busy_flag, 1);
	k_sem_reset(&rx_done_sem);
	k_sem_give(&rx_start_sem);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_rx_start_obj, 1, esdr_rx_start_fn);

static mp_obj_t esdr_rx_stop_fn(void)
{
	rx_stop();
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	MP_STATE_PORT(esdr_rx_fsk) = MP_OBJ_NULL;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stop_obj, esdr_rx_stop_fn);

static mp_obj_t esdr_rx_busy_fn(void)
{
	return mp_obj_new_bool(atomic_get(&rx_busy_flag) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_busy_obj, esdr_rx_busy_fn);

static mp_obj_t esdr_rx_wait_fn(size_t n_args, const mp_obj_t *args)
{
	mp_int_t timeout = n_args > 0 ? mp_obj_get_int(args[0]) : -1;
	mp_uint_t t0 = mp_hal_ticks_ms();

	while (atomic_get(&rx_busy_flag)) {
		if (timeout >= 0 && mp_hal_ticks_ms() - t0 >= (mp_uint_t)timeout) {
			return mp_const_false;
		}
		mp_event_wait_ms(5);
	}
	return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_rx_wait_obj, 0, 1, esdr_rx_wait_fn);

static uint64_t rx_count(void)
{
	unsigned int key = irq_lock();
	uint64_t n = rx_samples;

	irq_unlock(key);
	return n;
}

static mp_obj_t esdr_rx_count_fn(void)
{
	return mp_obj_new_int_from_ull(rx_count());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_count_obj, esdr_rx_count_fn);

static mp_obj_t esdr_rx_stats_fn(void)
{
	mp_obj_t d = mp_obj_new_dict(12);

#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(samples, mp_obj_new_int_from_ull(rx_count()));
	PUT(gaps, mp_obj_new_int_from_uint(rx_gaps));
	PUT(peak, mp_obj_new_int(peak));
	PUT(frames, mp_obj_new_int_from_uint(fr_count));
	PUT(dropped, mp_obj_new_int_from_uint(fr_dropped));
	PUT(last_status, mp_obj_new_int(rx_ret));
	PUT(skips, mp_obj_new_int_from_uint(diag_stats.skips));
	PUT(skipped_blocks, mp_obj_new_int_from_uint(diag_stats.skipped_blocks));
	PUT(lsb, mp_obj_new_int(rx_lsb));
	PUT(sink_cycles, mp_obj_new_float((mp_float_t)diag_stats.sink_cycles_x100 / 100));
	PUT(iq_rate, mp_obj_new_int_from_uint(diag_stats.iq_rate));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stats_obj, esdr_rx_stats_fn);

static mp_obj_t esdr_fsk_frames_fn(void)
{
	mp_obj_t out = mp_obj_new_list(0, NULL);

	while (fr_tail != fr_head) {
		const struct esdr_fsk_frame *f = &frames[fr_tail];
		mp_obj_t t[9] = {
			mp_obj_new_bytes(f->data, f->len),
			mp_obj_new_int_from_uint(f->errors),
			mp_obj_new_int_from_uint(f->end),
			mp_obj_new_int_from_uint(f->phase),
			mp_obj_new_float((mp_float_t)f->dc),
			mp_obj_new_float((mp_float_t)f->amp),
			mp_obj_new_float((mp_float_t)f->var),
			mp_obj_new_float((mp_float_t)f->power),
			mp_obj_new_int_from_uint(f->floor),
		};

		mp_obj_list_append(out, mp_obj_new_tuple(9, t));
		barrier_dmem_fence_full();
		fr_tail = (fr_tail + 1U) % FRAMES;
	}
	return out;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_fsk_frames_obj, esdr_fsk_frames_fn);

/* tx_start(rate, hz, gain, buf, count=0, scale=1.0) -> centre: buf played once. */
static mp_obj_t esdr_tx_start_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_rate, ARG_hz, ARG_gain, ARG_buf, ARG_repeat, ARG_count, ARG_scale, ARG_gen };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_gain, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_repeat, MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_count, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_scale, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_gen, MP_ARG_OBJ, {.u_obj = mp_const_none}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	mp_buffer_info_t bi;
	uint32_t mhz;
	int32_t khz;
	unsigned int key;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	mp_int_t rate = args[ARG_rate].u_int;

	if (args[ARG_buf].u_obj == mp_const_none || args[ARG_repeat].u_bool ||
	    args[ARG_gen].u_obj != mp_const_none) {
		mp_raise_ValueError(MP_ERROR_TEXT("tx_start on the C5: a buffer, played once"));
	}
	if (rate <= 0 || (240000000 % (rate * TX_INTERP)) != 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("rate: 4 times it must divide 240 MHz"));
	}
	mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_READ);
	if (((uintptr_t)bi.buf & 1U) != 0U || bi.len < 4) {
		mp_raise_ValueError(MP_ERROR_TEXT("buf: 16-bit aligned, one sample at least"));
	}
	check(rx_init());
	check_idle();

	size_t n = bi.len / 4U;

	if (args[ARG_count].u_int > 0 && (size_t)args[ARG_count].u_int < n) {
		n = (size_t)args[ARG_count].u_int;
	}
	mp_int_t g = CLAMP(args[ARG_gain].u_int, 0, TX_GAIN_MAX);
	float s = args[ARG_scale].u_obj == mp_const_none ? 1.0f
							  : (float)mp_obj_get_float(args[ARG_scale].u_obj);

	s *= powf(10.0f, (float)(g - TX_GAIN_MAX) / 20.0f);
	tune_to(mp_obj_get_float(args[ARG_hz].u_obj), &mhz, &khz);
	memset(&tx, 0, sizeof(tx));
	tx.buf = bi.buf;
	tx.n = n;
	tx.scale = (int32_t)(CLAMP(s, 0.0f, 1.0f) * 32767.0f);
	tx.ret = esp_sdr_tx_paced_start((uint32_t)rate, TX_INTERP, 0, tx_input, NULL);
	if (tx.ret == 0) {
		key = irq_lock();
		esp_sdr_paced_sync();
		while (tx.calls < tx.n + TX_TAIL) {
			(void)esp_sdr_tx_paced_poll();
		}
		irq_unlock(key);
		esp_sdr_paced_get_stats(&tx.stats);
		esp_sdr_tx_paced_stop();
	}
	tx.buf = NULL;
	check(tx.ret);
	return mp_obj_new_float((mp_float_t)((double)mhz * 1e6 + (double)khz * 1e3));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tx_start_obj, 3, esdr_tx_start_fn);

static mp_obj_t esdr_tx_none(void)
{
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_stop_obj, esdr_tx_none);

static mp_obj_t esdr_tx_busy_fn(void)
{
	return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_busy_obj, esdr_tx_busy_fn);

static mp_obj_t esdr_tx_wait_fn(size_t n_args, const mp_obj_t *args)
{
	ARG_UNUSED(n_args);
	ARG_UNUSED(args);
	return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_tx_wait_obj, 0, 1, esdr_tx_wait_fn);

static mp_obj_t esdr_tx_stats_fn(void)
{
	mp_obj_t d = mp_obj_new_dict(6);

#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(played, mp_obj_new_int_from_uint(tx.pos));
	PUT(fed, mp_obj_new_int_from_uint(tx.n));
	PUT(late, mp_obj_new_int_from_uint(tx.stats.late));
	PUT(late_max, mp_obj_new_int_from_uint(tx.stats.late_max));
	PUT(last_error, mp_obj_new_int(tx.ret));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_stats_obj, esdr_tx_stats_fn);

static mp_obj_t esdr_tx_gain_max_fn(void)
{
	return MP_OBJ_NEW_SMALL_INT(TX_GAIN_MAX);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_gain_max_obj, esdr_tx_gain_max_fn);

/* ---- board ---- */

static bool started;

static mp_obj_t esdr_first_start(void)
{
	bool first = !started;

	started = true;
	return mp_obj_new_bool(first);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_first_start_obj, esdr_first_start);

static char next_app[128];

static mp_obj_t esdr_next_app(size_t n_args, const mp_obj_t *args)
{
	char out[sizeof(next_app)];
	const char *path;
	size_t len;

	if (n_args == 0) {
		strcpy(out, next_app);
		next_app[0] = '\0';
		return mp_obj_new_str_from_cstr(out);
	}
	if (!mp_obj_is_str(args[0])) {
		mp_raise_TypeError(MP_ERROR_TEXT("path must be a str"));
	}
	path = mp_obj_str_get_data(args[0], &len);
	if (len >= sizeof(next_app) || memchr(path, '\0', len) != NULL) {
		mp_raise_ValueError(MP_ERROR_TEXT("path too long or with a NUL"));
	}
	memcpy(next_app, path, len);
	next_app[len] = '\0';
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_next_app_obj, 0, 1, esdr_next_app);

static mp_obj_t esdr_home(void)
{
	return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_home_obj, esdr_home);

static mp_obj_t esdr_reset_reason(void)
{
	static const char *const names[] = {
		"unknown", "power-on", "external pin", "software", "panic", "interrupt watchdog",
		"task watchdog", "other watchdog", "deep sleep", "brownout", "sdio", "usb", "jtag",
		"efuse", "power glitch", "cpu lockup",
	};
	unsigned int r = (unsigned int)esp_reset_reason();

	return mp_obj_new_str_from_cstr(r < ARRAY_SIZE(names) ? names[r] : "?");
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_reset_reason_obj, esdr_reset_reason);

/* ---- heap, added at every (soft) start; the radio stops before a soft reset ---- */

void esdr_port_init(void)
{
	static uint8_t *libc_area;
	static size_t libc_len;

	gc_add(__esp_sdr_bank_start + HEAP_GUARD, __esp_sdr_bank_end);
	if (libc_area == NULL) {
		/* The largest block the libc heap gives, less what it keeps for others. */
		for (size_t len = 64U * 1024U; len >= 2U * LIBC_KEEP; len -= 1024U) {
			void *p = malloc(len);

			if (p != NULL) {
				free(p);
				libc_len = len - LIBC_KEEP;
				libc_area = malloc(libc_len);
				break;
			}
		}
	}
	if (libc_area != NULL) {
		gc_add(libc_area, libc_area + libc_len);
	}
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	MP_STATE_PORT(esdr_rx_fsk) = MP_OBJ_NULL;
}

void esdr_port_deinit(void)
{
	rx_stop();
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	MP_STATE_PORT(esdr_rx_fsk) = MP_OBJ_NULL;
}

/* esdr_link.c */
extern const mp_obj_fun_builtin_fixed_t esdr_fsk_rx_reset_obj, esdr_fsk_rx_state_obj;
extern const mp_obj_fun_builtin_var_t esdr_fsk_rx_init_obj, esdr_fsk_rx_obj, esdr_fsk_mod_obj;

static const mp_rom_obj_tuple_t esdr_rates_obj = {
	{&mp_type_tuple},
	5,
	{
		MP_ROM_INT(DECIM_RATE / 10U),
		MP_ROM_INT(DECIM_RATE / 20U),
		MP_ROM_INT(DECIM_RATE / 40U),
		MP_ROM_INT(DECIM_RATE / 80U),
		MP_ROM_INT(DECIM_RATE / 160U),
	},
};

static const mp_rom_obj_tuple_t esdr_burst_rates_obj = {{&mp_type_tuple}, 0, {}};

static const mp_rom_map_elem_t esdr_globals_table[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esdr)},
	{MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esdr_init_obj)},
	{MP_ROM_QSTR(MP_QSTR_tune), MP_ROM_PTR(&esdr_tune_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain), MP_ROM_PTR(&esdr_rx_gain_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain_max), MP_ROM_PTR(&esdr_rx_gain_max_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_bandwidth), MP_ROM_PTR(&esdr_rx_bandwidth_obj)},
	{MP_ROM_QSTR(MP_QSTR_channel_bw), MP_ROM_PTR(&esdr_channel_bw_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_lsb), MP_ROM_PTR(&esdr_rx_lsb_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_start), MP_ROM_PTR(&esdr_rx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stop), MP_ROM_PTR(&esdr_rx_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_busy), MP_ROM_PTR(&esdr_rx_busy_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_wait), MP_ROM_PTR(&esdr_rx_wait_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_count), MP_ROM_PTR(&esdr_rx_count_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stats), MP_ROM_PTR(&esdr_rx_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_frames), MP_ROM_PTR(&esdr_fsk_frames_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_start), MP_ROM_PTR(&esdr_tx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_stop), MP_ROM_PTR(&esdr_tx_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_busy), MP_ROM_PTR(&esdr_tx_busy_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_wait), MP_ROM_PTR(&esdr_tx_wait_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_stats), MP_ROM_PTR(&esdr_tx_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_gain_max), MP_ROM_PTR(&esdr_tx_gain_max_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx_init), MP_ROM_PTR(&esdr_fsk_rx_init_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx), MP_ROM_PTR(&esdr_fsk_rx_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx_reset), MP_ROM_PTR(&esdr_fsk_rx_reset_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx_state), MP_ROM_PTR(&esdr_fsk_rx_state_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_mod), MP_ROM_PTR(&esdr_fsk_mod_obj)},
	{MP_ROM_QSTR(MP_QSTR_first_start), MP_ROM_PTR(&esdr_first_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_next_app), MP_ROM_PTR(&esdr_next_app_obj)},
	{MP_ROM_QSTR(MP_QSTR_home), MP_ROM_PTR(&esdr_home_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&esdr_reset_reason_obj)},
	{MP_ROM_QSTR(MP_QSTR_RATES), MP_ROM_PTR(&esdr_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_BURST_RATES), MP_ROM_PTR(&esdr_burst_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_FULL_SCALE), MP_ROM_INT(FULL_SCALE)},
	{MP_ROM_QSTR(MP_QSTR_TX_PREBUF_S), MP_ROM_INT(0)},
	{MP_ROM_QSTR(MP_QSTR_RX_FSK_SLOW), MP_ROM_INT(2)},
};
static MP_DEFINE_CONST_DICT(esdr_globals, esdr_globals_table);

const mp_obj_module_t esdr_module = {
	.base = {&mp_type_module},
	.globals = (mp_obj_dict_t *)&esdr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esdr, esdr_module);
