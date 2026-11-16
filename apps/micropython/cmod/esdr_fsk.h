/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The (G)FSK modem of the gnuradio.digital stand-ins: the receiver and modulator of
 * modules/gnuradio/digital/_fsk.py in C, frame for frame the same (the receiver is integer
 * arithmetic throughout; tests/host/test_fsk.py compares the two). Plain C, no Zephyr: the host
 * test builds it too.
 */

#ifndef ESDR_FSK_H_
#define ESDR_FSK_H_

#include <stddef.h>
#include <stdint.h>

#define ESDR_FSK_SPS_MAX 32U
#define ESDR_FSK_LEN_MAX 255U

struct esdr_fsk_frame {
	uint32_t len;     /* payload bytes */
	uint32_t errors;  /* access code bit errors */
	uint32_t end;     /* run sample index of the last payload bit */
	uint32_t phase;   /* sampling phase, 0 .. sps - 1 */
	double dc;        /* carrier offset, binary angle per symbol (65536 a turn) */
	double amp;       /* deviation, binary angle per symbol */
	double var;       /* variance of the access code's symbols around the ideal */
	double power;     /* mean I^2 + Q^2 over the frame */
	uint32_t floor;   /* mean I^2 + Q^2 of the noise before it */
	uint8_t data[ESDR_FSK_LEN_MAX];
};

struct esdr_fsk_rx {
	/* Configuration. */
	uint32_t sps, code_len, threshold, max_len, mask;
	uint64_t code;
	uint32_t trig, trig_mask, trig_errors;
	int32_t csum;
	/* Discriminator and matched filter. */
	int32_t pi, pq, dsum;
	int32_t dring[ESDR_FSK_SPS_MAX];
	uint32_t n, ph;
	int32_t mean[ESDR_FSK_SPS_MAX];
	uint32_t reg[ESDR_FSK_SPS_MAX];
	int64_t fl;
	/* The frame being received. */
	uint32_t state, pend, next, bits, want, acc;
	int64_t num, den;
	uint64_t power;
	uint32_t npower;
	struct esdr_fsk_frame cur;
	/* Matched filter history, mask + 1 entries. */
	int32_t hist[];
};

/* Bytes of a receiver for these settings (the history is a power of two of entries). */
size_t esdr_fsk_rx_size(uint32_t sps, uint32_t code_len);

/* Sets a receiver up in @p size bytes; -1 when the settings or the size do not fit. */
int esdr_fsk_rx_init(struct esdr_fsk_rx *rx, size_t size, uint32_t sps, uint64_t code,
		     uint32_t code_len, uint32_t threshold, uint32_t max_len);

/* A new run: nothing carries over. */
void esdr_fsk_rx_reset(struct esdr_fsk_rx *rx);

typedef void (*esdr_fsk_emit_t)(void *user, const struct esdr_fsk_frame *f);

/* @p n samples (int16 I, Q interleaved); calls @p emit for every frame that ends among them. */
void esdr_fsk_rx_process(struct esdr_fsk_rx *rx, const int16_t *iq, size_t n, esdr_fsk_emit_t emit,
			 void *user);

/* What the receiver does: 0 searching, 1 checking an access code, 2 header, 3 payload. */
uint32_t esdr_fsk_rx_state(const struct esdr_fsk_rx *rx);

/* atan2(im, re) as a binary angle, -32768 .. 32767. */
int32_t esdr_fsk_angle(int32_t re, int32_t im);

/*
 * gfsk_mod: @p nbits bits (MSB first in @p bits) as NRZ, filtered by @p taps (@p ntaps) while
 * interpolating by @p sps, frequency modulated (sensitivity radians per sample per unit),
 * amplitude @p amp of full scale, then the filter flushed. Writes int16 I, Q to @p out (room for
 * @p cap samples) and returns the samples written (0 if they do not fit). @p phase carries the
 * modulator's phase.
 */
size_t esdr_fsk_mod(const uint8_t *bits, size_t nbits, uint32_t sps, const float *taps,
		    size_t ntaps, float sensitivity, float amp, int16_t *out, size_t cap,
		    float *phase);

#endif /* ESDR_FSK_H_ */
