/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module: the radio for the gnuradio shim, and the board.
 *
 *   init()                     radio up, engine thread started (Wi-Fi must have started)
 *   tune(hz, offset=True)      LO as whole MHz plus a kHz PLL offset; with offset the LO sits
 *                              4 MHz below and the ring mixes the band down. Returns the
 *                              centre actually tuned (within 500 Hz).
 *   rx_gain([index])           set (-1: automatic) or read the receive gain index
 *   rx_gain_max()
 *   rx_bandwidth(hz)           receive filter, 0: the PHY's own setting
 *   RATES                      the continuous rates, 16 MS/s / 64 .. 1024
 *   BURST_RATES                80, 40, 16 MS/s: rx_start() takes single-bank bursts at
 *                              these, contiguous within a burst only (16380 samples)
 *   rx_start(rate, buf=None, count=0, offset=True, monitor=False, wrap=False)
 *                              capture into buf (int16 I, Q interleaved) or only count;
 *                              count 0: until rx_stop() or the buffer is full; monitor
 *                              keeps the latest samples for rx_latest() (displays); wrap:
 *                              buf is a ring, sample k at k % its length, until rx_stop()
 *   rx_wait([timeout_ms]) -> done, rx_stop(), rx_busy(), rx_stats() -> dict
 *                              samples, runs, errors, gaps, gap_samples, last_status,
 *                              last_detail; the ring run's units, abandoned, lost_pairs,
 *                              late_max, slice_max, stalls, cycles_x100 (esdr_rx.h)
 *   rx_count() -> int          samples the capture has stored so far
 *   fsk_rx_init(), fsk_rx(), fsk_rx_reset(), fsk_rx_state(), fsk_mod()
 *                              the packet modem (esdr_link.c)
 *   sc16_to_fc32(src, dst, scale)  int16 pairs to float32 pairs times scale
 *   tx_start(rate, hz, gain, buf=None, repeat=False, count=0, scale=1.0, gen=None) -> centre
 *                              transmit buf (int16 I, Q) or the generator gen = (wave, cycles
 *                              per sample, amp, dc_i, dc_q) at rate (divides 40 MS/s)
 *   tx_loop(rate, hz, gain, buf=None, interp=1, scale=1.0, gen=None) -> centre
 *                              loop one period of gen or buf at the DAC rate (40 or 80 MS/s)
 *   tx_period(cycles) -> (k, n)  the loop's whole cycles k in n samples
 *   channel_bw(mode)           PHY channel bandwidth for the next tuning: 0 20 MHz, 2 40 MHz
 *   tx_lpf(a, b)               transmit low-pass codes, 0 widest, -1 the PHY's
 *   tx_gen(...), tx_scale(s)   change the generator or the buffer scale while transmitting
 *   tx_wait([timeout_ms]) -> done, tx_stop(), tx_busy(), tx_stats() -> dict, tx_gain_max()
 *   fc32_to_sc16(src, dst, scale)  float32 pairs times scale to clipped int16 pairs
 *   shell(cmd) -> str          a Zephyr shell command (usj, mcuboot confirm, ...)
 *   claim(mnt), release(mnt)   take a volume ("/FLASH:", "/SD:") from the USB host for
 *                              writing, give it back (src/usb_disks.h)
 *   sd_info() -> dict           what the SD stack negotiated with the card
 *   reset_reason() -> str      why the chip last started (power-on, brownout, watchdogs)
 *   crash() -> dict or None    what ended the previous boot (fatal error, watchdog, double
 *                              exception)
 *   next_app([path]) -> str    the app main.py starts after the next soft reset (launcher.py);
 *                              without an argument returns it and forgets it
 *   home() -> bool             whether the knob was held for the launcher since the last call
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <esp_attr.h>
#include <esp_system.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_ring.h>
#include <esp_sdr/esp_sdr_rx.h>

#ifdef CONFIG_LVGL
#include <lvgl.h>
#endif

#include "py/cstack.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"

#include "app_crash.h"
#include "usb_disks.h"
#include "esdr_rx.h"
#include "esdr_tx.h"

#define OFFSET_HZ (ESP_SDR_RING_RATE_HZ / 4U)

/* The capture buffer, kept alive while the engine writes into it. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_rx_buf);
/* The transmit buffer, read by the feeder thread while it plays. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_tx_buf);

/*
 * ---- the launcher (modules/launcher.py) ----
 *
 * The app to start survives the soft reset launcher.start() makes, outside the heap. Holding
 * the knob (the LVGL encoder's switch) for KNOB_HOME_MS ends what runs as Ctrl-C does, flagged
 * so that the launcher shows its menu rather than the REPL. When no Ctrl-C can help (the REPL
 * waits for input, keyboard interrupts are off, what runs ignored a hold KNOB_AGAIN_MS ago),
 * holding the knob restarts the board. A hold counts for the launcher for KNOB_FRESH_MS: a
 * hold whose Ctrl-C an app swallowed does not turn a later Ctrl-C from the console into one.
 */

#define KNOB_HOME_MS  1500
#define KNOB_AGAIN_MS 5000
#define KNOB_FRESH_MS 15000
#define KNOB_NODE     DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_lvgl_encoder_input)

static char next_app[128];
static atomic_t knob_home;
static int64_t knob_time;

