/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFSK modulator for the transmit DAC (BT 0.5, 1 Msym/s) of the Ubertooth TX
 * modes: BLE advertising (h 0.5), BR (h about 0.32), TX_GENERIC and TX_TEST
 * with a CC2400 deviation.
 *
 * DAC conventions (derived from lib/ieee802154/ieee154_dsp.h and the
 * osmosdr transmit path):
 *  - Words are esp_sdr_tx_word() words at the 40 MS/s DAC rate: I in bits
 *    9:0, Q in bits 19:10, 10 bit two's complement. 40 samples per symbol.
 *  - A baseband phase that grows (I + jQ turning counter clockwise) puts the
 *    carrier above the LO. The wanted channel f0 is therefore sent with the LO
 *    GFSK_MOD_IF_HZ below it (esp_sdr_set_freq(f0 - 4)), as the 802.15.4
 *    driver does with LO_OFFSET_MHZ 4: the LO leakage then sits 4 MHz and the
 *    image 8 MHz away from the channel (osmosdr measured about 40 and 43 dB down).
 *    A 4 MHz IF is a whole number of periods (10 samples) per symbol period
 *    (40 samples). A bit 1 is the higher frequency (positive deviation).
 *  - The amplitude is a table property (gfsk_lut_init()), the envelope is
 *    constant: the analog TX compresses amplitude, so keep it moderate (the
 *    802.15.4 PHY uses 300 of 511) and set the output power with the TX gain
 *    step (esp_sdr_tx_set_gain()).
 *
 * Design: the phase of symbol s at sample k is
 *   IF(40 s + k) + D sum_{j<s-1} b_j + D (b[s-1] A[k] + b[s] B[k] + b[s+1] C[k])
 * with b = +-1 (0 outside a burst: carrier at the centre), D the phase per bit
 * (deviation / symbol rate, in turns) and A, B, C thirds of the integrated
 * Gaussian frequency pulse (three symbols long, truncated; the pulse is
 * centred on its own bit, so a symbol depends on its two neighbours). The
 * 3 bit history picks one of 8 rows of per sample phase steps (16 bit
 * fraction of a turn, IF included): the hot loop per sample is a load, an add,
 * a shift and mask, a table lookup and a store. The accumulator restarts at
 * every symbol from exact 32 bit values, so the words are a pure function of
 * the sample index: any chunking of fill(), and overlapping or repeated
 * gen() calls (the DAC filler asks for overlapping ranges, possibly from
 * both CPUs), give identical words. Only the tables are shared, read only.
 *
 * Memory: struct gfsk_tab 680 bytes per deviation and IF, struct gfsk_lut 4 << GFSK_LUT_BITS + 4
 * bytes (2 KB at the default 9 bits: error vector -49 dB against the ideal GFSK, below the DAC and
 * the analog chain; 8 bits: 1 KB, -44 dB; 10 bits: 4 KB, -53 dB), const Gaussian and quarter wave
 * tables about 760 bytes in flash. Code in IRAM (UBT_HOT): about 0.8 KB, the rest of the file may
 * stay in flash.
 *
 * Speed: Xtensa gcc -O2 compiles the steady state (gen_steady()) to 5.5 instructions per sample in
 * the unrolled sample loop and 46 per symbol around it, 6.7 per sample in all (a static count, one
 * cycle each is the estimate: 36 MS/s per 240 MHz core, 40 MS/s needs 267 MHz). The first block of
 * a DAC session (16380 samples) is therefore ready after 0.46 ms, a 22000 sample TX_GENERIC packet
 * needs 0.23 ms for the 8300 samples of its second block while the first plays for 0.36 ms. A BLE
 * advertising packet with ramps is at most 15360 samples, one bank.
 */

#ifndef GFSK_MOD_H_
#define GFSK_MOD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Output rate and symbol timing. */
#define GFSK_MOD_RATE_HZ 40000000U
#define GFSK_MOD_SYM_HZ  1000000U
#define GFSK_MOD_SPS     (GFSK_MOD_RATE_HZ / GFSK_MOD_SYM_HZ)

/* The IF of the existing TX paths: LO = channel - 4 MHz, carrier at +4 MHz in the DAC words. */
#define GFSK_MOD_IF_HZ 4000000

/* The LO sits this far below the channel: esp_sdr_set_freq(channel - GFSK_MOD_LO_OFFSET_MHZ). */
#define GFSK_MOD_LO_OFFSET_MHZ ((unsigned int)(GFSK_MOD_IF_HZ / 1000000))

/* Default digital amplitude (of 511) and ramp. */
#define GFSK_MOD_AMP      300
#define GFSK_MOD_RAMP_MAX 255U

/* Deviations: the CC2400 MDMCTRL values 0x40, 0x29 and 0x57 (3906.25 Hz per step). */
#define GFSK_DEV_BLE_HZ     250000
#define GFSK_DEV_BR_HZ      160156
#define GFSK_DEV_GENERIC_HZ 339844

/* Size of the sine table, a power of two of 2^5 to 2^10 entries. */
#ifndef GFSK_LUT_BITS
#define GFSK_LUT_BITS 9U
#endif
#define GFSK_LUT_SIZE (1U << GFSK_LUT_BITS)

/* DAC word (esp_sdr_tx_word()). */
static inline uint32_t gfsk_word(int32_t i, int32_t q)
{
	return ((uint32_t)i & 0x3ffU) | (((uint32_t)q & 0x3ffU) << 10);
}

/* Sine table: one DAC word per phase step, amplitude baked in. */
struct gfsk_lut {
	int16_t amp;
	uint32_t w[GFSK_LUT_SIZE];
};

