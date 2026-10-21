/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The monitor of the radios behind esdr_remote.h (esdr_remote.c on the P4, esdr_local.c on the
 * ESP32): the newest ESDR_MON_SAMPLES pairs that came in (int16 I in the low half, Q in the
 * high), and how many came in all, for esdr_rx_latest() (esdr_rx.h). The backend's receive
 * thread writes, displays read. Plain Zephyr code, built as an app source.
 */

#ifndef ESDR_MON_H_
#define ESDR_MON_H_

#include <stdbool.h>
#include <stdint.h>

#include "esdr_rx.h"

extern uint32_t esdr_mon[ESDR_MON_SAMPLES];
extern volatile uint64_t esdr_mon_total;
/* Set by every esdr_rx_latest(): a display read the monitor. */
extern volatile bool esdr_mon_read;

/* The place of the pair @p k after the newest; esdr_mon_advance() makes them the newest. */
static inline uint32_t *esdr_mon_slot(uint32_t k)
{
	return &esdr_mon[(esdr_mon_total + k) % ESDR_MON_SAMPLES];
}

static inline void esdr_mon_advance(uint32_t n)
{
	esdr_mon_total += n;
}

static inline void esdr_mon_put(int16_t i, int16_t q)
{
	*esdr_mon_slot(0) = (uint16_t)i | ((uint32_t)(uint16_t)q << 16);
	esdr_mon_advance(1);
}

/* 8-bit pairs to 16-bit ones. */
static inline void esdr_widen(int16_t *dst, const int8_t *src, uint32_t n)
{
	for (uint32_t k = 0; k < 2U * n; k++) {
		dst[k] = src[k];
	}
}

/* @p n pairs, int16 (I, Q) or (esdr_mon_put_s8()) int8: the newest of them into the monitor. */
void esdr_mon_put_s16(const int16_t *iq, uint32_t n);
void esdr_mon_put_s8(const int8_t *iq, uint32_t n);

#endif /* ESDR_MON_H_ */
