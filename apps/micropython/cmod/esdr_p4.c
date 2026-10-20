/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module on the ESP32-P4 of the WHY2025 badge, as modesdr.c is on the S3. The P4 has
 * no radio of its own: the badge's ESP32-C6 is its radio, running apps/osmosdr_serial behind
 * a link (esdr_remote.c). CPU-paced on the C6: no offset tuning, no bursts, 8-bit samples.
 *
 *   init()                     opens the link, reads the radio's description
 *   tune(hz, offset=True)      LO at hz (offset is ignored); returns the centre tuned
 *   rx_gain([index]), rx_gain_max(), rx_bandwidth(hz), channel_bw(mode) (ignored)
 *   radio_state() -> dict      the radio's settings as it reports them (mode, freq, centre,
 *                              rx_gain, tx_gain, rate, bits, bandwidth); after an app ends
 *                              they are the radio's start settings again
 *   RATES, BURST_RATES (80, 40 MS/s: bursts of up to 16380 samples at 80 MS/s, contiguous
 *                              within a burst only, full scale 16384 as on the S3),
 *                              FULL_SCALE (streams: 128), MON_SAMPLES, TX_PREBUF_S
 *   rx_start(rate, buf=None, count=0, offset=True, monitor=False, wrap=False)
 *                              capture into buf (int16 I, Q; wrap: a ring), count samples
 *                              (0: endless); the monitor (rx_latest()) sees every sample
 *   rx_stop(), rx_busy(), rx_wait([ms]), rx_count(), rx_stats()
 *   tx_start(rate, hz, gain, buf=None, count=0, scale=1.0, gen=None, repeat=False) -> centre
 *                              play buf (int16 I, Q) once or repeat, or the generator gen
 *                              (wave, cycles, amp, dc_i, dc_q); count samples then stop
 *   tx_gen(*gen), tx_scale(scale)  change what plays
 *   tx_stop(), tx_busy(), tx_wait([ms]), tx_stats(), tx_gain_max(), TX_RATE_MAX (bursts),
 *                              TX_STREAM_MAX (gapless streams)
 *   trig_start(ratio=8, post=TRIG_SAMPLES // 2), trig_stop()
 *                              triggered snapshots: TRIG_SAMPLES at TRIG_RATE (80 MS/s) around
 *                              each burst ratio times the quiet power, post of them after it
 *   trig_get(buf, after=0) -> (seq, trig) or None
 *                              the newest snapshot (int8 I, Q, 2 * TRIG_SAMPLES bytes) unless
 *                              its seq is after; trig: samples before the trigger
 *   trig_stats()               snapshots, broken (a block lost)
 *   channelize(snap, nfft, mean=None, peak=None, image=None, lo=-100, hi=-20) -> frames
 *                              (esdr_chan.c)
 *   fb_image(fb, stride, x, y, w, h, image, cols, palette)   draws channelize()'s image
 *   stretch(src, dst)          src's floats over dst's entries (screen columns)
 *   fsk_rx_init(), fsk_rx(), fsk_rx_reset(), fsk_rx_state(), fsk_mod()   (esdr_link.c)
 *   psd(), iq_cols(), fb_trace(), fb_wf_row(), fb_scroll(), fb_points(), rx_latest()
 *                              (esdr_gui.c)
 *   fc32_to_sc16(), sc16_to_fc32(), display_size(), reset_reason(), next_app([path]), home(),
 *   shift()                    (esdr_board.c)
 *   claim(mnt), release(mnt)   no-ops: the volumes are not USB drives here
 *   shell(cmd) -> str          a Zephyr shell command (mcuboot confirm, ...)
 *   usb_to_c6(seconds=60)      USB-C port J8 to the ESP32-C6's USB-Serial-JTAG (esptool) for
 *                              that long, then back to this chip's REPL and DFU (esdr_usb.c)
 *
 * Memory: the Python heap is CONFIG_APP_MP_PSRAM_HEAP_KB of PSRAM, added at every start; LVGL
 * allocates from it too (CONFIG_LV_Z_MEM_POOL_MICROPYTHON).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#ifdef CONFIG_LVGL
#include <lvgl.h>
#endif

#include "py/gc.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_board.h"
#include "esdr_remote.h"
#include "esdr_usb.h"
#include "esdr_rx.h"

/* 8-bit samples from the C6, carried as int16. */
#define FULL_SCALE 128
#define RATE_MAX_HZ 250000
/* Receive: over SDIO the C6 offers up to 2 MS/s (8-bit items above 250 kS/s). */
#if DT_NODE_EXISTS(DT_CHOSEN(esdr_link_sdio))
#define RX_RATE_MAX_HZ 2000000
#define RX_RATE_COUNT 8
#else
#define RX_RATE_MAX_HZ RATE_MAX_HZ
#define RX_RATE_COUNT 5
#endif
/* Gapless streams: the 5 Mbaud link carried some 194 kS/s of the 250 kS/s asked (bursts fit
 * the radio's ring and play from its prebuffer). */
#define STREAM_MAX_HZ 125000
/* The radio's native burst rate (ESDR_REQ_CAPTURE_BURST). */
#define BURST_HZ 80000000

/* The buffers the link reads or writes, kept alive meanwhile. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_rx_buf);
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_tx_buf);

/* PSRAM (the P4's linker script places .ext_ram.bss there; esp_attr.h's EXT_RAM_BSS_ATTR is
 * empty on the P4). */
static uint8_t psram_heap[CONFIG_APP_MP_PSRAM_HEAP_KB * 1024]
	__attribute__((section(".ext_ram.bss"))) __aligned(16);

static const struct esdr_info *radio;

static void check(int ret)
{
	if (ret < 0) {
		mp_raise_OSError(-ret);
	}
}

static void need_radio(void)
{
	if (radio == NULL) {
		check(esdr_remote_init(&radio));
	}
}

/* ---- radio settings ---- */

static mp_obj_t esdr_init(void)
{
	need_radio();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_init_obj, esdr_init);

static mp_obj_t esdr_tune(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	static const mp_arg_t allowed[] = {
		{MP_QSTR_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	struct esdr_state st;
	uint8_t le[8];

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	need_radio();
	sys_put_le64((uint64_t)mp_obj_get_float(args[0].u_obj), le);
	check(esdr_remote_request(ESDR_REQ_SET_FREQ, 0, le, sizeof(le), NULL, 0));
	check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
	return mp_obj_new_float((mp_float_t)sys_le64_to_cpu(st.center_hz));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tune_obj, 1, esdr_tune);

/* radio_state() -> dict: the radio's settings (struct esdr_state), as it reports them. */
static mp_obj_t esdr_radio_state_fn(void)
{
	struct esdr_state st;
	mp_obj_t d = mp_obj_new_dict(8);

	need_radio();
	check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(mode, MP_OBJ_NEW_SMALL_INT(st.mode));
	PUT(bits, MP_OBJ_NEW_SMALL_INT(st.bits));
	PUT(rx_gain, MP_OBJ_NEW_SMALL_INT(st.rx_gain == ESDR_GAIN_AUTO ? -1 : st.rx_gain));
	PUT(tx_gain, MP_OBJ_NEW_SMALL_INT(st.tx_gain));
	PUT(freq, mp_obj_new_int_from_ull(st.freq_hz));
	PUT(centre, mp_obj_new_int_from_ull(st.center_hz));
	PUT(rate, mp_obj_new_int_from_uint(st.sample_rate_hz));
	PUT(bandwidth, mp_obj_new_int_from_uint(st.bandwidth_hz));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_radio_state_obj, esdr_radio_state_fn);

static mp_obj_t esdr_rx_gain(size_t n_args, const mp_obj_t *args)
{
	need_radio();
	if (n_args == 0) {
		struct esdr_state st;

		check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
		return MP_OBJ_NEW_SMALL_INT(st.rx_gain == ESDR_GAIN_AUTO ? -1 : st.rx_gain);
	}
	mp_int_t g = mp_obj_get_int(args[0]);

	check(esdr_remote_request(ESDR_REQ_SET_RX_GAIN, g < 0 ? ESDR_GAIN_AUTO : (uint16_t)g,
				  NULL, 0, NULL, 0));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_rx_gain_obj, 0, 1, esdr_rx_gain);

static mp_obj_t esdr_rx_gain_max(void)
{
	need_radio();
	return MP_OBJ_NEW_SMALL_INT(radio->rx_gain_max);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_gain_max_obj, esdr_rx_gain_max);

static mp_obj_t esdr_tx_gain_max(void)
{
	need_radio();
	return MP_OBJ_NEW_SMALL_INT(radio->tx_gain_max);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_gain_max_obj, esdr_tx_gain_max);

static mp_obj_t esdr_rx_bandwidth(mp_obj_t hz_in)
{
	uint8_t le[4];

	need_radio();
	sys_put_le32((uint32_t)mp_obj_get_int(hz_in), le);
	check(esdr_remote_request(ESDR_REQ_SET_BANDWIDTH, 0, le, sizeof(le), NULL, 0));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_rx_bandwidth_obj, esdr_rx_bandwidth);

/* The C6's analog filters follow its own settings: nothing to switch. */
static mp_obj_t esdr_channel_bw(mp_obj_t mode_in)
{
	ARG_UNUSED(mode_in);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_channel_bw_obj, esdr_channel_bw);

/* ---- receive ---- */

static mp_obj_t esdr_rx_start_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_rate, ARG_buf, ARG_count, ARG_offset, ARG_monitor, ARG_wrap, ARG_fsk };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_count, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true}},
		{MP_QSTR_monitor, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_wrap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_fsk, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	mp_buffer_info_t bi = {0};
	int16_t *buf = NULL;
	size_t cap = 0;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	if (args[ARG_fsk].u_obj != mp_const_none) {
		mp_raise_ValueError(MP_ERROR_TEXT("no FSK receiver in the radio"));
	}
	mp_int_t rate = args[ARG_rate].u_int;
	bool burst = rate == BURST_HZ || rate == BURST_HZ / 2;

	if (!burst && (rate <= 0 || rate > RX_RATE_MAX_HZ)) {
		mp_raise_ValueError(MP_ERROR_TEXT("rate: one of RATES or BURST_RATES"));
	}
	if (burst && args[ARG_wrap].u_bool) {
		mp_raise_ValueError(MP_ERROR_TEXT("bursts into a ring: no"));
	}
	need_radio();
	if (args[ARG_buf].u_obj != mp_const_none) {
		mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_WRITE);
		if ((((uintptr_t)bi.buf) & 3U) != 0U || bi.len < 4U) {
			mp_raise_ValueError(MP_ERROR_TEXT("buf: aligned, at least one sample"));
		}
		buf = bi.buf;
		cap = bi.len / 4U;
	}
	MP_STATE_PORT(esdr_rx_buf) = args[ARG_buf].u_obj;
	if (burst) {
		check(esdr_remote_rx_start_burst(BURST_HZ / 1000000, BURST_HZ / rate, buf, cap,
						 (uint64_t)args[ARG_count].u_int));
		return mp_const_none;
	}
	check(esdr_remote_rx_start((uint32_t)rate, buf, cap, (uint64_t)args[ARG_count].u_int,
				   args[ARG_wrap].u_bool));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_rx_start_obj, 1, esdr_rx_start_fn);