/*
 * Fill @p lut with amp * exp(+j 2 pi i / size); @p conj mirrors the spectrum
 * (the 802.15.4 driver's tx_conj diagnostic). @p amp 1 to 511.
 */
void gfsk_lut_init(struct gfsk_lut *lut, int amp, bool conj);

/* Phase steps of one deviation and IF. */
struct gfsk_tab {
	/* Step after sample k of a symbol, history prev, cur, next (bit 2, 1, 0), 1/65536 turn. */
	int16_t inc[8][GFSK_MOD_SPS];
	/* Phase at the symbol start for the history, 2^-32 turn. */
	int32_t phi0[8];
	/* Phase per bit and per output sample (IF), 2^-32 turn. */
	int32_t bit_step;
	int32_t if_inc;
};

/*
 * Tables for @p dev_hz peak deviation (long run of equal bits) and carrier
 * offset @p if_hz (+-19 MHz) from the LO. Integer arithmetic only.
 * CC2400 MDMCTRL[6:0] steps are 3906.25 Hz: see gfsk_dev_mdmctrl().
 */
void gfsk_tab_init(struct gfsk_tab *tab, int32_t dev_hz, int32_t if_hz);

/* Deviation of the CC2400 MDMCTRL register in Hz (bits 6:0 times 3906.25, rounded). */
static inline int32_t gfsk_dev_mdmctrl(uint16_t mdmctrl)
{
	return (int32_t)(((mdmctrl & 0x7fU) * 15625U + 2U) / 4U);
}

/* What follows the symbols of a burst: go to the centre frequency, or hold the last bit's. */
enum gfsk_end {
	GFSK_END_CENTRE,
	GFSK_END_HOLD,
};

enum gfsk_kind {
	/* Lead symbols, the bits, trail symbols; amplitude ramps at both ends. */
	GFSK_BURST,
	/* The bits as a periodic pattern (a loop bank): no ramps, bit -1 is the last, bit n the
	 * first. */
	GFSK_LOOP,
	/* Unmodulated carrier at the IF, @c nbits symbols long (period of a loop bank). */
	GFSK_CARRIER,
};

struct gfsk_burst {
	enum gfsk_kind kind;
	/* Bits, packed: bit i is (bits[i / 8] >> (i % 8)) & 1, or >> (7 - i % 8) for msb_first. */
	const uint8_t *bits;
	uint32_t nbits;
	bool msb_first;
	enum gfsk_end end;
	/* Unmodulated carrier symbols before and after the bits (burst only; trail 2 lets the pulse
	 * finish). */
	uint16_t lead, trail;
	/* Raised cosine amplitude ramp in samples at each end, 0 to GFSK_MOD_RAMP_MAX (burst only).
	 */
	uint16_t ramp;
};

struct gfsk_mod {
	const struct gfsk_tab *tab;
	const struct gfsk_lut *lut;
	struct gfsk_burst b;
	/* Samples of the burst, or the period of a loop or carrier. */
	uint32_t total;
	/* Next sample of gfsk_mod_fill(). */
	uint32_t pos;
	/* Grid steps of the ramp's cosine per ramp sample (Q16), see ramp_gain(). */
	uint32_t ramp_step;
};

/*
 * Prepare a burst. The bit array is not copied and must stay valid. Returns
 * the samples of the burst (period for loop and carrier), 0 on bad
 * arguments (no bits for a burst or loop, too many symbols).
 */
uint32_t gfsk_mod_start(struct gfsk_mod *m, const struct gfsk_tab *tab, const struct gfsk_lut *lut,
			const struct gfsk_burst *b);

/* @return The first sample of the first bit (lead symbols, in samples). */
static inline uint32_t gfsk_mod_data_start(const struct gfsk_mod *m)
{
	return m->b.kind == GFSK_BURST ? (uint32_t)m->b.lead * GFSK_MOD_SPS : 0U;
}

/*
 * DAC words for samples [index, index + n): an esp_sdr_tx_dac_gen_t body (the
 * words are a pure function of the index, @p m is not written). A burst is
 * zero (esp_sdr_tx_word(0, 0)) beyond its end, a loop or carrier wraps at the period.
 */
void gfsk_mod_gen(const struct gfsk_mod *m, uint32_t *dst, uint64_t index, uint32_t n);

/* gfsk_mod_gen() with the esp_sdr_tx_dac_gen_t signature; @p user is the struct gfsk_mod. */
void gfsk_mod_gen_cb(void *user, uint32_t *dst, uint64_t index, uint32_t n);

/*
 * The next @p n words from the cursor, which then advances. Returns the words
 * written: n, or fewer at the end of a burst (the rest of @p dst is untouched);
 * a loop and a carrier never end.
 */
size_t gfsk_mod_fill(struct gfsk_mod *m, uint32_t *dst, size_t n);

/* Back to the first sample. */
static inline void gfsk_mod_rewind(struct gfsk_mod *m)
{
	m->pos = 0;
}

/* @return Whether a burst has been read to its end by gfsk_mod_fill(). */
static inline bool gfsk_mod_done(const struct gfsk_mod *m)
{
	return m->b.kind == GFSK_BURST && m->pos >= m->total;
}

/*
 * Phase step table of a symbol with neighbours @p bp, @p bc, @p bn (-1, 0 or
 * +1 each): the row gfsk_tab_init() makes for the 8 binary histories, and what
 * a burst's lead and trail symbols use. Phase at the symbol start in @p phi0.
 */
void gfsk_tab_row(const struct gfsk_tab *tab, int bp, int bc, int bn, int16_t row[GFSK_MOD_SPS],
		  int32_t *phi0);

#endif /* GFSK_MOD_H_ */
