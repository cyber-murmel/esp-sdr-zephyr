/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The packet modem (esdr_fsk.c) in the esdr module, for the gnuradio.digital stand-ins:
 *
 *   fsk_rx_init(sps, code, code_len, threshold, max_len) -> state
 *                              a receiver (a bytearray); code: 8 bytes, big-endian
 *   fsk_rx(state, buf, start, n) -> [(data, errors, end, phase, dc, amp, var, power, floor)]
 *                              n samples of buf (int16 I, Q) from sample start: the frames
 *                              that ended among them (as digital/_fsk.py's Frame)
 *   fsk_rx_reset(state), fsk_rx_state(state) -> int
 *   fsk_mod(data, nbits, sps, taps, sensitivity, amp, out, phase) -> (samples, phase)
 *                              gfsk_mod into out (int16 I, Q), the rest of out zeroed (a buffer
 *                              used again); taps: float32
 */

#include <stdint.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "esdr_fsk.h"

static struct esdr_fsk_rx *rx_of(mp_obj_t st)
{
	mp_buffer_info_t bi;
	struct esdr_fsk_rx *rx;

	mp_get_buffer_raise(st, &bi, MP_BUFFER_RW);
	rx = bi.buf;
	if (bi.len < sizeof(*rx) || ((uintptr_t)bi.buf & 7U) != 0U || rx->sps < 2U ||
	    rx->sps > ESDR_FSK_SPS_MAX || rx->code_len < 8U || rx->code_len > 64U ||
	    bi.len < esdr_fsk_rx_size(rx->sps, rx->code_len)) {
		mp_raise_ValueError(MP_ERROR_TEXT("not an fsk_rx state"));
	}
	return rx;
}

static mp_obj_t esdr_fsk_rx_init_fn(size_t n_args, const mp_obj_t *args)
{
	mp_buffer_info_t code;
	uint32_t sps = (uint32_t)mp_obj_get_int(args[0]);
	uint32_t code_len = (uint32_t)mp_obj_get_int(args[2]);
	uint64_t c = 0;

	ARG_UNUSED(n_args);
	mp_get_buffer_raise(args[1], &code, MP_BUFFER_READ);
	if (code.len != 8U) {
		mp_raise_ValueError(MP_ERROR_TEXT("code: 8 bytes"));
	}
	for (size_t k = 0; k < 8U; k++) {
		c = (c << 8) | ((const uint8_t *)code.buf)[k];
	}
	if (sps < 2U || sps > ESDR_FSK_SPS_MAX || code_len < 8U || code_len > 64U) {
		mp_raise_ValueError(MP_ERROR_TEXT("sps 2..32, code 8..64 bits"));
	}
	size_t size = esdr_fsk_rx_size(sps, code_len);
	uint8_t *mem = m_new0(uint8_t, size);

	if (esdr_fsk_rx_init((struct esdr_fsk_rx *)mem, size, sps, c, code_len,
			     (uint32_t)mp_obj_get_int(args[3]), (uint32_t)mp_obj_get_int(args[4])) != 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("fsk_rx settings"));
	}
	return mp_obj_new_bytearray_by_ref(size, mem);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fsk_rx_init_obj, 5, 5, esdr_fsk_rx_init_fn);

static void emit(void *user, const struct esdr_fsk_frame *f)
{
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

	mp_obj_list_append(MP_OBJ_FROM_PTR(user), mp_obj_new_tuple(9, t));
}

static mp_obj_t esdr_fsk_rx_fn(size_t n_args, const mp_obj_t *args)
{
	struct esdr_fsk_rx *rx = rx_of(args[0]);
	mp_buffer_info_t bi;
	mp_int_t start = mp_obj_get_int(args[2]), n = mp_obj_get_int(args[3]);
	mp_obj_t out = mp_obj_new_list(0, NULL);

	ARG_UNUSED(n_args);
	mp_get_buffer_raise(args[1], &bi, MP_BUFFER_READ);
	if (((uintptr_t)bi.buf & 1U) != 0U || start < 0 || n < 0 ||
	    (size_t)(start + n) > bi.len / 4U) {
		mp_raise_ValueError(MP_ERROR_TEXT("fsk_rx: samples outside the buffer"));
	}
	esdr_fsk_rx_process(rx, (const int16_t *)bi.buf + 2 * start, (size_t)n, emit,
			    MP_OBJ_TO_PTR(out));
	return out;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fsk_rx_obj, 4, 4, esdr_fsk_rx_fn);

static mp_obj_t esdr_fsk_rx_reset_fn(mp_obj_t st)
{
	esdr_fsk_rx_reset(rx_of(st));
	return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_fsk_rx_reset_obj, esdr_fsk_rx_reset_fn);

static mp_obj_t esdr_fsk_rx_state_fn(mp_obj_t st)
{
	return mp_obj_new_int_from_uint(esdr_fsk_rx_state(rx_of(st)));
}
MP_DEFINE_CONST_FUN_OBJ_1(esdr_fsk_rx_state_obj, esdr_fsk_rx_state_fn);

static mp_obj_t esdr_fsk_mod_fn(size_t n_args, const mp_obj_t *args)
{
	mp_buffer_info_t data, taps, out;
	mp_int_t nbits = mp_obj_get_int(args[1]), sps = mp_obj_get_int(args[2]);
	float phase = (float)mp_obj_get_float(args[7]);

	ARG_UNUSED(n_args);
	mp_get_buffer_raise(args[0], &data, MP_BUFFER_READ);
	mp_get_buffer_raise(args[3], &taps, MP_BUFFER_READ);
	mp_get_buffer_raise(args[6], &out, MP_BUFFER_WRITE);
	if (nbits < 0 || (size_t)nbits > 8U * data.len || sps < 2 ||
	    sps > (mp_int_t)ESDR_FSK_SPS_MAX || ((uintptr_t)taps.buf & 3U) != 0U ||
	    ((uintptr_t)out.buf & 1U) != 0U) {
		mp_raise_ValueError(MP_ERROR_TEXT("fsk_mod arguments"));
	}
	size_t n = esdr_fsk_mod(data.buf, (size_t)nbits, (uint32_t)sps, taps.buf, taps.len / 4U,
				(float)mp_obj_get_float(args[4]), (float)mp_obj_get_float(args[5]),
				out.buf, out.len / 4U, &phase);

	if (n == 0U && nbits > 0) {
		mp_raise_ValueError(MP_ERROR_TEXT("fsk_mod: out too small"));
	}
	memset((uint8_t *)out.buf + 4U * n, 0, out.len - 4U * n);
	mp_obj_t t[2] = {mp_obj_new_int_from_uint(n), mp_obj_new_float((mp_float_t)phase)};

	return mp_obj_new_tuple(2, t);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(esdr_fsk_mod_obj, 8, 8, esdr_fsk_mod_fn);