static mp_obj_t esdr_rx_stop_fn(void)
{
	esdr_remote_rx_stop();
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stop_obj, esdr_rx_stop_fn);

static mp_obj_t esdr_rx_busy_fn(void)
{
	return mp_obj_new_bool(esdr_remote_rx_busy());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_busy_obj, esdr_rx_busy_fn);

static mp_obj_t esdr_rx_count_fn(void)
{
	return mp_obj_new_int_from_ull(esdr_remote_rx_count());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_count_obj, esdr_rx_count_fn);

/* Waits for a counted capture to end, up to ms (default: as long as it takes); Ctrl-C stops
 * it. True once it has ended. */
static mp_obj_t esdr_rx_wait_fn(size_t n_args, const mp_obj_t *args)
{
	mp_int_t timeout = n_args > 0 ? mp_obj_get_int(args[0]) : -1;
	mp_uint_t t0 = mp_hal_ticks_ms();
	nlr_buf_t nlr;

	if (nlr_push(&nlr) == 0) {
		while (esdr_remote_rx_busy() &&
		       (timeout < 0 || mp_hal_ticks_ms() - t0 < (mp_uint_t)timeout)) {
			mp_event_wait_ms(10);
		}
		nlr_pop();
	} else {
		esdr_remote_rx_stop();
		MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
		nlr_jump(nlr.ret_val);
	}
	if (!esdr_remote_rx_busy()) {
		MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
		return mp_const_true;
	}
	return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_rx_wait_obj, 0, 1, esdr_rx_wait_fn);

