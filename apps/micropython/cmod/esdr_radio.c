/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module, as modesdr.c is on the S3, for a radio behind esdr_remote.h: on the
 * ESP32-P4 of the WHY2025 badge the badge's ESP32-C6, running apps/osmosdr_serial behind a link
 * (esdr_remote.c), CPU-paced: no offset tuning, 8-bit samples; on the classic ESP32 of the
 * MCH2022 badge the chip's own radio (esdr_local.c), which captures bursts only: its stream
 * rates are decimated bursts with gaps between them, its snapshots come from bursts, and it
 * transmits a carrier only (a generated tone or constant; no TX_STREAM_MAX: the launcher hides
 * the apps that send samples).
 *
 *   init()                     opens the link, reads the radio's description
 *   tune(hz, offset=True)      LO at hz (offset is ignored); returns the centre tuned
 *   rx_gain([index]), rx_gain_max(), rx_bandwidth(hz), channel_bw(mode) (ignored)
 *   radio_state() -> dict      the radio's settings as it reports them (mode, freq, centre,
 *                              rx_gain, tx_gain, rate, bits, bandwidth); after an app ends
 *                              they are the radio's start settings again
 *   temp() -> float            the radio chip's die temperature in degC (OSError without a
 *                              sensor); over the link it briefly stops a running stream, read
 *                              it between runs
 *   RATES, BURST_RATES (P4: 80, 40 MS/s, bursts of up to 16380 samples at 80 MS/s; ESP32:
 *                              80, 40, 16 MS/s; contiguous within a burst only, full scale
 *                              16384 as on the S3), FULL_SCALE (P4 streams: 128, ESP32: 16384),
 *                              MON_SAMPLES, TX_PREBUF_S
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
 *   gapless_start(ratio=8, level=0, pre=1, post=1, budget=2000000, gain_servo=False,
 *                 monitor=True, channels=0, open_db=6)
 *                              P4: the whole 80 MHz without a gap (the C6's gapless
 *                              stream, its default build), the 10 us windows ratio times the
 *                              quiet power (or past a fixed level) and pre / post of them
 *                              around, at most budget bytes a second; monitor: their samples to
 *                              rx_latest() (full scale 16384); channels (16, 32, 40, 64, 80):
 *                              the channelizer and burst detector (esdr_gl.c), opening open_db
 *                              over a channel's floor. OSError EOPNOTSUPP from a C6 without the
 *                              mode
 *   gapless_stop(), gapless_stats() -> dict
 *   chan_power(power, floor=None, peak=None, open=None) -> blocks
 *                              per channel (float32 arrays, the lowest frequency first) since
 *                              the last call: mean, floor, peak power (dB of a full-scale
 *                              carrier), share of blocks open; the blocks that took
 *   chan_bursts() -> [(offset_hz, start_s, length_s, peak_db)]   bursts ended since the last
 *                              call
 *   chan_spectrum(out) -> bins a row of the band's spectrum made of the bursts found since the
 *                              last call, each at its channel, over the channels' floors
 *                              (esdr_gl_spectrum(): dB of a full-scale carrier, the lowest
 *                              frequency first, 8 bins a channel) into out (float32, up to
 *                              CHAN_SPEC_MAX entries); 0 without a channelizer
 *   GAPLESS_RATE               80 MS/s
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
 *   usb_to_c6(seconds=60)      WHY2025 P4: USB-C port J8 to the ESP32-C6's USB-Serial-JTAG
 *                              (esptool) for that long, then back to this chip's REPL and DFU
 *                              (esdr_usb.c)
 *   request(req, value=0, data=None, n_in=0) -> bytes or None
 *                              one protocol request to the radio (esdr_proto.h; on the ESP32
 *                              the subset esdr_local.c takes); n_in > 0 asks for that many
 *                              bytes back; a negative status raises OSError
 *   lte_detect(buf, fs, lo_hz, ...) -> dict or None, iq_power(buf, count=0)
 *                              the LTE crystal calibration's estimator on a capture
 *                              (esdr_lte.c); modules/ltecal.py measures the radio's crystal
 *                              with it and keeps the result
 *
 * Memory: the Python heap is CONFIG_APP_MP_PSRAM_HEAP_KB of PSRAM, added at every start; LVGL
 * allocates from it too (CONFIG_LV_Z_MEM_POOL_MICROPYTHON).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#ifdef CONFIG_LVGL
