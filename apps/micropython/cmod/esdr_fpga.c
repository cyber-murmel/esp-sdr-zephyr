/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The fpga module: the MCH2022 badge's FPGA (drivers/esdr_fpga, gateware/) for the qtgui
 * stand-ins. It computes the spectrum of the monitored samples and draws a waterfall and a
 * spectrum trace on the display itself; modules/gnuradio/qtgui.py hands it the plot areas.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>
#include <esp_sdr/esp_sdr_dsp.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_rx.h"

static const struct device *const fpga = DEVICE_DT_GET(DT_NODELABEL(fpga));

/* The monitor's samples since the last feed, in PSRAM (the driver bounces them for the DMA). */
static uint32_t feed_buf[ESDR_MON_SAMPLES] __attribute__((section(".ext_ram.bss")));
static uint64_t fed;
/* Samples sent since the last line: a line once a frame's worth came (lines on demand). */
static uint32_t since_line;

static struct esdr_fpga_dsp dsp;

static void check(int ret)
{
	if (ret < 0) {
		mp_raise_OSError(-ret);
	}
}

/* ok() -> bool: the gateware is loaded and has the panel. */
static mp_obj_t fpga_ok(void)
{
	return mp_obj_new_bool(device_is_ready(fpga) && esdr_fpga_loaded(fpga));
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_ok_obj, fpga_ok);