static mp_obj_t esdr_rx_stats_fn(void)
{
	struct esdr_remote_rx_stats st;
	mp_obj_t d = mp_obj_new_dict(5);

	esdr_remote_rx_stats(&st);
#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(samples, mp_obj_new_int_from_ull(st.samples));
	PUT(gaps, mp_obj_new_int_from_uint(st.gaps));
	PUT(gap_samples, mp_obj_new_int_from_ull(st.gap_samples));
	PUT(overflows, mp_obj_new_int_from_uint(st.overflows));
	PUT(resyncs, mp_obj_new_int_from_uint(st.resyncs));
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_rx_stats_obj, esdr_rx_stats_fn);

/* ---- triggered snapshots ---- */

static mp_obj_t esdr_trig_start_fn(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
	enum { ARG_ratio, ARG_post };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_ratio, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_post, MP_ARG_INT, {.u_int = ESDR_TRIG_SAMPLES / 2}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	float ratio = 8.0f;

	mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, args);
	if (args[ARG_ratio].u_obj != MP_OBJ_NULL) {
		ratio = (float)mp_obj_get_float(args[ARG_ratio].u_obj);
	}
	if (!(ratio > 1.0625f) || ratio > 4000.0f || args[ARG_post].u_int <= 0 ||
	    args[ARG_post].u_int >= (mp_int_t)ESDR_TRIG_SAMPLES) {
		mp_raise_ValueError(MP_ERROR_TEXT("trig_start: ratio above 1, 0 < post < TRIG_SAMPLES"));
	}
	check(esdr_remote_trig_start((uint16_t)(ratio * 16.0f + 0.5f),
				     (uint32_t)args[ARG_post].u_int));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_trig_start_obj, 0, esdr_trig_start_fn);