#if DT_NODE_EXISTS(KNOB_NODE)
static void knob_held(struct k_work *work)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(work);
	if (mp_interrupt_char < 0 ||
	    (atomic_get(&knob_home) != 0 && now - knob_time < KNOB_AGAIN_MS)) {
		sys_reboot(SYS_REBOOT_COLD);
	}
	knob_time = now;
	(void)atomic_set(&knob_home, 1);
	mp_hal_signal_event();
	mp_sched_keyboard_interrupt();
}

static K_WORK_DELAYABLE_DEFINE(knob_work, knob_held);

static void knob_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY || evt->code != DT_PROP(KNOB_NODE, button_input_code)) {
		return;
	}
	if (evt->value != 0) {
		(void)k_work_reschedule(&knob_work, K_MSEC(KNOB_HOME_MS));
	} else {
		(void)k_work_cancel_delayable(&knob_work);
	}
}
INPUT_CALLBACK_DEFINE(NULL, knob_input, NULL);
#endif

/*
 * ---- heap, added at every (soft) start ----
 *
 * The port's own heap (MICROPY_HEAP_SIZE) is a token: DRAM ends at the ring banks. The next
 * blocks are internal RAM above the capture bank, the bulk is PSRAM. LVGL allocates from this
 * heap too (CONFIG_LV_Z_MEM_POOL_MICROPYTHON), its state hangs off a GC root.
 */

#define FAST_HEAP_SIZE 3072

ESP_SDR_HIGH_BUF_DEFINE(uint8_t, fast_heap, [FAST_HEAP_SIZE], 16);
static EXT_RAM_BSS_ATTR uint8_t psram_heap[CONFIG_APP_MP_PSRAM_HEAP_KB * 1024] __aligned(16);

void esdr_port_init(void)
{
	/* The port sized its stack check for the main thread; MicroPython runs in its own (boot.c). */
	const struct k_thread *self = k_current_get();

	mp_cstack_init_with_top((void *)(self->stack_info.start + self->stack_info.size),
				self->stack_info.size);
	gc_add(fast_heap, fast_heap + sizeof(fast_heap));
	gc_add(psram_heap, psram_heap + sizeof(psram_heap));
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
	(void)atomic_clear(&knob_home);
}

void esdr_port_deinit(void)
{
	/* The heap goes next: nothing may write into it any more. */
	esdr_rx_stop();
	(void)esdr_rx_wait(-1);
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	esdr_tx_loop_stop();
	esdr_tx_stop();
	(void)esdr_tx_wait(3000);
	MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
#ifdef CONFIG_LVGL
	/*
	 * LVGL's state, displays and draw buffers included, was in the heap, which the port has
	 * swept already (lv_deinit() would walk freed blocks). Forget it: the binding's roots and
	 * flags reset, the next import and ZDD.init() build LVGL afresh.
	 */
	mp_lv_deinit_gc();
#endif
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
	if (esdr_rx_busy() || esdr_tx_busy()) {
		mp_raise_OSError(EBUSY);
	}
}

/* The native rate (enum esp_sdr_rate) of a BURST_RATES entry, else -1. */
static int rate_native(mp_obj_t rate_in)
{
	static const int natives[] = {ESP_SDR_RATE_80MSPS, ESP_SDR_RATE_40MSPS, ESP_SDR_RATE_16MSPS};
	mp_float_t rate = mp_obj_get_float(rate_in);

	for (size_t k = 0; k < ARRAY_SIZE(natives); k++) {
		if (fabsf((float)rate - (float)esp_sdr_rx_rate_hz(natives[k])) < 0.5f) {
			return natives[k];
		}
	}
	return -1;
}

static unsigned int rate_decim(mp_obj_t rate_in)
{
	mp_float_t rate = mp_obj_get_float(rate_in);

	for (unsigned int d = ESP_SDR_RING_DECIM_MIN; d <= ESP_SDR_RING_DECIM_MAX; d *= 2U) {
		if (fabsf((float)rate - (float)(ESP_SDR_RING_RATE_HZ / d)) < 0.5f) {
			return d;
		}
	}
	mp_raise_ValueError(MP_ERROR_TEXT("rate not in esdr.RATES"));
}

/* ---- radio ---- */

static mp_obj_t esdr_init(void)
{
	check(esdr_rx_init());
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_init_obj, esdr_init);

static mp_obj_t esdr_tune(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_hz, ARG_offset };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_offset, MP_ARG_BOOL, {.u_bool = true}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	check_idle();

	double hz = mp_obj_get_float(args[ARG_hz].u_obj);
	double lo = hz - (args[ARG_offset].u_bool ? (double)OFFSET_HZ : 0.0);

	if (lo < ESP_SDR_FREQ_MIN_MHZ * 1e6 - 0.5e6 || lo > ESP_SDR_FREQ_MAX_MHZ * 1e6 + 0.5e6) {
		mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
	}

	uint32_t mhz = (uint32_t)llround(lo / 1e6);
	int32_t khz = (int32_t)llround((lo - (double)mhz * 1e6) / 1e3);

	check(esp_sdr_set_freq(mhz));
	check(esp_sdr_set_freq_offset(khz));
	return mp_obj_new_float((mp_float_t)((double)mhz * 1e6 + (double)khz * 1e3 +
					     (args[ARG_offset].u_bool ? (double)OFFSET_HZ : 0.0)));
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
		uint32_t lo, hi;
		uint32_t mhz = (uint32_t)ceilf((float)hz / 1e6f);

		esp_sdr_rx_bandwidth_range(&lo, &hi);
		check(esp_sdr_rx_set_bandwidth(CLAMP(mhz, lo, hi)));
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_rx_bandwidth_obj, esdr_rx_bandwidth);