#include <lvgl.h>
#endif

#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_board.h"
#include "esdr_defs.h"
#include "esdr_lte.h"
#include "esdr_remote.h"
#include "esdr_rx.h"

#if defined(CONFIG_ESP_SDR)
/* The radio on the chip (esdr_local.c): bursts, and streams decimated from them. */
#define RADIO_LINKED 0
#define FULL_SCALE 16384
/* Transmit is a carrier: any rate holds a generated tone. */
#define RATE_MAX_HZ 250000
#define RX_RATE_MAX_HZ 2000000
#define RX_RATE_COUNT 5
#else
/* The radio behind a link: transmit and triggered snapshots too. */
#define RADIO_LINKED 1
#include "esdr_usb.h"
#if defined(CONFIG_SOC_SERIES_ESP32P4)
/* The gapless stream and its channelizer (the badge's C6 over SDIO, esdr_gl.c). */
#define HAVE_GAPLESS 1
#include "esdr_gl.h"
#endif

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
#endif

/* The buffers the link reads or writes, kept alive meanwhile. */
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_rx_buf);
MP_REGISTER_ROOT_POINTER(mp_obj_t esdr_tx_buf);

static uint8_t psram_heap[CONFIG_APP_MP_PSRAM_HEAP_KB * 1024] ESDR_PSRAM __aligned(16);

static const struct esdr_info *radio;