static mp_obj_t esdr_trig_stop_fn(void)
{
	esdr_remote_trig_stop();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_trig_stop_obj, esdr_trig_stop_fn);

static mp_obj_t esdr_trig_get_fn(size_t n_args, const mp_obj_t *args)
{
	mp_buffer_info_t bi;
	uint32_t after = n_args > 1 ? (uint32_t)mp_obj_get_int(args[1]) : 0U, seq, trig;

	mp_get_buffer_raise(args[0], &bi, MP_BUFFER_WRITE);
	if (bi.len < 2U * ESDR_TRIG_SAMPLES) {
		mp_raise_ValueError(MP_ERROR_TEXT("trig_get: buf of 2 * TRIG_SAMPLES bytes"));
	}
	if (!esdr_remote_trig_get(bi.buf, after, &seq, &trig)) {
		return mp_const_none;
	}
	mp_obj_t t[2] = {mp_obj_new_int_from_uint(seq), mp_obj_new_int_from_uint(trig)};

	return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_trig_get_obj, 1, 2, esdr_trig_get_fn);

static mp_obj_t esdr_trig_stats_fn(void)
{
	struct esdr_remote_trig_stats st;
	mp_obj_t d = mp_obj_new_dict(2);

	esdr_remote_trig_stats(&st);
	mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_snapshots),
			  mp_obj_new_int_from_uint(st.snapshots));
	mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_broken), mp_obj_new_int_from_uint(st.broken));
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_trig_stats_obj, esdr_trig_stats_fn);

