/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Offsets of struct gfsk_demod's fields for the demodulator's assembly (gfsk_groups.S,
 * gfsk_slice.S); gfsk_demod.c checks every one of them against the struct at build time.
 */

#ifndef GFSK_DEMOD_OFF_H_
#define GFSK_DEMOD_OFF_H_

#define GD_MOD        0
#define GD_ALWAYS     1
#define GD_PWR_THR    2
#define GD_HOLD       4
#define GD_S_CLOSE    12
#define GD_F          20
#define GD_LG         24
#define GD_REPLAY_END 28
#define GD_SAVG       52
#define GD_FLOOR      56
#define GD_S_GATE     60
#define GD_HOLD_CNT   64
#define GD_S_INIT     66
#define GD_OFF        67
#define GD_RUN        68
#define GD_PW_C       69
#define GD_PEND       72
#define GD_SBASE      76
#define GD_EDGE       80
#define GD_PH         84
#define GD_EL_CNT     85
#define GD_EL_E       88
#define GD_EL_A       92
#define GD_M0         96
#define GD_M1         100
#define GD_SEEDED     104
#define GD_CUR        108
#define GD_CUR_VALID  112
#define GD_SLICED     114
#define GD_CRING      116
#define GD_PRING      628
#define GD_WRING      756

#endif /* GFSK_DEMOD_OFF_H_ */