static void need_radio(void)
{
	if (radio == NULL) {
		esdr_check(esdr_remote_init(&radio));
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
	esdr_check(esdr_remote_request(ESDR_REQ_SET_FREQ, 0, le, sizeof(le), NULL, 0));
	esdr_check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
	return mp_obj_new_float((mp_float_t)sys_le64_to_cpu(st.center_hz));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tune_obj, 1, esdr_tune);

/* radio_state() -> dict: the radio's settings (struct esdr_state), as it reports them. */
static mp_obj_t esdr_radio_state_fn(void)
{
	struct esdr_state st;
	mp_obj_t d = mp_obj_new_dict(8);

	need_radio();
	esdr_check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
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

/* temp() -> float: the radio chip's die temperature in degC. */
static mp_obj_t esdr_temp_fn(void)
{
	uint8_t b[2];
	int ret;

	need_radio();
	ret = esdr_remote_request(ESDR_REQ_GET_TEMP, 0, NULL, 0, b, sizeof(b));
	esdr_check(ret < 0 ? ret : ret == (int)sizeof(b) ? 0 : -EPROTO);
	return mp_obj_new_float((mp_float_t)(int16_t)sys_get_le16(b) / 10);
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_temp_obj, esdr_temp_fn);

static mp_obj_t esdr_rx_gain(size_t n_args, const mp_obj_t *args)
{
	need_radio();
	if (n_args == 0) {
		struct esdr_state st;

		esdr_check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
		return MP_OBJ_NEW_SMALL_INT(st.rx_gain == ESDR_GAIN_AUTO ? -1 : st.rx_gain);
	}
	mp_int_t g = mp_obj_get_int(args[0]);

	esdr_check(esdr_remote_request(ESDR_REQ_SET_RX_GAIN, g < 0 ? ESDR_GAIN_AUTO : (uint16_t)g,
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
	esdr_check(esdr_remote_request(ESDR_REQ_SET_BANDWIDTH, 0, le, sizeof(le), NULL, 0));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_rx_bandwidth_obj, esdr_rx_bandwidth);

/* The analog filters follow the radio's own settings: nothing to switch. */
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
#if RADIO_LINKED
	bool burst = rate == BURST_HZ || rate == BURST_HZ / 2;
#else
	bool burst = rate == 80000000 || rate == 40000000 || rate == 16000000;
#endif

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
#if RADIO_LINKED
		esdr_check(esdr_remote_rx_start_burst(BURST_HZ / 1000000, BURST_HZ / rate, buf, cap,
						      (uint64_t)args[ARG_count].u_int));
#else
		esdr_check(esdr_remote_rx_start_burst(rate / 1000000, 1, buf, cap,
						      (uint64_t)args[ARG_count].u_int));
#endif
		return mp_const_none;
	}
	esdr_check(esdr_remote_rx_start((uint32_t)rate, buf, cap, (uint64_t)args[ARG_count].u_int,
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
	esdr_check(esdr_remote_trig_start((uint16_t)(ratio * 16.0f + 0.5f),
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

#if defined(HAVE_GAPLESS)
/* ---- gapless receive and its channelizer ---- */

/*
 * gapless_start(ratio=8, level=0, pre=1, post=1, budget=2000000, gain_servo=False,
 *               monitor=True, channels=0, open_db=6)
 */
static mp_obj_t esdr_gapless_start_fn(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
	enum {
		ARG_ratio, ARG_level, ARG_pre, ARG_post, ARG_budget, ARG_gain_servo, ARG_monitor,
		ARG_channels, ARG_open_db
	};
	static const mp_arg_t allowed[] = {
		{MP_QSTR_ratio, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_level, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_pre, MP_ARG_INT, {.u_int = 1}},
		{MP_QSTR_post, MP_ARG_INT, {.u_int = 1}},
		{MP_QSTR_budget, MP_ARG_INT, {.u_int = 2000000}},
		{MP_QSTR_gain_servo, MP_ARG_BOOL, {.u_bool = false}},
		{MP_QSTR_monitor, MP_ARG_BOOL, {.u_bool = true}},
		{MP_QSTR_channels, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_open_db, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
	};
	mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
	struct esdr_gapless_cfg cfg;
	float ratio = 8.0f, open_db = 6.0f;
	mp_int_t level, pre, post, budget, channels;
	int ret;

	mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, args);
	if (args[ARG_ratio].u_obj != MP_OBJ_NULL) {
		ratio = (float)mp_obj_get_float(args[ARG_ratio].u_obj);
	}
	if (args[ARG_open_db].u_obj != MP_OBJ_NULL) {
		open_db = (float)mp_obj_get_float(args[ARG_open_db].u_obj);
	}
	level = args[ARG_level].u_int;
	pre = args[ARG_pre].u_int;
	post = args[ARG_post].u_int;
	budget = args[ARG_budget].u_int;
	channels = args[ARG_channels].u_int;
	if (!(ratio > 1.0625f) || ratio > 4000.0f || level < 0 || pre < 0 || pre > 4 || post < 0 ||
	    post > 4 || budget < 0 || !(open_db > 0.0f)) {
		mp_raise_ValueError(MP_ERROR_TEXT(
			"gapless_start: ratio above 1, level >= 0, pre and post 0 to 4, budget >= 0"));
	}
	need_radio();
	memset(&cfg, 0, sizeof(cfg));
	cfg.ratio_x16 = sys_cpu_to_le16((uint16_t)(ratio * 16.0f + 0.5f));
	cfg.pre = (uint8_t)pre;
	cfg.post = (uint8_t)post;
	cfg.level = sys_cpu_to_le32((uint32_t)level);
	cfg.budget_bps = sys_cpu_to_le32((uint32_t)budget);
	cfg.flags = sys_cpu_to_le16((args[ARG_gain_servo].u_bool ? ESDR_GAPLESS_F_GAIN_SERVO : 0U) |
				    (level != 0 ? ESDR_GAPLESS_F_FIXED_THR : 0U));
	if (channels != 0) {
		esdr_check(esdr_gl_start((unsigned int)channels, open_db));
	} else {
		esdr_gl_stop();
	}
	ret = esdr_remote_gl_start(&cfg, args[ARG_monitor].u_bool);
	if (ret < 0) {
		esdr_gl_stop();
		/* A radio without the mode: as errno.EOPNOTSUPP, which MicroPython names (it has no
		 * ENOTSUP), apart from a transient error. */
		esdr_check(ret == -ENOTSUP ? -MP_EOPNOTSUPP : ret);
	}
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_gapless_start_obj, 0, esdr_gapless_start_fn);

static mp_obj_t esdr_gapless_stop_fn(void)
{
	esdr_remote_gl_stop();
	esdr_gl_stop();
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_gapless_stop_obj, esdr_gapless_stop_fn);

static mp_obj_t esdr_gapless_stats_fn(void)
{
	struct esdr_remote_gl_stats st;
	struct esdr_gapless_stats rs;
	struct esdr_gl_stats cs;
	mp_obj_t d = mp_obj_new_dict(20);

	esdr_remote_gl_stats(&st);
	esdr_gl_stats(&cs);
#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(blocks, mp_obj_new_int_from_uint(st.blocks));
	PUT(segments, mp_obj_new_int_from_uint(st.segments));
	PUT(bad, mp_obj_new_int_from_uint(st.bad));
	PUT(gaps, mp_obj_new_int_from_uint(st.gaps));
	PUT(pairs, mp_obj_new_int_from_ull(st.pairs));
	PUT(chan_blocks, mp_obj_new_int_from_uint(cs.blocks));
	PUT(chan_dropped, mp_obj_new_int_from_uint(cs.dropped));
	PUT(chan_pairs, mp_obj_new_int_from_ull(cs.pairs));
	PUT(chan_cycles, mp_obj_new_int_from_ull(cs.cycles));
	PUT(bursts, mp_obj_new_int_from_uint(cs.bursts));
	PUT(bursts_lost, mp_obj_new_int_from_uint(cs.bursts_lost));
	PUT(chan_stack_free, mp_obj_new_int_from_uint(cs.stack_free));
	/* The radio's own counters (the request restarts its run). */
	int ret = esdr_remote_gl_busy() ? esdr_remote_gl_radio_stats(&rs) : -ENODEV;

	if (ret != 0) {
		PUT(radio_error, MP_OBJ_NEW_SMALL_INT(-ret));
	} else {
		PUT(radio_pairs, mp_obj_new_int_from_ull(sys_le64_to_cpu(rs.pairs)));
		PUT(kept_pairs, mp_obj_new_int_from_ull(sys_le64_to_cpu(rs.kept_pairs)));
		PUT(dropped_pairs, mp_obj_new_int_from_ull(sys_le64_to_cpu(rs.dropped_pairs)));
		PUT(ring_errors, mp_obj_new_int_from_uint(sys_le32_to_cpu(rs.ring_errors)));
		PUT(ratio, mp_obj_new_float(sys_le32_to_cpu(rs.ratio_x16) / 16.0f));
		PUT(gain, mp_obj_new_int_from_uint(sys_le32_to_cpu(rs.gain)));
		PUT(blocks_sent, mp_obj_new_int_from_uint(sys_le32_to_cpu(rs.blocks_sent)));
	}
#undef PUT
	return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_gapless_stats_obj, esdr_gapless_stats_fn);

/* A float32 buffer for the channels, or NULL for None. */
static float *chan_buf(mp_obj_t o)
{
	mp_buffer_info_t bi;

	if (o == mp_const_none) {
		return NULL;
	}
	mp_get_buffer_raise(o, &bi, MP_BUFFER_WRITE);
	if (bi.len < 4U * esdr_gl_channels() || ((uintptr_t)bi.buf & 3U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("chan_power: float32 arrays of the channels"));
	}
	return bi.buf;
}

/* chan_power(power, floor=None, peak=None, open=None) -> blocks */
static mp_obj_t esdr_chan_power_fn(size_t n_args, const mp_obj_t *args)
{
	float *v[4] = {NULL, NULL, NULL, NULL};

	if (esdr_gl_channels() == 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("chan_power: no channelizer (gapless_start)"));
	}
	for (size_t i = 0; i < n_args; i++) {
		v[i] = chan_buf(args[i]);
	}
	return mp_obj_new_int_from_uint(esdr_gl_power(v[0], v[1], v[2], v[3]));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_chan_power_obj, 1, 4, esdr_chan_power_fn);

/* chan_bursts() -> [(offset_hz, start_s, length_s, peak_db), ...] */
static mp_obj_t esdr_chan_bursts_fn(void)
{
	struct esdr_gl_burst b[16];
	mp_obj_t list = mp_obj_new_list(0, NULL);
	unsigned int n;

	while ((n = esdr_gl_bursts(b, ARRAY_SIZE(b))) > 0U) {
		for (unsigned int i = 0; i < n; i++) {
			mp_obj_t t[4] = {
				mp_obj_new_float(b[i].offset_hz), mp_obj_new_float(b[i].start_s),
				mp_obj_new_float(b[i].length_s), mp_obj_new_float(b[i].peak_db),
			};

			mp_obj_list_append(list, mp_obj_new_tuple(4, t));
		}
	}
	return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(esdr_chan_bursts_obj, esdr_chan_bursts_fn);

/* chan_spectrum(out) -> bins */
static mp_obj_t esdr_chan_spectrum_fn(mp_obj_t out_in)
{
	mp_buffer_info_t bi;
	int n;

	mp_get_buffer_raise(out_in, &bi, MP_BUFFER_WRITE);
	if (((uintptr_t)bi.buf & 3U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("chan_spectrum: an aligned float32 array"));
	}
	n = esdr_gl_spectrum(bi.buf, bi.len / 4U);
	if (n == -ENODEV) {
		return MP_OBJ_NEW_SMALL_INT(0);
	}
	if (n < 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("chan_spectrum: 8 float32 entries a channel"));
	}
	return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esdr_chan_spectrum_obj, esdr_chan_spectrum_fn);
#endif

/* ---- transmit ---- */

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
	struct esdr_gen gen;
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
		esdr_gen_from(&gen, args[ARG_gen].u_obj);
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
	esdr_check(esdr_remote_request(ESDR_REQ_SET_FREQ, 0, le, sizeof(le), NULL, 0));
	esdr_check(esdr_remote_request(ESDR_REQ_SET_TX_GAIN, (uint16_t)args[ARG_gain].u_int, NULL,
				       0, NULL, 0));
	esdr_check(esdr_remote_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st)));
	MP_STATE_PORT(esdr_tx_buf) = args[ARG_buf].u_obj;
	esdr_check(esdr_remote_tx_start((uint32_t)args[ARG_rate].u_int, bi.buf, n,
					args[ARG_repeat].u_bool,
					args[ARG_gen].u_obj != mp_const_none ? &gen : NULL,
					(uint64_t)MAX(args[ARG_count].u_int, 0), scale));
	return mp_obj_new_float((mp_float_t)sys_le64_to_cpu(st.center_hz));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_tx_start_obj, 3, esdr_tx_start_fn);

/* tx_gen(wave, cycles, amp, dc_i, dc_q): the generator changes while it plays. */
static mp_obj_t esdr_tx_gen_fn(size_t n_args, const mp_obj_t *args)
{
	struct esdr_gen g;

	esdr_gen_from(&g, mp_obj_new_tuple(n_args, args));
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
#if defined(HAVE_GAPLESS)
		esdr_gl_stop();
#endif
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
#if RADIO_LINKED
/* The C6 captures bursts at 80 MS/s; 40 MS/s is them halved on the P4. */
static const mp_rom_obj_tuple_t esdr_burst_rates_obj = {
	{&mp_type_tuple},
	2,
	{
		MP_ROM_INT(BURST_HZ),
		MP_ROM_INT(BURST_HZ / 2),
	},
};
#else
/* The chip's native rates. */
static const mp_rom_obj_tuple_t esdr_burst_rates_obj = {
	{&mp_type_tuple},
	3,
	{
		MP_ROM_INT(80000000),
		MP_ROM_INT(40000000),
		MP_ROM_INT(16000000),
	},
};
#endif

#if RADIO_LINKED && ESDR_USB_TO_C6
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
#endif

static mp_obj_t esdr_request_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_req, ARG_value, ARG_data, ARG_n_in };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_req, MP_ARG_REQUIRED | MP_ARG_INT},
		{MP_QSTR_value, MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_data, MP_ARG_OBJ, {.u_obj = mp_const_none}},
		{MP_QSTR_n_in, MP_ARG_INT, {.u_int = 0}},
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	mp_buffer_info_t bi = {.buf = NULL, .len = 0};
	uint8_t in[ESDR_SER_MAX_DATA];
	int ret;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
	need_radio();
	if (a[ARG_data].u_obj != mp_const_none) {
		mp_get_buffer_raise(a[ARG_data].u_obj, &bi, MP_BUFFER_READ);
	}
	if (bi.len > ESDR_SER_MAX_DATA || a[ARG_n_in].u_int < 0 ||
	    a[ARG_n_in].u_int > (mp_int_t)sizeof(in) || (a[ARG_n_in].u_int > 0 && bi.len > 0)) {
		mp_raise_ValueError(MP_ERROR_TEXT("data or n_in, up to 512 bytes"));
	}
	ret = esdr_remote_request((uint8_t)a[ARG_req].u_int, (uint16_t)a[ARG_value].u_int, bi.buf,
				  (uint16_t)bi.len, a[ARG_n_in].u_int > 0 ? in : NULL,
				  (uint16_t)a[ARG_n_in].u_int);
	if (ret < 0) {
		mp_raise_OSError(-ret);
	}
	return a[ARG_n_in].u_int > 0 ? mp_obj_new_bytes(in, ret) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(esdr_request_obj, 1, esdr_request_fn);

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
	{MP_ROM_QSTR(MP_QSTR_temp), MP_ROM_PTR(&esdr_temp_obj)},
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
#if defined(CONFIG_SHELL_BACKEND_DUMMY)
	{MP_ROM_QSTR(MP_QSTR_shell), MP_ROM_PTR(&esdr_shell_obj)},
#endif
#if RADIO_LINKED && ESDR_USB_TO_C6
	{MP_ROM_QSTR(MP_QSTR_usb_to_c6), MP_ROM_PTR(&esdr_usb_to_c6_obj)},
#endif
	{MP_ROM_QSTR(MP_QSTR_request), MP_ROM_PTR(&esdr_request_obj)},
	{MP_ROM_QSTR(MP_QSTR_lte_detect), MP_ROM_PTR(&esdr_lte_detect_obj)},
	{MP_ROM_QSTR(MP_QSTR_iq_power), MP_ROM_PTR(&esdr_iq_power_obj)},
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
#if defined(HAVE_GAPLESS)
	{MP_ROM_QSTR(MP_QSTR_gapless_start), MP_ROM_PTR(&esdr_gapless_start_obj)},
	{MP_ROM_QSTR(MP_QSTR_gapless_stop), MP_ROM_PTR(&esdr_gapless_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_gapless_stats), MP_ROM_PTR(&esdr_gapless_stats_obj)},
	{MP_ROM_QSTR(MP_QSTR_chan_power), MP_ROM_PTR(&esdr_chan_power_obj)},
	{MP_ROM_QSTR(MP_QSTR_chan_bursts), MP_ROM_PTR(&esdr_chan_bursts_obj)},
	{MP_ROM_QSTR(MP_QSTR_chan_spectrum), MP_ROM_PTR(&esdr_chan_spectrum_obj)},
	{MP_ROM_QSTR(MP_QSTR_CHAN_SPEC_MAX), MP_ROM_INT(ESDR_GL_SPEC_BINS_MAX)},
	{MP_ROM_QSTR(MP_QSTR_GAPLESS_RATE), MP_ROM_INT(ESDR_GAPLESS_RATE_HZ)},
#endif
	{MP_ROM_QSTR(MP_QSTR_iq_cols), MP_ROM_PTR(&esdr_iq_cols_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_trace), MP_ROM_PTR(&esdr_fb_trace_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_wf_row), MP_ROM_PTR(&esdr_fb_wf_row_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_scroll), MP_ROM_PTR(&esdr_fb_scroll_obj)},
	{MP_ROM_QSTR(MP_QSTR_fb_points), MP_ROM_PTR(&esdr_fb_points_obj)},
	{MP_ROM_QSTR(MP_QSTR_RATES), MP_ROM_PTR(&esdr_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_BURST_RATES), MP_ROM_PTR(&esdr_burst_rates_obj)},
	{MP_ROM_QSTR(MP_QSTR_FULL_SCALE), MP_ROM_INT(FULL_SCALE)},
	{MP_ROM_QSTR(MP_QSTR_MON_SAMPLES), MP_ROM_INT(ESDR_MON_SAMPLES)},
	{MP_ROM_QSTR(MP_QSTR_TX_RATE_MAX), MP_ROM_INT(RATE_MAX_HZ)},
#if RADIO_LINKED
	/* Samples from buffers (the ESP32 transmits carriers only; an app's "needs"). */
	{MP_ROM_QSTR(MP_QSTR_TX_BUFFERS), MP_ROM_INT(1)},
	{MP_ROM_QSTR(MP_QSTR_TX_PREBUF_S), MP_ROM_INT(0)},
	{MP_ROM_QSTR(MP_QSTR_TX_STREAM_MAX), MP_ROM_INT(STREAM_MAX_HZ)},
#endif
};
static MP_DEFINE_CONST_DICT(esdr_globals, esdr_globals_table);

const mp_obj_module_t esdr_module = {
	.base = {&mp_type_module},
	.globals = (mp_obj_dict_t *)&esdr_globals,
};

MP_REGISTER_MODULE(MP_QSTR_esdr, esdr_module);