/* ---- transmit ---- */

static void tx_gen_from(struct esdr_remote_gen *g, mp_obj_t t)
{
	mp_obj_t *items;
	size_t len;

	mp_obj_get_array(t, &len, &items);
	if (len != 5) {
		mp_raise_ValueError(MP_ERROR_TEXT("gen: (wave, cycles, amp, dc_i, dc_q)"));
	}
	mp_int_t wave = mp_obj_get_int(items[0]);

	/* gnuradio.analog's GR_CONST_WAVE .. GR_SAW_WAVE are 100 .. 105. */
	g->wave = (int)(wave >= 100 ? wave - 100 : wave);
	g->cycles = (float)mp_obj_get_float(items[1]);
	g->amp = (float)mp_obj_get_float(items[2]);
	g->dc_i = (float)mp_obj_get_float(items[3]);
	g->dc_q = (float)mp_obj_get_float(items[4]);
}

static mp_obj_t esdr_tx_start_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_rate, ARG_hz, ARG_gain, ARG_buf, ARG_count, ARG_scale, ARG_gen, ARG_repeat };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_rate, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_gain, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_buf, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_count, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_scale, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_gen, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_repeat, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	struct esdr_remote_gen gen;
	mp_buffer_info_t bi = {0};
	struct esdr_state st;
	uint8_t le[8];
	size_t n = 0;
	float scale = 1.0f;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
	if ((args[ARG_gen].u_obj == mp_const_none) == (args[ARG_buf].u_obj == mp_const_none)) {
		mp_raise_ValueError(MP_ERROR_TEXT("buf or gen"));
	}
	if (args[ARG_rate].u_int <= 0 || args[ARG_rate].u_int > RATE_MAX_HZ) {
		mp_raise_ValueError(MP_ERROR_TEXT("rate: up to TX_RATE_MAX"));
	}
	if (args[ARG_scale].u_obj != MP_OBJ_NULL) {
		scale = (float)mp_obj_get_float(args[ARG_scale].u_obj);
	}
	if (args[ARG_gen].u_obj != mp_const_none) {
		tx_gen_from(&gen, args[ARG_gen].u_obj);
	} else {
		mp_get_buffer_raise(args[ARG_buf].u_obj, &bi, MP_BUFFER_READ);
		if ((((uintptr_t)bi.buf) & 1U) != 0U || bi.len < 4U) {
			mp_raise_ValueError(MP_ERROR_TEXT("buf: aligned, at least one sample"));
		}
		n = bi.len / 4U;
	}
	need_radio();
	esdr_remote_tx_stop();
	sys_put_le64((uint64_t)mp_obj_get_float(args[ARG_hz].u_obj), le);
	check(esdr_remote_request(ESDR_REQ_SET_FREQ, 0, le, sizeof(le), NULL, 0));
	check(esdr_remote_request(ESDR_REQ_SET_TX_GAIN, (uint16_t)args[ARG_gain].u_int, NULL, 0,
				  NULL, 0));
	check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
	MP_STATE_PORT(esdr_tx_buf) = args[ARG_buf].u_obj;
	check(esdr_remote_tx_start((uint32_t)args[ARG_rate].u_int, bi.buf, n,
				   args[ARG_repeat].u_bool,
				   args[ARG_gen].u_obj != mp_const_none ? &gen : NULL,
				   (uint64_t)MAX(args[ARG_count].u_int, 0), scale));
	return mp_obj_new_float((mp_float_t)sys_le64_to_cpu(st.center_hz));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tx_start_obj, 3, esdr_tx_start_fn);