/* palette(colours): up to 256 RGB565 entries (array 'H'). */
static mp_obj_t fpga_palette(mp_obj_t pal_in)
{
	mp_buffer_info_t bi;

	mp_get_buffer_raise(pal_in, &bi, MP_BUFFER_READ);
	check(esdr_fpga_palette(fpga, 0, bi.buf, MIN(bi.len / 2, 256)));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(fpga_palette_obj, fpga_palette);

/* waterfall(x, y, w, h): its plot area on the screen; w = 0 turns it off. */
static mp_obj_t fpga_waterfall(size_t n_args, const mp_obj_t *args)
{
	check(esdr_fpga_waterfall(fpga, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
				  mp_obj_get_int(args[2]), mp_obj_get_int(args[3])));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fpga_waterfall_obj, 4, 4, fpga_waterfall);

/* spectrum(x, y, w, h, centre): the trace's plot area; w = 0 turns it off. */
static mp_obj_t fpga_spectrum(size_t n_args, const mp_obj_t *args)
{
	struct esdr_fpga_trace t = {
		.flags = mp_obj_is_true(args[4]) ? ESDR_FPGA_SP_CENTER : 0,
	};

	check(esdr_fpga_spectrum(fpga, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
				 mp_obj_get_int(args[2]), mp_obj_get_int(args[3]), &t));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fpga_spectrum_obj, 5, 5, fpga_spectrum);

/*
 * dsp(size, window, alpha, frames_per_line, cols, wf_min, wf_max, tr_top, tr_bottom, rows):
 * the FFT (16..512, window kind as GNU Radio numbers them), the exponential average's weight
 * (1: none), a line every frames_per_line frames (0: at feed() calls) over cols columns, the waterfall's and the
 * trace's dB ranges (relative to a full-scale tone, as esdr.psd()), the trace's rows. Starts it.
 */
static mp_obj_t fpga_dsp(size_t n_args, const mp_obj_t *args)
{
	int size = mp_obj_get_int(args[0]);
	int kind = mp_obj_get_int(args[1]);
	int log2n = 4;

	while ((1 << log2n) < size && log2n < 9) {
		log2n++;
	}
	dsp = (struct esdr_fpga_dsp){
		.log2n = log2n,
		/* gnuradio.fft.window kinds: 0..3 as they are, others Blackman-Harris. */
		.window = kind >= 0 && kind <= ESP_SDR_WINDOW_RECT ? (enum esp_sdr_window)kind
								 : ESP_SDR_WINDOW_BLACKMAN_HARRIS,
		.alpha = mp_obj_get_float(args[2]),
		.frames_per_line = CLAMP(mp_obj_get_int(args[3]), 0, 255),
		.cols = mp_obj_get_int(args[4]),
		.wf_min_db = mp_obj_get_float(args[5]),
		.wf_max_db = mp_obj_get_float(args[6]),
		.tr_top_db = mp_obj_get_float(args[7]),
		.tr_bottom_db = mp_obj_get_float(args[8]),
		.tr_rows = MAX(mp_obj_get_int(args[9]), 2),
		/* The monitor's int16 samples: full scale 16384. */
		.full_scale = 16384.0f,
	};
	if (dsp.alpha <= 0.0f || dsp.alpha > 1.0f) {
		dsp.alpha = 1.0f;
	}
	check(esdr_fpga_dsp_config(fpga, &dsp));
	check(esdr_fpga_render(fpga, true, true, true));
	check(esdr_fpga_dsp_run(fpga, true, &dsp));
	fed = 0;
	since_line = 0;
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fpga_dsp_obj, 10, 10, fpga_dsp);

/*
 * feed() -> int: the monitor's samples since the last call to the DSP (at most the monitor's
 * size; a gap starts new frames), and with lines on demand a line once a frame's worth has come
 * since the last one: one line per call while the samples keep up. Returns how many went.
 */
static mp_obj_t fpga_feed(void)
{
	uint64_t total = esdr_rx_latest((int16_t *)feed_buf, 0);
	uint64_t fresh = total - fed;
	size_t n = (size_t)MIN(fresh, (uint64_t)ESDR_MON_SAMPLES);
	bool gap = fresh > ESDR_MON_SAMPLES || fed == 0;

	if (n == 0) {
		return MP_OBJ_NEW_SMALL_INT(0);
	}
	total = esdr_rx_latest((int16_t *)feed_buf, n);
	/* Whatever came between the two calls goes next time. */
	n = (size_t)MIN((uint64_t)n, total - fed);
	fed = total;
	check(esdr_fpga_samples(fpga, ESDR_FPGA_FMT_IQ16, gap, feed_buf, n * 4));
	since_line += n;
	if (dsp.frames_per_line == 0 && since_line >= (1U << dsp.log2n)) {
		const uint16_t emit = 1;

		check(esdr_fpga_reg_write(fpga, ESDR_FPGA_REG_EMIT, &emit, 1));
		since_line = 0;
	}
	return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_feed_obj, fpga_feed);

/* levels(out) -> out: the last line's level per column in dB (array 'f'). */
static mp_obj_t fpga_levels(mp_obj_t out_in)
{
	static int16_t l[320] __aligned(4);
	mp_buffer_info_t bi;
	size_t n;

	mp_get_buffer_raise(out_in, &bi, MP_BUFFER_WRITE);
	n = MIN(bi.len / sizeof(float), ARRAY_SIZE(l));
	check(esdr_fpga_dsp_columns(fpga, l, n));
	for (size_t c = 0; c < n; c++) {
		((float *)bi.buf)[c] = esdr_fpga_l_to_db(l[c]);
	}
	return out_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(fpga_levels_obj, fpga_levels);

/* counters() -> (frames, lines, renders, dropped): the gateware's counters (16 bits, wrapping). */
static mp_obj_t fpga_counters(void)
{
	static const uint16_t regs[] = {
		ESDR_FPGA_REG_FRAMES, ESDR_FPGA_REG_LINES, ESDR_FPGA_REG_RENDERS,
		ESDR_FPGA_REG_DROPPED,
	};
	mp_obj_t out[ARRAY_SIZE(regs)];

	for (size_t k = 0; k < ARRAY_SIZE(regs); k++) {
		uint16_t v;

		check(esdr_fpga_reg_read(fpga, regs[k], &v, 1));
		out[k] = MP_OBJ_NEW_SMALL_INT(v);
	}
	return mp_obj_new_tuple(ARRAY_SIZE(out), out);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_counters_obj, fpga_counters);

/* reg(addr[, value]) -> int: read (or write) a gateware register (esdr_fpga_regs.h). */
static mp_obj_t fpga_reg(size_t n_args, const mp_obj_t *args)
{
	uint16_t addr = mp_obj_get_int(args[0]);
	uint16_t v;

	if (n_args > 1) {
		v = mp_obj_get_int(args[1]);
		check(esdr_fpga_reg_write(fpga, addr, &v, 1));
		return mp_const_none;
	}
	check(esdr_fpga_reg_read(fpga, addr, &v, 1));
	return MP_OBJ_NEW_SMALL_INT(v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fpga_reg_obj, 1, 2, fpga_reg);

/* stop(): no more lines, no more drawing; the regions keep their last picture. */
static mp_obj_t fpga_stop(void)
{
	check(esdr_fpga_dsp_run(fpga, false, &dsp));
	check(esdr_fpga_render(fpga, false, false, false));
	check(esdr_fpga_waterfall(fpga, 0, 0, 0, 0));
	check(esdr_fpga_spectrum(fpga, 0, 0, 0, 0, &(struct esdr_fpga_trace){0}));
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_stop_obj, fpga_stop);

static const mp_rom_map_elem_t fpga_globals[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fpga)},
	{MP_ROM_QSTR(MP_QSTR_ok), MP_ROM_PTR(&fpga_ok_obj)},
	{MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&fpga_palette_obj)},
	{MP_ROM_QSTR(MP_QSTR_waterfall), MP_ROM_PTR(&fpga_waterfall_obj)},
	{MP_ROM_QSTR(MP_QSTR_spectrum), MP_ROM_PTR(&fpga_spectrum_obj)},
	{MP_ROM_QSTR(MP_QSTR_dsp), MP_ROM_PTR(&fpga_dsp_obj)},
	{MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&fpga_feed_obj)},
	{MP_ROM_QSTR(MP_QSTR_levels), MP_ROM_PTR(&fpga_levels_obj)},
	{MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&fpga_stop_obj)},
	{MP_ROM_QSTR(MP_QSTR_counters), MP_ROM_PTR(&fpga_counters_obj)},
	{MP_ROM_QSTR(MP_QSTR_reg), MP_ROM_PTR(&fpga_reg_obj)},
	{MP_ROM_QSTR(MP_QSTR_FFT_MAX), MP_ROM_INT(512)},
	{MP_ROM_QSTR(MP_QSTR_PALETTE_SIZE), MP_ROM_INT(ESDR_FPGA_PAL_BG)},
	{MP_ROM_QSTR(MP_QSTR_PAL_BG), MP_ROM_INT(ESDR_FPGA_PAL_BG)},
	{MP_ROM_QSTR(MP_QSTR_PAL_TRACE), MP_ROM_INT(ESDR_FPGA_PAL_TRACE)},
	{MP_ROM_QSTR(MP_QSTR_PAL_CENTER), MP_ROM_INT(ESDR_FPGA_PAL_CENTER)},
};
static MP_DEFINE_CONST_DICT(fpga_globals_dict, fpga_globals);

const mp_obj_module_t fpga_module = {
	.base = {&mp_type_module},
	.globals = (mp_obj_dict_t *)&fpga_globals_dict,
};
MP_REGISTER_MODULE(MP_QSTR_fpga, fpga_module);