/* ---- receive ---- */

static mp_obj_t esdr_rx_start_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_rate, ARG_buf, ARG_count, ARG_offset, ARG_monitor, ARG_wrap };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_count, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
		{MP_QSTR_monitor, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_wrap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	int16_t *buf = NULL;
	size_t cap = 0;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	int native = rate_native(args[ARG_rate].u_obj);
	unsigned int decim = native >= 0 ? 0U : rate_decim(args[ARG_rate].u_obj);

	if (args[ARG_count].u_int < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("count < 0"));
	}
	if (args[ARG_buf].u_obj != mp_const_none) {
		mp_buffer_info_t bi;

		mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_WRITE);
		if (((uintptr_t)bi.buf & 1U) != 0U || bi.len < 4) {
			mp_raise_ValueError(MP_ERROR_TEXT("buf: 16-bit aligned, one sample at least"));
		}
		buf = bi.buf;
		cap = bi.len / 4U;
	}
	if (esdr_tx_busy()) {
		mp_raise_OSError(EBUSY);
	}
	if (native >= 0) {
		if (args[ARG_wrap].u_bool) {
			mp_raise_ValueError(MP_ERROR_TEXT("wrap: the continuous rates only"));
		}
		check(esdr_rx_start_burst(native, buf, cap, (uint64_t)args[ARG_count].u_int,
					  args[ARG_monitor].u_bool));
		MP_STATE_PORT(esdr_rx_buf) = args[ARG_buf].u_obj;
		return mp_const_none;
	}
	check(esdr_rx_start(decim, args[ARG_offset].u_bool, buf, cap,
			    (uint64_t)args[ARG_count].u_int, args[ARG_monitor].u_bool,
			    args[ARG_wrap].u_bool));
	MP_STATE_PORT(esdr_rx_buf) = args[ARG_buf].u_obj;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_rx_start_obj, 1, esdr_rx_start_fn);

static mp_obj_t esdr_rx_stop_fn(void)
{
	esdr_rx_stop();
	(void)esdr_rx_wait(-1);
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stop_obj, esdr_rx_stop_fn);

static mp_obj_t esdr_rx_count_fn(void)
{
	return mp_obj_new_int_from_ull(esdr_rx_count());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_count_obj, esdr_rx_count_fn);

static mp_obj_t esdr_rx_busy_fn(void)
{
	return mp_obj_new_bool(esdr_rx_busy());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_busy_obj, esdr_rx_busy_fn);

/* Waits in short slices so that Ctrl-C and scheduled callbacks run; on an exception the
 * capture is stopped before it propagates. */
static mp_obj_t esdr_rx_wait_fn(size_t n_args, const mp_obj_t *args)
{
	mp_int_t timeout = n_args > 0 ? mp_obj_get_int(args[0]) : -1;
	mp_uint_t t0 = mp_hal_ticks_ms();
	bool done = false;
	nlr_buf_t nlr;

	if (nlr_push(&nlr) == 0) {
		for (;;) {
			done = esdr_rx_wait(20);
			if (done || (timeout >= 0 && mp_hal_ticks_ms() - t0 >= (mp_uint_t)timeout)) {
				break;
			}
			mp_handle_pending(true);
		}
		nlr_pop();
	} else {
		esdr_rx_stop();
		(void)esdr_rx_wait(-1);
		MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
		nlr_jump(nlr.ret_val);
	}
	if (done) {
		MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	}
	return mp_obj_new_bool(done);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_rx_wait_obj, 0, 1, esdr_rx_wait_fn);

static mp_obj_t esdr_rx_stats_fn(void)
{
	struct esdr_rx_stats s;
	mp_obj_t d = mp_obj_new_dict(14);

	esdr_rx_get_stats(&s);
#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(samples, mp_obj_new_int_from_ull(s.samples));
	PUT(runs, mp_obj_new_int_from_uint(s.runs));
	PUT(errors, mp_obj_new_int_from_uint(s.errors));
	PUT(gaps, mp_obj_new_int_from_uint(s.gaps));
	PUT(gap_samples, mp_obj_new_int_from_ull(s.gap_samples));
	PUT(last_status, mp_obj_new_int(s.last_status));
	PUT(last_detail, mp_obj_new_int_from_uint(s.last_detail));
	PUT(units, mp_obj_new_int_from_uint(s.units));
	PUT(abandoned, mp_obj_new_int_from_uint(s.abandoned));
	PUT(lost_pairs, mp_obj_new_int_from_uint(s.lost_pairs));
	PUT(late_max, mp_obj_new_int_from_uint(s.late_max));
	PUT(slice_max, mp_obj_new_int_from_uint(s.slice_max));
	PUT(stalls, mp_obj_new_int_from_uint(s.stalls));
	PUT(cycles_x100, mp_obj_new_int_from_uint(s.cycles_x100));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stats_obj, esdr_rx_stats_fn);