/* tx_gen(wave, cycles, amp, dc_i, dc_q): the generator changes while it plays. */
static mp_obj_t esdr_tx_gen_fn(size_t n_args, const mp_obj_t *args)
{
	struct esdr_remote_gen g;

	tx_gen_from(&g, mp_obj_new_tuple(n_args, args));
	esdr_remote_tx_set_gen(&g);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_tx_gen_obj, 5, 5, esdr_tx_gen_fn);

/* tx_scale(scale): the buffer's or generator's scale changes while it plays. */
static mp_obj_t esdr_tx_scale_fn(mp_obj_t scale_in)
{
	esdr_remote_tx_set_scale((float)mp_obj_get_float(scale_in));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_tx_scale_obj, esdr_tx_scale_fn);

static mp_obj_t esdr_tx_stop_fn(void)
{
	esdr_remote_tx_stop();
	MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_stop_obj, esdr_tx_stop_fn);

static mp_obj_t esdr_tx_busy_fn(void)
{
	return mp_obj_new_bool(esdr_remote_tx_busy());
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_busy_obj, esdr_tx_busy_fn);

static mp_obj_t esdr_tx_wait_fn(size_t n_args, const mp_obj_t *args)
{
	mp_int_t timeout = n_args > 0 ? mp_obj_get_int(args[0]) : -1;
	mp_uint_t t0 = mp_hal_ticks_ms();

	while (esdr_remote_tx_busy() &&
	       (timeout < 0 || mp_hal_ticks_ms() - t0 < (mp_uint_t)timeout)) {
		mp_event_wait_ms(10);
	}
	if (!esdr_remote_tx_busy()) {
		MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
		return mp_const_true;
	}
	return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_tx_wait_obj, 0, 1, esdr_tx_wait_fn);

static mp_obj_t esdr_tx_stats_fn(void)
{
	mp_obj_t d = mp_obj_new_dict(1);

	mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_played),
			  mp_obj_new_int_from_ull(esdr_remote_tx_played()));
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_tx_stats_obj, esdr_tx_stats_fn);

/* ---- board ---- */

static mp_obj_t esdr_claim(mp_obj_t mnt_in)
{
	ARG_UNUSED(mnt_in);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_claim_obj, esdr_claim);

#if DT_NODE_EXISTS(DT_NODELABEL(sd_fs))
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(sd_fs));

/* A card that does not mount is left alone, never formatted (FS_FATFS_MOUNT_MKFS formats the
 * other volumes on first use); before MicroPython mounts the volumes. */
static int esdr_sd_no_format(void)
{
	FS_FSTAB_ENTRY(DT_NODELABEL(sd_fs)).flags |= FS_MOUNT_FLAG_NO_FORMAT;
	return 0;
}
SYS_INIT(esdr_sd_no_format, APPLICATION, 0);
#endif

void esdr_port_init(void)
{
	gc_add(psram_heap, psram_heap + sizeof(psram_heap));
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
	esdr_board_init();
}

void esdr_port_deinit(void)
{
	/* The heap goes next: the link may not write into it any more. An app that ends leaves
	 * the radio as esdr.init() found it, so the next starts afresh.
	 */
	if (radio != NULL) {
		esdr_remote_reset();
	}
	MP_STATE_PORT(esdr_rx_buf) = MP_OBJ_NULL;
	MP_STATE_PORT(esdr_tx_buf) = MP_OBJ_NULL;
#ifdef CONFIG_LVGL
	/* LVGL's state was in the heap the port swept already: forget it (as in modesdr.c). */
	mp_lv_deinit_gc();
#endif
}

/* esdr_link.c */
extern const mp_obj_fun_builtin_fixed_t esdr_fsk_rx_reset_obj, esdr_fsk_rx_state_obj;
extern const mp_obj_fun_builtin_var_t esdr_fsk_rx_init_obj, esdr_fsk_rx_obj, esdr_fsk_mod_obj;

