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
 *   RATES                      the output rates, 16 MS/s / 64 .. 1024
 *   rx_start(rate, buf=None, count=0, offset=True, monitor=False)
 *                              capture into buf (int16 I, Q interleaved) or only count;
 *                              count 0: until rx_stop() or the buffer is full; monitor
 *                              keeps the latest samples for rx_latest() (displays)
 *   rx_wait([timeout_ms]) -> done, rx_stop(), rx_busy(), rx_stats() -> dict
 *   sc16_to_fc32(src, dst, scale)  int16 pairs to float32 pairs times scale
 *   shell(cmd) -> str          a Zephyr shell command (usj, mcuboot confirm, ...)
 *   claim(mnt), release(mnt)   take a volume ("/FLASH:", "/SD:") from the USB host for
 *                              writing, give it back (src/usb_disks.h)
 *   sd_info() -> dict           what the SD stack negotiated with the card
 *   reset_reason() -> str      why the chip last started (power-on, brownout, watchdogs)
 *   crash() -> dict or None    what ended the previous boot (fatal error, watchdog, double
 *                              exception)
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>

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

#include "app_crash.h"
#include "usb_disks.h"
#include "esdr_rx.h"

#define OFFSET_HZ (ESP_SDR_RING_RATE_HZ / 4U)

/* The capture buffer, kept alive while the engine writes into it. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_rx_buf);

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
}

void esdr_port_deinit(void)
{
	/* The heap goes next: nothing may write into it any more. */
	esdr_rx_stop();
	(void)esdr_rx_wait(-1);
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
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
	if (esdr_rx_busy()) {
		mp_raise_OSError(EBUSY);
	}
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
	enum { ARG_rate, ARG_buf, ARG_count, ARG_offset, ARG_monitor };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_count, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
		{MP_QSTR_monitor, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	int16_t *buf = NULL;
	size_t cap = 0;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	unsigned int decim = rate_decim(args[ARG_rate].u_obj);

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
	check(esdr_rx_start(decim, args[ARG_offset].u_bool, buf, cap,
			    (uint64_t)args[ARG_count].u_int, args[ARG_monitor].u_bool));
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
	mp_obj_t d = mp_obj_new_dict(8);

	esdr_rx_get_stats(&s);
#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(samples, mp_obj_new_int_from_ull(s.samples));
	PUT(runs, mp_obj_new_int_from_uint(s.runs));
	PUT(errors, mp_obj_new_int_from_uint(s.errors));
	PUT(gaps, mp_obj_new_int_from_uint(s.gaps));
	PUT(gap_samples, mp_obj_new_int_from_ull(s.gap_samples));
	PUT(last_status, mp_obj_new_int(s.last_status));
	PUT(last_detail, mp_obj_new_int_from_uint(s.last_detail));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stats_obj, esdr_rx_stats_fn);

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

/* esdr_gui.c */
extern const mp_obj_fun_builtin_fixed_t esdr_rx_latest_obj, esdr_iq_cols_obj;
extern const mp_obj_fun_builtin_var_t esdr_psd_obj, esdr_fb_trace_obj, esdr_fb_wf_row_obj,
	esdr_fb_scroll_obj;

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
	{MP_ROM_QSTR(MP_QSTR_rx_gain), MP_ROM_PTR(&esdr_rx_gain_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain_max), MP_ROM_PTR(&esdr_rx_gain_max_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_bandwidth), MP_ROM_PTR(&esdr_rx_bandwidth_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_start), MP_ROM_PTR(&esdr_rx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stop), MP_ROM_PTR(&esdr_rx_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_busy), MP_ROM_PTR(&esdr_rx_busy_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_wait), MP_ROM_PTR(&esdr_rx_wait_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stats), MP_ROM_PTR(&esdr_rx_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_sc16_to_fc32), MP_ROM_PTR(&esdr_sc16_to_fc32_obj)},
	{MP_ROM_QSTR(MP_QSTR_shell), MP_ROM_PTR(&esdr_shell_obj)},
	{MP_ROM_QSTR(MP_QSTR_crash), MP_ROM_PTR(&esdr_crash_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&esdr_reset_reason_obj)},
	{MP_ROM_QSTR(MP_QSTR_sd_info), MP_ROM_PTR(&esdr_sd_info_obj)},
	{MP_ROM_QSTR(MP_QSTR_claim), MP_ROM_PTR(&esdr_claim_obj)},
	{MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&esdr_release_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_latest), MP_ROM_PTR(&esdr_rx_latest_obj)},
	{MP_ROM_QSTR(MP_QSTR_psd), MP_ROM_PTR(&esdr_psd_obj)},
	{MP_ROM_QSTR(MP_QSTR_iq_cols), MP_ROM_PTR(&esdr_iq_cols_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_trace), MP_ROM_PTR(&esdr_fb_trace_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_wf_row), MP_ROM_PTR(&esdr_fb_wf_row_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_scroll), MP_ROM_PTR(&esdr_fb_scroll_obj)},
	{MP_ROM_QSTR(MP_QSTR_RATES), MP_ROM_PTR(&esdr_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_FULL_SCALE), MP_ROM_INT(ESP_SDR_RING_FULL_SCALE)},
	{MP_ROM_QSTR(MP_QSTR_MON_SAMPLES), MP_ROM_INT(ESDR_MON_SAMPLES)},
};
static MP_DEFINE_CONST_DICT(esdr_globals, esdr_globals_table);

const mp_obj_module_t esdr_module = {
	.base = {&mp_type_module},
	.globals = (mp_obj_dict_t *)&esdr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esdr, esdr_module);