/* ---- transmit ---- */

static void tx_gen_from(struct esdr_tx_gen *g, mp_obj_t t)
{
	mp_obj_t *items;
	size_t len;

	mp_obj_get_array(t, &len, &items);
	if (len != 5) {
		mp_raise_ValueError(MP_ERROR_TEXT("gen: (wave, cycles, amp, dc_i, dc_q)"));
	}
	mp_int_t wave = mp_obj_get_int(items[0]);

	/* gnuradio.analog's GR_CONST_WAVE .. GR_SAW_WAVE are 100 .. 105. */
	g->wave = (enum esdr_tx_wave)(wave >= 100 ? wave - 100 : wave);
	g->cycles = (float)mp_obj_get_float(items[1]);
	g->amp = (float)mp_obj_get_float(items[2]);
	g->dc_i = (float)mp_obj_get_float(items[3]);
	g->dc_q = (float)mp_obj_get_float(items[4]);
}

/* tx_start(rate, hz, gain, buf=None, repeat=False, count=0, scale=1.0, gen=None) -> centre
 * Transmits from the generator gen = (wave, cycles per sample, amp, dc_i, dc_q), or plays buf
 * (int16 I, Q). rate divides 40 MS/s. The centre is within 500 Hz of hz. */
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
	struct esdr_tx_gen g = {ESDR_WAVE_COS, 0.0f, 0.0f, 0.0f, 0.0f};
	const int16_t *buf = NULL;
	size_t len = 0;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	mp_int_t rate = args[ARG_rate].u_int;

	if (rate < 2 || 40000000 % rate != 0 || 40000000 / rate < 2) {
		mp_raise_ValueError(MP_ERROR_TEXT("rate: a divisor of 40 MS/s, at most 20 MS/s"));
	}
	if (args[ARG_count].u_int < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("count < 0"));
	}
	double hz = mp_obj_get_float(args[ARG_hz].u_obj);

	if (hz < ESP_SDR_FREQ_MIN_MHZ * 1e6 - 0.5e6 || hz > ESP_SDR_FREQ_MAX_MHZ * 1e6 + 0.5e6) {
		mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
	}
	if (args[ARG_buf].u_obj != mp_const_none) {
		mp_buffer_info_t bi;

		mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_READ);
		if (((uintptr_t)bi.buf & 1U) != 0U || bi.len < 4) {
			mp_raise_ValueError(MP_ERROR_TEXT("buf: 16-bit aligned, one sample at least"));
		}
		buf = bi.buf;
		len = bi.len / 4U;
	} else if (args[ARG_gen].u_obj != mp_const_none) {
		tx_gen_from(&g, args[ARG_gen].u_obj);
	}
	uint32_t mhz = (uint32_t)llround(hz / 1e6);
	int32_t khz = (int32_t)llround((hz - (double)mhz * 1e6) / 1e3);
	float scale = args[ARG_scale].u_obj == mp_const_none ? 1.0f
							      : (float)mp_obj_get_float(args[ARG_scale].u_obj);

	if (esdr_rx_busy()) {
		mp_raise_OSError(EBUSY);
	}
	check(esdr_tx_start(mhz, khz, (uint32_t)rate, args[ARG_gain].u_int, buf, len,
			    args[ARG_repeat].u_bool, &g, scale, (uint64_t)args[ARG_count].u_int));
	MP_STATE_PORT(esdr_tx_buf) = args[ARG_buf].u_obj;
	return mp_obj_new_float((mp_float_t)((double)mhz * 1e6 + (double)khz * 1e3));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tx_start_obj, 3, esdr_tx_start_fn);

/* channel_bw(mode): the PHY's channel bandwidth mode for the next tuning, 0 (20 MHz, the
 * receiver's characterized setting) to 2 (40 MHz: the analog filters pass about +-18 MHz). */