/* esdr_gui.c */
extern const mp_obj_fun_builtin_fixed_t esdr_rx_latest_obj, esdr_iq_cols_obj;
extern const mp_obj_fun_builtin_var_t esdr_psd_obj, esdr_fb_trace_obj, esdr_fb_wf_row_obj,
	esdr_fb_scroll_obj, esdr_fb_points_obj;
/* esdr_chan.c */
extern const mp_obj_fun_builtin_var_t esdr_channelize_obj, esdr_fb_image_obj;
extern const mp_obj_fun_builtin_fixed_t esdr_stretch_obj;

/* The C6's paced rates (apps/osmosdr_serial): its top rate over the link and halvings. */
static const mp_rom_obj_tuple_t esdr_rates_obj = {
	{&mp_type_tuple},
	RX_RATE_COUNT,
	{
		MP_ROM_INT(RX_RATE_MAX_HZ),
		MP_ROM_INT(RX_RATE_MAX_HZ / 2),
		MP_ROM_INT(RX_RATE_MAX_HZ / 4),
		MP_ROM_INT(RX_RATE_MAX_HZ / 8),
		MP_ROM_INT(RX_RATE_MAX_HZ / 16),
#if RX_RATE_COUNT > 5
		MP_ROM_INT(RX_RATE_MAX_HZ / 32),
		MP_ROM_INT(RX_RATE_MAX_HZ / 64),
		MP_ROM_INT(RX_RATE_MAX_HZ / 128),
#endif
	},
};
/* The C6 captures bursts at 80 MS/s; 40 MS/s is them halved on the P4. */
static const mp_rom_obj_tuple_t esdr_burst_rates_obj = {
	{&mp_type_tuple},
	2,
	{
		MP_ROM_INT(BURST_HZ),
		MP_ROM_INT(BURST_HZ / 2),
	},
};

