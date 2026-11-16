/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module's crash(), where apps/common's crash records are built (app_crash.c with
 * common.cmake): the S3 (modesdr.c) and the P4 (esdr_radio.c) put it in their module tables.
 */

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "app_crash.h"
#include "esdr_board.h"

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
MP_DEFINE_CONST_FUN_OBJ_0(esdr_crash_obj, esdr_crash);
