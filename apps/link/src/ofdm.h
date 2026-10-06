/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OFDM physical layer on raw radio words. Configured by occupied bandwidth,
 * subcarrier count and modulation; the FFT size follows (the power of two
 * that puts that many subcarriers into that bandwidth at the sample rate).
 * Complex baseband: the subcarriers sit on both sides of DC, DC unused.
 *
 * A frame is a Schmidl-Cox preamble (known BPSK on the even subcarriers, so
 * its time halves repeat), a pilot symbol (known BPSK on all, for the channel
 * estimate) and the data symbols, each with a cyclic prefix. The frame is
 * built to loop in the DAC like the QAM frames; the receiver decodes one
 * whole copy from a capture window at least twice the frame long.
 *
 * Carrier offset correction covers +-1 subcarrier spacing.
 */

#ifndef ESDR_LINK_OFDM_H
#define ESDR_LINK_OFDM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OFDM_NFFT_MIN 16U
#define OFDM_NFFT_MAX 512U
/* Shortest cyclic prefix as a fraction of the FFT size (cp_div at most this). */
#define OFDM_CP_DIV_MAX 32U

enum ofdm_mod {
	OFDM_BPSK,
	OFDM_QPSK,
	OFDM_MODS
};

struct ofdm_cfg {
	/* Radio sample rate, Hz. */
	uint32_t sample_rate_hz;
	/* Occupied bandwidth, Hz: rounded so that spacing = sample_rate / nfft. */
	uint32_t bandwidth_hz;
	/* Subcarriers in use (pilot and data), DC excluded. */
	unsigned int channels;
	/* Cyclic prefix: nfft / cp_div samples (4 to OFDM_CP_DIV_MAX). */
	unsigned int cp_div;
	enum ofdm_mod mod;
	/* Transmit RMS amplitude, DAC units (full scale 511). */
	int amp;
	/* Data symbols per frame, 0: as many as fit max_samples. */
	unsigned int symbols;
	/* Longest frame in samples. */
	size_t max_samples;
	/* The receiver samples at sample_rate_hz / rx_div (1 or 2): half the work at 2. */
	unsigned int rx_div;
};

struct ofdm_cf {
	float re, im;
};

/* Subcarriers in use at most: a quarter of the band stays free around Nyquist. */
#define OFDM_CH_MAX (OFDM_NFFT_MAX * 3U / 4U)
/* Power of two above the longest cyclic prefix (nfft / 4). */
#define OFDM_RING 256U

struct ofdm_ctx {
	/* FFT twiddles (esp-dsp radix-2 layout: cos, sin, bit reversed; serves every size up
	 * to nfft) and work buffer.
	 */
	float w[OFDM_NFFT_MAX] __attribute__((aligned(16)));
	struct ofdm_cf buf[OFDM_NFFT_MAX] __attribute__((aligned(16)));
	struct ofdm_cfg cfg;
	/* Derived by ofdm_init(): transmit layout at sample_rate_hz. */
	unsigned int nfft, log2n, cp, ndata;
	size_t slot, len;
	float spacing_hz, bw_hz;
	/* The same frame at the receive rate. */
	unsigned int rnfft, rcp;
	size_t rslot, rlen;
	/* Bit reversal over nfft; per subcarrier (frequency order) its transmit FFT bin and
	 * where it comes out of the (bit reversed) receive FFT.
	 */
	uint16_t rev[OFDM_NFFT_MAX];
	uint16_t bin[OFDM_CH_MAX], binr[OFDM_CH_MAX];
	/* Equalizer per subcarrier, conj(H) / |H|^2, from the last pilot. */
	struct ofdm_cf eq[OFDM_CH_MAX];
	union {
		/* Search: the last preamble metrics (numerator, denominator) over the prefix. */
		struct {
			float rnum[OFDM_RING], rden[OFDM_RING];
		};
		/* Then the carrier offset derotation over one receive FFT window. */
		struct ofdm_cf rot[OFDM_NFFT_MAX];
	};
	/* Transmit samples clipped by the last ofdm_tx_build(). */
	uint32_t clipped;
};

/*
 * Derive the frame layout from @p cfg.
 *
 * @retval 0 on success.
 * @retval -1 if no FFT size fits (bandwidth, channel count, cp_div, frame length).
 */
int ofdm_init(struct ofdm_ctx *ctx, const struct ofdm_cfg *cfg);

static inline unsigned int ofdm_mod_bits(enum ofdm_mod mod)
{
	return mod == OFDM_BPSK ? 1U : 2U;
}

/* Data bits per frame. */
static inline size_t ofdm_frame_bits(const struct ofdm_ctx *ctx)
{
	return (size_t)ctx->ndata * ctx->cfg.channels * ofdm_mod_bits(ctx->cfg.mod);
}

/* Test payload: whitened bits, a function of @p seed (packed, MSB first). */
void ofdm_test_bits(uint8_t *bits, size_t nbits, uint32_t seed);

/* Transmit words: I in bits 9:0, Q in 19:10 (esp_sdr_tx_word()). */
static inline uint32_t ofdm_tx_word(int i, int q)
{
	return ((uint32_t)i & 0x3ffU) | (((uint32_t)q & 0x3ffU) << 10);
}

/* Receive words: Q in bits 9:0, I in 19:10 (esp_sdr_rx_i/q()). */
static inline int ofdm_rx_i(uint32_t w)
{
	return (int32_t)(w << 12) >> 22;
}

static inline int ofdm_rx_q(uint32_t w)
{
	return (int32_t)(w << 22) >> 22;
}

/*
 * Build one frame of ofdm_frame_bits() bits into @p out (ctx->len words).
 *
 * @return ctx->len.
 */
size_t ofdm_tx_build(struct ofdm_ctx *ctx, const uint8_t *bits, uint32_t *out);

struct ofdm_rx_info {
	/* Frame start in the window, sample index. */
	int start;
	/* Preamble metric of that start (normalized correlation, squared). */
	float metric;
	/* Carrier offset: in subcarrier spacings, and in Hz. */
	float cfo_frac, cfo_hz;
	/* Error of the data symbols against their decisions, dB. */
	float mer_db;
	/* Cycles (ofdm_clock): search, channel estimate, data symbols. */
	uint32_t prof[3];
};

/* Forward FFT of ctx->buf in place (output bit reversed, see ctx->rev); for tests. */
void ofdm_test_fft(struct ofdm_ctx *ctx);

/* Optional cycle counter for ofdm_rx_info.prof. */
extern uint32_t (*ofdm_clock)(void);

/*
 * Find a whole frame copy in @p n receive words (at the receive rate) and
 * decode its ofdm_frame_bits() bits into @p bits. The window must hold one
 * whole copy (a looped frame: n >= 2 * ctx->rlen + ctx->rcp).
 *
 * @retval 0 on success.
 * @retval -1 if no preamble was found.
 */
int ofdm_rx_decode(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, uint8_t *bits,
		   struct ofdm_rx_info *info);

#endif