/* usb_to_c6(seconds=60): J8 to the C6 for a while (esdr_usb.c). */
static mp_obj_t esdr_usb_to_c6_fn(size_t n_args, const mp_obj_t *args)
{
	mp_int_t s = n_args > 0 ? mp_obj_get_int(args[0]) : 60;
	int ret;

	if (s < 1 || s > 3600) {
		mp_raise_ValueError(MP_ERROR_TEXT("seconds: 1 to 3600"));
	}
	ret = esdr_usb_to_c6((uint32_t)s);
	if (ret != 0) {
		mp_raise_OSError(-ret);
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_usb_to_c6_obj, 0, 1, esdr_usb_to_c6_fn);

static const mp_rom_map_elem_t esdr_globals_table[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_esdr)},
	{MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&esdr_init_obj)},
	{MP_ROM_QSTR(MP_QSTR_tune), MP_ROM_PTR(&esdr_tune_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain), MP_ROM_PTR(&esdr_rx_gain_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_gain_max), MP_ROM_PTR(&esdr_rx_gain_max_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_bandwidth), MP_ROM_PTR(&esdr_rx_bandwidth_obj)},
	{MP_ROM_QSTR(MP_QSTR_channel_bw), MP_ROM_PTR(&esdr_channel_bw_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_start), MP_ROM_PTR(&esdr_rx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stop), MP_ROM_PTR(&esdr_rx_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_busy), MP_ROM_PTR(&esdr_rx_busy_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_wait), MP_ROM_PTR(&esdr_rx_wait_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_count), MP_ROM_PTR(&esdr_rx_count_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_stats), MP_ROM_PTR(&esdr_rx_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_radio_state), MP_ROM_PTR(&esdr_radio_state_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_start), MP_ROM_PTR(&esdr_tx_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_gen), MP_ROM_PTR(&esdr_tx_gen_obj)},
	{MP_ROM_QSTR(MP_QSTR_tx_scale), MP_ROM_PTR(&esdr_tx_scale_obj)},
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
	{MP_ROM_QSTR(MP_QSTR_fc32_to_sc16), MP_ROM_PTR(&esdr_fc32_to_sc16_obj)},
	{MP_ROM_QSTR(MP_QSTR_sc16_to_fc32), MP_ROM_PTR(&esdr_sc16_to_fc32_obj)},
	{MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&esdr_reset_reason_obj)},
	{MP_ROM_QSTR(MP_QSTR_shell), MP_ROM_PTR(&esdr_shell_obj)},
	{MP_ROM_QSTR(MP_QSTR_usb_to_c6), MP_ROM_PTR(&esdr_usb_to_c6_obj)},
	{MP_ROM_QSTR(MP_QSTR_next_app), MP_ROM_PTR(&esdr_next_app_obj)},
	{MP_ROM_QSTR(MP_QSTR_home), MP_ROM_PTR(&esdr_home_obj)},
	{MP_ROM_QSTR(MP_QSTR_shift), MP_ROM_PTR(&esdr_shift_obj)},
	{MP_ROM_QSTR(MP_QSTR_display_size), MP_ROM_PTR(&esdr_display_size_obj)},
	{MP_ROM_QSTR(MP_QSTR_claim), MP_ROM_PTR(&esdr_claim_obj)},
	{MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&esdr_claim_obj)},
	{MP_ROM_QSTR(MP_QSTR_rx_latest), MP_ROM_PTR(&esdr_rx_latest_obj)},
	{MP_ROM_QSTR(MP_QSTR_psd), MP_ROM_PTR(&esdr_psd_obj)},
	{MP_ROM_QSTR(MP_QSTR_channelize), MP_ROM_PTR(&esdr_channelize_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_image), MP_ROM_PTR(&esdr_fb_image_obj)},
	{MP_ROM_QSTR(MP_QSTR_stretch), MP_ROM_PTR(&esdr_stretch_obj)},
	{MP_ROM_QSTR(MP_QSTR_trig_start), MP_ROM_PTR(&esdr_trig_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_trig_stop), MP_ROM_PTR(&esdr_trig_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_trig_get), MP_ROM_PTR(&esdr_trig_get_obj)},
	{MP_ROM_QSTR(MP_QSTR_trig_stats), MP_ROM_PTR(&esdr_trig_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_TRIG_SAMPLES), MP_ROM_INT(ESDR_TRIG_SAMPLES)},
	{MP_ROM_QSTR(MP_QSTR_TRIG_RATE), MP_ROM_INT(ESDR_TRIG_RATE_HZ)},
	{MP_ROM_QSTR(MP_QSTR_iq_cols), MP_ROM_PTR(&esdr_iq_cols_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_trace), MP_ROM_PTR(&esdr_fb_trace_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_wf_row), MP_ROM_PTR(&esdr_fb_wf_row_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_scroll), MP_ROM_PTR(&esdr_fb_scroll_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_points), MP_ROM_PTR(&esdr_fb_points_obj)},
	{MP_ROM_QSTR(MP_QSTR_RATES), MP_ROM_PTR(&esdr_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_BURST_RATES), MP_ROM_PTR(&esdr_burst_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_FULL_SCALE), MP_ROM_INT(FULL_SCALE)},
	{MP_ROM_QSTR(MP_QSTR_MON_SAMPLES), MP_ROM_INT(ESDR_MON_SAMPLES)},
	{MP_ROM_QSTR(MP_QSTR_TX_PREBUF_S), MP_ROM_INT(0)},
	{MP_ROM_QSTR(MP_QSTR_TX_RATE_MAX), MP_ROM_INT(RATE_MAX_HZ)},
	{MP_ROM_QSTR(MP_QSTR_TX_STREAM_MAX), MP_ROM_INT(STREAM_MAX_HZ)},
};
static MP_DEFINE_CONST_DICT(esdr_globals, esdr_globals_table);

const mp_obj_module_t esdr_module = {
	.base = {&mp_type_module},
	.globals = (mp_obj_dict_t *)&esdr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esdr, esdr_module);
