/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interface between the GFSK demodulator (gfsk_demod.c) and its consumers
 * (the BR symbol blocks in ubt_br.c, the BLE packet finder in ubt_le.c): one
 * hard-decision symbol per 1 Msym/s symbol period.
 */

#ifndef GFSK_SYM_H_
#define GFSK_SYM_H_

#include <stdint.h>

/* The 4 MS/s stream: 4 samples per symbol. */
#define GFSK_SAMPLES_PER_SYM 4U

struct gfsk_sym {
	/* Sample index of the stream (4 MS/s units, wraps at 2^32) at the decision instant. */
	uint32_t t;
	/* 1 for a positive frequency deviation (upstream: "a 1 is the higher frequency"). */
	uint8_t bit;
	/*
	 * Signal power of the symbol in 0.5 dB steps: 2 * (dBFS + 100), saturating at 0 and 255,
	 * dBFS relative to a full scale complex tone (int16 samples, 8192 peak per axis, 0 dBFS).
	 */
	uint8_t pwr;
};

#endif /* GFSK_SYM_H_ */