static mp_obj_t esdr_channel_bw_fn(mp_obj_t mode_in)
{
	check_idle();
	check(esp_sdr_set_channel_bw((unsigned int)mp_obj_get_int(mode_in)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_channel_bw_obj, esdr_channel_bw_fn);

/* tx_lpf(a, b): the transmit low-pass codes (experimental), 0 the widest, -1 the PHY's. */
static mp_obj_t esdr_tx_lpf_fn(mp_obj_t a_in, mp_obj_t b_in)
{
	check_idle();
	check(esp_sdr_tx_set_lpf(mp_obj_get_int(a_in), mp_obj_get_int(b_in)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(esdr_tx_lpf_obj, esdr_tx_lpf_fn);

/* tx_loop(rate, hz, gain, buf=None, interp=1, scale=1.0, gen=None) -> centre
 * Loop playback at the DAC rate (40 or 80 MS/s): one period of the generator gen (cycles per
 * DAC sample) or buf (int16 I, Q) interpolated by interp, until tx_stop(). */
static mp_obj_t esdr_tx_loop_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_rate, ARG_hz, ARG_gain, ARG_buf, ARG_interp, ARG_scale, ARG_gen };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_gain, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_interp, MP_ARG_INT, {.u_int = 1}},
		{MP_QSTR_scale, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_gen, MP_ARG_OBJ, {.u_obj = mp_const_none}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	struct esdr_tx_gen g = {ESDR_WAVE_COS, 0.0f, 0.0f, 0.0f, 0.0f};
	const int16_t *buf = NULL;
	size_t len = 0;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	mp_int_t rate = args[ARG_rate].u_int;

	if (rate != 40000000 && rate != 80000000) {
		mp_raise_ValueError(MP_ERROR_TEXT("rate: 40 or 80 MS/s"));
	}
	double hz = mp_obj_get_float(args[ARG_hz].u_obj);

	if (hz < ESP_SDR_FREQ_MIN_MHZ * 1e6 - 0.5e6 || hz > ESP_SDR_FREQ_MAX_MHZ * 1e6 + 0.5e6) {
		mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
	}
	if (args[ARG_buf].u_obj != mp_const_none) {
		mp_buffer_info_t bi;

		mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_READ);
		if (((uintptr_t)bi.buf & 1U) != 0U || bi.len < 4) {
			mp_raise_ValueError(MP_ERROR_TEXT("buf: 16-bit aligned, one sample at least"));
		}
		buf = bi.buf;
		len = bi.len / 4U;
	} else if (args[ARG_gen].u_obj != mp_const_none) {
		tx_gen_from(&g, args[ARG_gen].u_obj);
	}
	uint32_t mhz = (uint32_t)llround(hz / 1e6);
	int32_t khz = (int32_t)llround((hz - (double)mhz * 1e6) / 1e3);
	float scale = args[ARG_scale].u_obj == mp_const_none ? 1.0f
							      : (float)mp_obj_get_float(args[ARG_scale].u_obj);

	check(esdr_tx_loop_start(mhz, khz, (uint32_t)rate, args[ARG_gain].u_int, buf, len,
				 (uint32_t)args[ARG_interp].u_int, &g, scale));
	MP_STATE_PORT(esdr_tx_buf) = args[ARG_buf].u_obj;
	return mp_obj_new_float((mp_float_t)((double)mhz * 1e6 + (double)khz * 1e3));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tx_loop_obj, 3, esdr_tx_loop_fn);

/* tx_period(cycles) -> (k, n): the loop's k whole cycles in n samples for cycles per sample. */
static mp_obj_t esdr_tx_period_fn(mp_obj_t cycles_in)
{
	int32_t k;
	uint32_t n;
	mp_obj_t t[2];

	esdr_tx_period((float)mp_obj_get_float(cycles_in), &k, &n);
	t[0] = mp_obj_new_int(k);
	t[1] = mp_obj_new_int_from_uint(n);
	return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_tx_period_obj, esdr_tx_period_fn);

/* tx_gen(wave, cycles, amp, dc_i, dc_q): the generator's new settings, also while running. */
static mp_obj_t esdr_tx_gen_fn(size_t n_args, const mp_obj_t *args)
{
	struct esdr_tx_gen g;
	mp_obj_t t = mp_obj_new_tuple(n_args, args);

	tx_gen_from(&g, t);
	esdr_tx_set_gen(&g);
	if (esdr_tx_looping()) {
		check(esdr_tx_loop_update());
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_tx_gen_obj, 5, 5, esdr_tx_gen_fn);

static mp_obj_t esdr_tx_scale_fn(mp_obj_t scale_in)
{
	esdr_tx_set_scale((float)mp_obj_get_float(scale_in));
	if (esdr_tx_looping()) {
		check(esdr_tx_loop_update());
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_tx_scale_obj, esdr_tx_scale_fn);

static mp_obj_t esdr_tx_stop_fn(void)
{
	esdr_tx_loop_stop();
	esdr_tx_stop();
	if (!esdr_tx_wait(3000)) {
		mp_raise_OSError(ETIMEDOUT);
	}
	MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_stop_obj, esdr_tx_stop_fn);

static mp_obj_t esdr_tx_busy_fn(void)
{
	return mp_obj_new_bool(esdr_tx_busy());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_busy_obj, esdr_tx_busy_fn);

/* As rx_wait(): in short slices, stopping the transmission on an exception. */
static mp_obj_t esdr_tx_wait_fn(size_t n_args, const mp_obj_t *args)
{
	mp_int_t timeout = n_args > 0 ? mp_obj_get_int(args[0]) : -1;
	mp_uint_t t0 = mp_hal_ticks_ms();
	bool done = false;
	nlr_buf_t nlr;

	if (nlr_push(&nlr) == 0) {
		for (;;) {
			done = esdr_tx_wait(20);
			if (done || (timeout >= 0 && mp_hal_ticks_ms() - t0 >= (mp_uint_t)timeout)) {
				break;
			}
			mp_handle_pending(true);
		}
		nlr_pop();
	} else {
		esdr_tx_stop();
		(void)esdr_tx_wait(-1);
		MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
		nlr_jump(nlr.ret_val);
	}
	if (done) {
		MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
	}
	return mp_obj_new_bool(done);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_tx_wait_obj, 0, 1, esdr_tx_wait_fn);

static mp_obj_t esdr_tx_stats_fn(void)
{
	struct esdr_tx_stats s;
	mp_obj_t d = mp_obj_new_dict(18);

	esdr_tx_get_stats(&s);
#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(fed, mp_obj_new_int_from_ull(s.fed));
	PUT(played, mp_obj_new_int_from_ull(s.played));
	PUT(dropped, mp_obj_new_int_from_ull(s.dropped));
	PUT(underruns, mp_obj_new_int_from_uint(s.underruns));
	PUT(restarts, mp_obj_new_int_from_uint(s.restarts));
	PUT(queued, mp_obj_new_int_from_uint(s.queued));
	PUT(last_error, mp_obj_new_int(s.last_error));
	PUT(errors, mp_obj_new_int_from_uint(s.errors));
	PUT(switches, mp_obj_new_int_from_uint(s.switches));
	PUT(fill_us_max, mp_obj_new_int_from_uint(s.fill_us_max));
	PUT(wake_late_us_max, mp_obj_new_int_from_uint(s.wake_late_us_max));
	PUT(idle_us_max, mp_obj_new_int_from_uint(s.idle_us_max));
	PUT(switch_us_max, mp_obj_new_int_from_uint(s.switch_us_max));
	PUT(slack_us_min, mp_obj_new_int(s.slack_us_min));
	PUT(start_margin_us, mp_obj_new_int(s.start_margin_us));
	PUT(simd, mp_obj_new_bool(s.simd));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_stats_obj, esdr_tx_stats_fn);

static mp_obj_t esdr_tx_gain_max_fn(void)
{
	return mp_obj_new_int(esp_sdr_tx_gain_max());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_gain_max_obj, esdr_tx_gain_max_fn);

/* fc32_to_sc16(src, dst, scale): float32 pairs times scale to clipped int16 pairs, as many as
 * both hold; returns that count of samples. */
static mp_obj_t esdr_fc32_to_sc16(mp_obj_t src_in, mp_obj_t dst_in, mp_obj_t scale_in)
{
	mp_buffer_info_t src, dst;
	float scale = (float)mp_obj_get_float(scale_in);

	mp_get_buffer_raise(src_in, &src, MP_BUFFER_READ);
	mp_get_buffer_raise(dst_in, &dst, MP_BUFFER_WRITE);
	size_t n = MIN(src.len / 4U, dst.len / 2U) & ~1U;
	const float *s = src.buf;
	int16_t *o = dst.buf;

	if ((((uintptr_t)s) & 3U) != 0U || (((uintptr_t)o) & 1U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("unaligned buffer"));
	}
	for (size_t k = 0; k < n; k++) {
		float v = s[k] * scale;

		o[k] = (int16_t)(v > 32767.0f ? 32767 : (v < -32767.0f ? -32767 : (int)v));
	}
	return mp_obj_new_int_from_uint(n / 2U);
}
static MP_DEFINE_CONST_FUN_OBJ_3(esdr_fc32_to_sc16_obj, esdr_fc32_to_sc16);

/* sc16_to_fc32(src, dst, scale): as many pairs as both hold; returns that count. */
static mp_obj_t esdr_sc16_to_fc32(mp_obj_t src_in, mp_obj_t dst_in, mp_obj_t scale_in)
{
	mp_buffer_info_t src, dst;
	float scale = (float)mp_obj_get_float(scale_in);

	mp_get_buffer_raise(src_in, &src, MP_BUFFER_READ);
	mp_get_buffer_raise(dst_in, &dst, MP_BUFFER_WRITE);
	size_t n = MIN(src.len / 2U, dst.len / 4U);
	const int16_t *s = src.buf;
	float *o = dst.buf;

	if ((((uintptr_t)s) & 1U) != 0U || (((uintptr_t)o) & 3U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("unaligned buffer"));
	}
	for (size_t k = 0; k < n; k++) {
		o[k] = (float)s[k] * scale;
	}
	return mp_obj_new_int_from_uint(n / 2U);
}
static MP_DEFINE_CONST_FUN_OBJ_3(esdr_sc16_to_fc32_obj, esdr_sc16_to_fc32);

/* ---- board ---- */

/* claim(mnt), release(mnt): take a volume ("/FLASH:", "/SD:") from the USB host before
 * writing it, give it back once the files are closed (src/usb_disks.h). */
static mp_obj_t esdr_claim(mp_obj_t mnt_in)
{
	check(usb_disk_claim(mp_obj_str_get_str(mnt_in)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_claim_obj, esdr_claim);

static mp_obj_t esdr_release(mp_obj_t mnt_in)
{
	check(usb_disk_release(mp_obj_str_get_str(mnt_in)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_release_obj, esdr_release);

/* sd_info() -> dict: what the SD stack negotiated with the card (diagnostics). The sdmmc disk
 * driver's data starts with its struct sd_card (drivers/disk/sdmmc_subsys.c). */
#define SD_DISK_NODE DT_CHILD(DT_NODELABEL(sd_spi), mmc)

static mp_obj_t esdr_sd_info(void)
{
	const struct device *dev = DEVICE_DT_GET(SD_DISK_NODE);
	const struct sd_card *c = dev->data;
	mp_obj_t d = mp_obj_new_dict(10);

#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(high_capacity, mp_obj_new_bool((c->flags & SD_HIGH_CAPACITY_FLAG) != 0U));
	PUT(type, mp_obj_new_int(c->type));
	PUT(status, mp_obj_new_int(c->status));
	PUT(sd_version, mp_obj_new_int(c->sd_version));
	PUT(card_speed, mp_obj_new_int(c->card_speed));
	PUT(ocr, mp_obj_new_int_from_uint(c->ocr));
	PUT(block_size, mp_obj_new_int(c->block_size));
	PUT(block_count, mp_obj_new_int_from_uint(c->block_count));
	PUT(clock_hz, mp_obj_new_int_from_uint(c->bus_io.clock));
	PUT(flags, mp_obj_new_int(c->flags));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_sd_info_obj, esdr_sd_info);

/* reset_reason() -> str: why the chip last started (esp_reset_reason()). Catches what crash()
 * cannot: power-on, brownout, the hardware watchdogs, a USB or JTAG reset. */
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

/* crash() -> dict or None: the fatal error, watchdog reset or double exception that ended
 * the previous boot (apps/common app_crash.h; the console on USB loses the dump itself). */
static mp_obj_t esdr_crash(void)
{
	struct app_crash c;
	struct app_crash_dx dx;
	mp_obj_t d;

#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	if (app_crash_dx_last(&dx)) {
		d = mp_obj_new_dict(8);
		PUT(kind, MP_OBJ_NEW_QSTR(MP_QSTR_double_exception));
		PUT(depc, mp_obj_new_int_from_uint(dx.depc));
		PUT(exccause, mp_obj_new_int_from_uint(dx.exccause));
		PUT(excvaddr, mp_obj_new_int_from_uint(dx.excvaddr));
		PUT(epc1, mp_obj_new_int_from_uint(dx.epc1));
		PUT(ps, mp_obj_new_int_from_uint(dx.ps));
		PUT(sp, mp_obj_new_int_from_uint(dx.a1));
		PUT(cpu, mp_obj_new_int_from_uint(dx.prid));
		return d;
	}
	if (!app_crash_last(&c)) {
		return mp_const_none;
	}
	d = mp_obj_new_dict(10);
	PUT(kind, c.reason == APP_CRASH_WATCHDOG ? MP_OBJ_NEW_QSTR(MP_QSTR_watchdog)
						  : MP_OBJ_NEW_QSTR(MP_QSTR_fatal));
	PUT(reason, mp_obj_new_int_from_uint(c.reason));
	PUT(pc, mp_obj_new_int_from_uint(c.pc));
	PUT(a0, mp_obj_new_int_from_uint(c.a0));
	PUT(ps, mp_obj_new_int_from_uint(c.ps));
	PUT(exccause, mp_obj_new_int_from_uint(c.exccause));
	PUT(excvaddr, mp_obj_new_int_from_uint(c.excvaddr));
	PUT(cpu, mp_obj_new_int_from_uint(c.cpu));
	PUT(uptime_ms, mp_obj_new_int_from_uint(c.uptime_ms));
	PUT(thread, mp_obj_new_str(c.thread, strnlen(c.thread, sizeof(c.thread))));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_crash_obj, esdr_crash);


/* shell(cmd) -> str: run a shell command on the dummy backend, return what it printed. The
 * port's zephyr.shell_exec() needs the serial backend, which the REPL's console cannot share. */
static mp_obj_t esdr_shell(mp_obj_t cmd_in)
{
	const char *cmd = mp_obj_str_get_str(cmd_in);
	const struct shell *sh = shell_backend_dummy_get_ptr();
	size_t len = 0;
	const char *out;
	int ret;

	if (sh == NULL) {
		mp_raise_OSError(ENODEV);
	}
	shell_backend_dummy_clear_output(sh);
	ret = shell_execute_cmd(sh, cmd);
	out = shell_backend_dummy_get_output(sh, &len);
	if (ret < 0 && len == 0) {
		mp_raise_OSError(-ret);
	}
	return mp_obj_new_str(out, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_shell_obj, esdr_shell);

/* next_app([path]) -> str: with a path, the app main.py starts after the next soft reset;
 * without, that app ("" for none), forgotten. */
static mp_obj_t esdr_next_app(size_t n_args, const mp_obj_t *args)
{
	char out[sizeof(next_app)];
	const char *path;
	size_t len;

	if (n_args == 0) {
		/* Forgotten first: whatever the string does, the next soft reset starts the menu. */
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

/* home() -> bool: the knob was held since the last call, recently enough that the Ctrl-C
 * which ended the app was the knob's: "back to the launcher". */
static mp_obj_t esdr_home(void)
{
	bool held = atomic_clear(&knob_home) != 0;

	return mp_obj_new_bool(held && k_uptime_get() - knob_time < KNOB_FRESH_MS);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_home_obj, esdr_home);

/* esdr_link.c */
extern const mp_obj_fun_builtin_fixed_t esdr_fsk_rx_reset_obj, esdr_fsk_rx_state_obj;
extern const mp_obj_fun_builtin_var_t esdr_fsk_rx_init_obj, esdr_fsk_rx_obj, esdr_fsk_mod_obj;

/* esdr_gui.c */
extern const mp_obj_fun_builtin_fixed_t esdr_rx_latest_obj, esdr_iq_cols_obj;
extern const mp_obj_fun_builtin_var_t esdr_psd_obj, esdr_fb_trace_obj, esdr_fb_wf_row_obj,
	esdr_fb_scroll_obj, esdr_fb_points_obj;

static const mp_rom_obj_tuple_t esdr_burst_rates_obj = {
	{&mp_type_tuple},
	3,
	{
		MP_ROM_INT(80000000),
		MP_ROM_INT(40000000),
		MP_ROM_INT(16000000),
	},
};

static const mp_rom_obj_tuple_t esdr_rates_obj = {
	{&mp_type_tuple},
	5,
	{
		MP_ROM_INT(ESP_SDR_RING_RATE_HZ / 64U),
		MP_ROM_INT(ESP_SDR_RING_RATE_HZ / 128U),
		MP_ROM_INT(ESP_SDR_RING_RATE_HZ / 256U),
		MP_ROM_INT(ESP_SDR_RING_RATE_HZ / 512U),
		MP_ROM_INT(ESP_SDR_RING_RATE_HZ / 1024U),
	},
};

static const mp_rom_map_elem_t esdr_globals_table[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esdr)},
	{MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esdr_init_obj)},
	{MP_ROM_QSTR(MP_QSTR_tune), MP_ROM_PTR(&esdr_tune_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_start), MP_ROM_PTR(&esdr_tx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_gen), MP_ROM_PTR(&esdr_tx_gen_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_loop), MP_ROM_PTR(&esdr_tx_loop_obj)},
	{MP_ROM_QSTR(MP_QSTR_channel_bw), MP_ROM_PTR(&esdr_channel_bw_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_lpf), MP_ROM_PTR(&esdr_tx_lpf_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_period), MP_ROM_PTR(&esdr_tx_period_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_scale), MP_ROM_PTR(&esdr_tx_scale_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_stop), MP_ROM_PTR(&esdr_tx_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_busy), MP_ROM_PTR(&esdr_tx_busy_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_wait), MP_ROM_PTR(&esdr_tx_wait_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_stats), MP_ROM_PTR(&esdr_tx_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_gain_max), MP_ROM_PTR(&esdr_tx_gain_max_obj)},
	{MP_ROM_QSTR(MP_QSTR_fc32_to_sc16), MP_ROM_PTR(&esdr_fc32_to_sc16_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain), MP_ROM_PTR(&esdr_rx_gain_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain_max), MP_ROM_PTR(&esdr_rx_gain_max_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_bandwidth), MP_ROM_PTR(&esdr_rx_bandwidth_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_start), MP_ROM_PTR(&esdr_rx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stop), MP_ROM_PTR(&esdr_rx_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_busy), MP_ROM_PTR(&esdr_rx_busy_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_count), MP_ROM_PTR(&esdr_rx_count_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx_init), MP_ROM_PTR(&esdr_fsk_rx_init_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx), MP_ROM_PTR(&esdr_fsk_rx_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx_reset), MP_ROM_PTR(&esdr_fsk_rx_reset_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_rx_state), MP_ROM_PTR(&esdr_fsk_rx_state_obj)},
	{MP_ROM_QSTR(MP_QSTR_fsk_mod), MP_ROM_PTR(&esdr_fsk_mod_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_wait), MP_ROM_PTR(&esdr_rx_wait_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stats), MP_ROM_PTR(&esdr_rx_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_sc16_to_fc32), MP_ROM_PTR(&esdr_sc16_to_fc32_obj)},
	{MP_ROM_QSTR(MP_QSTR_shell), MP_ROM_PTR(&esdr_shell_obj)},
	{MP_ROM_QSTR(MP_QSTR_crash), MP_ROM_PTR(&esdr_crash_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&esdr_reset_reason_obj)},
	{MP_ROM_QSTR(MP_QSTR_next_app), MP_ROM_PTR(&esdr_next_app_obj)},
	{MP_ROM_QSTR(MP_QSTR_home), MP_ROM_PTR(&esdr_home_obj)},
	{MP_ROM_QSTR(MP_QSTR_sd_info), MP_ROM_PTR(&esdr_sd_info_obj)},
	{MP_ROM_QSTR(MP_QSTR_claim), MP_ROM_PTR(&esdr_claim_obj)},
	{MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&esdr_release_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_latest), MP_ROM_PTR(&esdr_rx_latest_obj)},
	{MP_ROM_QSTR(MP_QSTR_psd), MP_ROM_PTR(&esdr_psd_obj)},
	{MP_ROM_QSTR(MP_QSTR_iq_cols), MP_ROM_PTR(&esdr_iq_cols_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_trace), MP_ROM_PTR(&esdr_fb_trace_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_wf_row), MP_ROM_PTR(&esdr_fb_wf_row_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_scroll), MP_ROM_PTR(&esdr_fb_scroll_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_points), MP_ROM_PTR(&esdr_fb_points_obj)},
	{MP_ROM_QSTR(MP_QSTR_RATES), MP_ROM_PTR(&esdr_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_BURST_RATES), MP_ROM_PTR(&esdr_burst_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_FULL_SCALE), MP_ROM_INT(ESP_SDR_RING_FULL_SCALE)},
	{MP_ROM_QSTR(MP_QSTR_MON_SAMPLES), MP_ROM_INT(ESDR_MON_SAMPLES)},
};
static MP_DEFINE_CONST_DICT(esdr_globals, esdr_globals_table);

const mp_obj_module_t esdr_module = {
	.base = {&mp_type_module},
	.globals = (mp_obj_dict_t *)&esdr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esdr, esdr_module);
