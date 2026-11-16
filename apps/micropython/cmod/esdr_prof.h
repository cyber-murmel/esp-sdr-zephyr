/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sampling profiler's module functions (esdr_prof.c, CONFIG_APP_PROF).
 */

#ifndef ESDR_PROF_H
#define ESDR_PROF_H

#include "py/obj.h"

extern const mp_obj_fun_builtin_fixed_t esdr_prof_start_obj, esdr_prof_stop_obj,
	esdr_prof_read_obj, esdr_prof_locks_obj;

#endif /* ESDR_PROF_H */
