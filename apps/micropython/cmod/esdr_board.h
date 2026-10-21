/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module's board functions (esdr_board.c), for the module tables.
 */

#ifndef ESDR_BOARD_H
#define ESDR_BOARD_H

#include "py/obj.h"

#include "esdr_defs.h"

/* Clears the home gesture; esdr_port_init() calls it at every (soft) start. */
void esdr_board_init(void);

/* Raises OSError for a negative errno. */
void esdr_check(int ret);

/* The data of the buffer @p obj (@p flags: MP_BUFFER_READ or _WRITE) and its length in bytes;
 * ValueError unless aligned to @p align bytes. */
void *esdr_buffer(mp_obj_t obj, size_t *len, mp_uint_t flags, size_t align);

/* The generator settings of the tuple @p t, (wave, cycles, amp, dc_i, dc_q); ValueError else. */
void esdr_gen_from(struct esdr_gen *g, mp_obj_t t);

extern const mp_obj_fun_builtin_fixed_t esdr_fc32_to_sc16_obj, esdr_sc16_to_fc32_obj,
	esdr_display_size_obj, esdr_reset_reason_obj, esdr_home_obj, esdr_shift_obj;
extern const mp_obj_fun_builtin_var_t esdr_next_app_obj;
/* shell(cmd): with the shell's dummy backend (CONFIG_SHELL_BACKEND_DUMMY). */
extern const mp_obj_fun_builtin_fixed_t esdr_shell_obj;

#endif /* ESDR_BOARD_H */
