/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esdr module's board functions (esdr_board.c), for the module tables.
 */

#ifndef ESDR_BOARD_H
#define ESDR_BOARD_H

#include "py/obj.h"

/* Clears the home gesture; esdr_port_init() calls it at every (soft) start. */
void esdr_board_init(void);

extern const mp_obj_fun_builtin_fixed_t esdr_fc32_to_sc16_obj, esdr_sc16_to_fc32_obj,
	esdr_display_size_obj, esdr_reset_reason_obj, esdr_home_obj, esdr_shift_obj;
extern const mp_obj_fun_builtin_var_t esdr_next_app_obj;

#endif /* ESDR_BOARD_H */
