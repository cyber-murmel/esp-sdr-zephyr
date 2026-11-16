/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The stream rates of the radio on the chip (esdr_local.c), fastest first, as esdr.RATES gives
 * them (esdr_radio.c): every rate its chain reaches exactly, in steps of 1.25 to 2.
 *
 * - ESP32-S31: 20 MS/s bursts, the quarter-rate rotation, a CIC of ratio m and the half-band:
 *   10 MHz / m, m from 2 to 80 (at 2 and 4 the LO's DC still lies off the band or on the CIC's
 *   null).
 * - ESP32 (the MCH2022 badge): 16 MS/s bursts through the library's CIC of ratio m: 16 MHz / m,
 *   m from 8 to 128.
 */

#ifndef ESDR_RATES_H_
#define ESDR_RATES_H_

#if defined(CONFIG_SOC_SERIES_ESP32S31)
#define ESDR_LOCAL_RATES(X)                                                                        \
	X(5000000) X(2500000) X(2000000) X(1250000) X(1000000) X(625000) X(500000) X(400000)      \
	X(312500) X(250000) X(200000) X(156250) X(125000)
#else
#define ESDR_LOCAL_RATES(X)                                                                        \
	X(2000000) X(1600000) X(1000000) X(800000) X(640000) X(500000) X(400000) X(320000)        \
	X(250000) X(200000) X(160000) X(125000)
#endif

#endif /* ESDR_RATES_H_ */
