/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The LTE crystal calibration's estimator (include/esp_sdr/esp_sdr_lte.h, header only, no
 * Zephyr) on a capture in this chip's memory, for a radio behind a link: on the WHY2025 badge
 * the ESP32-P4 measures its ESP32-C6's crystal from the C6's 2 MS/s stream (modules/ltecal.py).
 *
 *   lte_detect(buf, fs, lo_hz, max_offset=900000, min_ratio=15.0, nfft=16384) -> dict or None
 *                              find an LTE cell in buf (int16 I, Q at fs, 1.92 to 4 MS/s;
 *                              nfft a power of two: the PSS is searched for in the first
 *                              nfft - symbol samples, 8 ms at 16384, the rest refines) and
 *                              measure its carrier's offset from the capture's centre; lo_hz
 *                              (int) is where the LO was, for ppb and raster_khz. None without
 *                              a PSS; keys cell, valid, offset, offset_sync, timing, pss_ratio,
 *                              pss_metric, sss, sss_second, pairs, crs_symbols, crs_metric,
 *                              ppb (the crystal's error on top of the correction in force,
 *                              positive when the LO runs high), raster_khz
 *   iq_power(buf, count=0) -> float   mean I^2 + Q^2 of the first count samples (0: all)
 */

#include <errno.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include <esp_sdr/esp_sdr_lte.h>

#include "esdr_lte.h"

static mp_obj_t esdr_lte_detect_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
	enum { ARG_buf, ARG_fs, ARG_lo_hz, ARG_max_offset, ARG_min_ratio, ARG_nfft };
	static const mp_arg_t allowed[] = {
		{MP_QSTR_buf, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_fs, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
		{MP_QSTR_lo_hz, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_max_offset, MP_ARG_INT, {.u_int = 900000}},
		{MP_QSTR_min_ratio, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
		{MP_QSTR_nfft, MP_ARG_INT, {.u_int = 16384}},
	};
	mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
	mp_buffer_info_t bi;
	struct esp_sdr_lte_work w;
	struct esp_sdr_lte_cell c;
	double raster, ppb;
	float min_ratio = 15.0f;
	int ret;

	mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
	mp_get_buffer_raise(a[ARG_buf].u_obj, &bi, MP_BUFFER_READ);
	size_t n = bi.len / sizeof(struct esp_sdr_lte_iq);
	double fs = (double)a[ARG_fs].u_int;
	double lo_hz = (double)mp_obj_get_ll(a[ARG_lo_hz].u_obj);
	uint32_t nfft = (uint32_t)a[ARG_nfft].u_int;
	size_t len = esp_sdr_lte_symbol_len(fs);

	if (a[ARG_min_ratio].u_obj != MP_OBJ_NULL) {
		min_ratio = (float)mp_obj_get_float(a[ARG_min_ratio].u_obj);
	}
	if (fs < 1.92e6 || fs > 4e6 || len > ESP_SDR_LTE_L_MAX || nfft < 512U ||
	    nfft > 65536U || (nfft & (nfft - 1U)) != 0U || n < 2U * len + 1U ||
	    a[ARG_max_offset].u_int <= 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("fs 1.92 to 4 MS/s, nfft a power of two, "
						  "buf over two symbols"));
	}
	float *mem = m_new(float, ESP_SDR_LTE_WORK_FLOATS(nfft));

	esp_sdr_lte_work_init(&w, mem, nfft);
	ret = esp_sdr_lte_detect((const struct esp_sdr_lte_iq *)bi.buf, n, fs,
				 (double)a[ARG_max_offset].u_int, min_ratio, &w, &c);
	m_del(float, mem, ESP_SDR_LTE_WORK_FLOATS(nfft));
	if (ret == -ENOENT) {
		return mp_const_none;
	}
	if (ret != 0) {
		mp_raise_OSError(-ret);
	}
	ppb = esp_sdr_lte_ppb(lo_hz, c.offset_hz, &raster);

	mp_obj_t d = mp_obj_new_dict(14);

#define PUT(name, val) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##name), (val))
	PUT(cell, MP_OBJ_NEW_SMALL_INT(c.cell_id));
	PUT(valid, mp_obj_new_bool(c.valid));
	PUT(offset, mp_obj_new_float((mp_float_t)c.offset_hz));
	PUT(offset_sync, mp_obj_new_float((mp_float_t)c.offset_sync_hz));
	PUT(timing, mp_obj_new_float((mp_float_t)c.timing));
	PUT(pss_ratio, mp_obj_new_float((mp_float_t)c.pss_ratio));
	PUT(pss_metric, mp_obj_new_float((mp_float_t)c.pss_metric));
	PUT(sss, mp_obj_new_float((mp_float_t)c.sss_metric));
	PUT(sss_second, mp_obj_new_float((mp_float_t)c.sss_second));
	PUT(pairs, MP_OBJ_NEW_SMALL_INT(c.pairs));
	PUT(crs_symbols, MP_OBJ_NEW_SMALL_INT(c.crs_symbols));
	PUT(crs_metric, mp_obj_new_float((mp_float_t)c.crs_metric));
	PUT(ppb, mp_obj_new_float((mp_float_t)ppb));
	PUT(raster_khz, mp_obj_new_int_from_uint((mp_uint_t)(raster / 1e3 + 0.5)));
#undef PUT
	return d;
}
MP_DEFINE_CONST_FUN_OBJ_KW(esdr_lte_detect_obj, 1, esdr_lte_detect_fn);

static mp_obj_t esdr_iq_power_fn(size_t n_args, const mp_obj_t *args)
{
	mp_buffer_info_t bi;
	double p = 0.0;

	mp_get_buffer_raise(args[0], &bi, MP_BUFFER_READ);
	const struct esp_sdr_lte_iq *x = bi.buf;
	size_t n = bi.len / sizeof(*x);

	if (n_args > 1 && mp_obj_get_int(args[1]) > 0 && (size_t)mp_obj_get_int(args[1]) < n) {
		n = (size_t)mp_obj_get_int(args[1]);
	}
	for (size_t i = 0; i < n; i++) {
		p += (double)x[i].i * x[i].i + (double)x[i].q * x[i].q;
	}
	return mp_obj_new_float((mp_float_t)(n > 0U ? p / (double)n : 0.0));
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_iq_power_obj, 1, 2, esdr_iq_power_fn);
