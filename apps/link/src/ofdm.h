/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OFDM physical layer on raw radio words. Configured by occupied bandwidth,
 * subcarrier count and cyclic prefix; the FFT size follows (the power of two
 * that puts that many subcarriers into that bandwidth at the sample rate).
 * Complex baseband: the subcarriers sit on both sides of DC, DC unused. The
 * modulation (BPSK to 64-QAM) is chosen per frame.
 *
 * A frame is a Schmidl-Cox preamble (known BPSK on the even subcarriers, so
 * its time halves repeat), one or more pilot symbols (known BPSK on all, for
 * the channel estimate), optional header symbols (BPSK, the header bits
 * repeated over all their subcarriers) and the payload symbols, each with a
 * cyclic prefix. The frame is built to loop in the DAC like the QAM frames;
 * the receiver decodes one whole copy from a capture window at least twice
 * the frame long.
 *
 * Carrier offset correction covers +-1 subcarrier spacing.
 */

#ifndef ESDR_LINK_OFDM_H
#define ESDR_LINK_OFDM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Receive and transmit words: esp_sdr_rx_i/q(), esp_sdr_tx_word(). */
#include <esp_sdr/esp_sdr.h>

#define OFDM_NFFT_MIN 16U
/* 256 covers every layout in use (26 MHz at 80 MS/s); 512 would cost 8 KB of internal RAM. */
#define OFDM_NFFT_MAX 256U
/* Shortest cyclic prefix as a fraction of the FFT size (cp_div at most this). */
#define OFDM_CP_DIV_MAX 32U
/* Subcarriers in use at most: a quarter of the band stays free around Nyquist. */
#define OFDM_CH_MAX (OFDM_NFFT_MAX * 3U / 4U)
/* Power of two above the longest cyclic prefix (nfft / 4). */
#define OFDM_RING 256U
#define OFDM_PILOTS_MAX 4U
#define OFDM_HDR_BITS_MAX 64U

enum ofdm_mod {
	OFDM_BPSK,
	OFDM_QPSK,
	OFDM_16QAM,
	OFDM_64QAM,
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
	/* Transmit RMS amplitude, DAC units (full scale 511). */
	int amp;
	/* Symbols after the pilots (header and payload), 0: as many as fit max_samples. */
	unsigned int symbols;
	/* Longest frame in samples. */
	size_t max_samples;
	/* The receiver samples at sample_rate_hz / rx_div (1 or 2): half the work at 2. */
	unsigned int rx_div;
	/* Pilot symbols (1 to OFDM_PILOTS_MAX), averaged for the channel estimate. */
	unsigned int pilots;
	/* Also smooth the estimate over neighbouring subcarriers (3 taps). */
	bool smooth;
	/* Header bits per frame (0 to OFDM_HDR_BITS_MAX), on BPSK symbols before the payload. */
	unsigned int hdr_bits;
	/*
	 * Another pilot symbol in the middle of the payload: the phase is
	 * measured again there, so the second half can be decoded on its own
	 * (on another CPU) and a long frame does not drift.
	 */
	bool mid_pilot;
};

struct ofdm_cf {
	float re, im;
};

/*
 * One receiver working through symbols: its FFT buffer, the tracked common
 * phase and its rate per slot (a residual carrier offset), and the known
 * phase of its current window (u). Two of them decode one frame on two CPUs.
 */
struct ofdm_worker {
	struct ofdm_cf buf[OFDM_NFFT_MAX] __attribute__((aligned(16)));
	struct ofdm_cf trk, rate, u;
	/* The slot in the buffer was read one period earlier: u includes rx.cw. */
	bool wrapped;
	/* The slot in the buffer, and when it was captured in slots from the frame start (one
	 * period earlier if wrapped).
	 */
	unsigned int slot;
	float tslot;
	/*
	 * The vector decoder's equalizer turned against the sample clock's drift (see
	 * rx.dslot), rebuilt when the drift has moved: for which frame, equalizer and phase.
	 */
	int16_t e16[OFDM_CH_MAX][2] __attribute__((aligned(16)));
	const void *e16_src;
	uint32_t e16_gen;
	float e16_th;
	/* The buffer holds the 16-bit FFT's output (payload symbols), else the float FFT's. */
	bool q16;
};

/* Receive state between ofdm_rx_begin() and ofdm_rx_finish(). */
struct ofdm_rxstate {
	int start, dci, dcq;
	/* The window (receive words) and the frame's period in it: a slot past the end is read one period earlier. */
	int n, per;
	/* The carrier's turn over one period, from the window's overlap with itself: a slot read
	 * one period earlier is turned back by this.
	 */
	struct ofdm_cf cw;
	/*
	 * Sample clock drift: subcarrier k of slot s is turned back by exp(j k dslot (s - sref)),
	 * the window's growing timing error against the pilots' (both crystals drive their
	 * chip's carrier and sample clock, so the drift follows the carrier offset).
	 */
	float dslot, sref;
	uint32_t gen;
	float eps;
	/* Step of the window phase per slot. */
	struct ofdm_cf us;
	/* How early the FFT window starts, from the pilots' phase slope (samples). */
	float delay;
	/* First payload slot. */
	unsigned int slot;
	float soft[OFDM_HDR_BITS_MAX];
};

struct ofdm_ctx {
	/* FFT twiddles (esp-dsp radix-2 layout: cos, sin, bit reversed; serves every size up
	 * to nfft).
	 */
	float w[OFDM_NFFT_MAX] __attribute__((aligned(16)));
	/* The same table in Q15 for the 16-bit receive FFT, and the factor back to the float FFT. */
	int16_t w16[OFDM_NFFT_MAX] __attribute__((aligned(16)));
	float fscale;
	/* The receiver's own worker (search, channel, header, payload). */
	struct ofdm_worker w0;
	struct ofdm_cfg cfg;
	/* Derived by ofdm_init(): transmit layout at sample_rate_hz; the payload symbol the
	 * mid pilot comes before (ndata if none).
	 */
	unsigned int nfft, log2n, cp, nhdr, ndata, mid;
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
	/* Equalizer per subcarrier, conj(H) / |H|^2, from the last pilots. */
	struct ofdm_cf eq[OFDM_CH_MAX];
	union {
		/* Search: the last preamble metrics (numerator, denominator) over the prefix. */
		struct {
			float rnum[OFDM_RING], rden[OFDM_RING];
		};

		/* Then the carrier offset derotation over one receive FFT window. */
		struct ofdm_cf rot[OFDM_NFFT_MAX];
	};
	/* The same in Q15 for the vector derotation of the payload symbols. */
	int16_t rot16[OFDM_NFFT_MAX][2] __attribute__((aligned(16)));
	struct ofdm_rxstate rx;
	/* The channel averaged over frames (ofdm_chavg), and how many frames are in it (0: none). */
	struct ofdm_cf havg[OFDM_CH_MAX];
	uint32_t havg_n;
	/* Diagnostic: when set, the payload decoder adds each subcarrier's squared error here. */
	float *sc_err;
	/*
	 * The equalizer for the vector payload decoder, per modulation (16-QAM, 64-QAM): conj(H)
	 * times the receive FFT's scale times the factor that makes the constellation spacing
	 * 256, in 16 bit times 2^sar (sar 0: does not fit, the float decoder is used).
	 */
	struct ofdm_veq {
		int16_t e[OFDM_CH_MAX][2] __attribute__((aligned(16)));
		int16_t sar;
	} veq[2];
};

/* A transmit work buffer (ofdm_tx_build()), separate so that building and decoding can overlap. */
struct ofdm_txbuf {
	struct ofdm_cf buf[OFDM_NFFT_MAX] __attribute__((aligned(16)));
	/* Samples clipped by the last build. */
	uint32_t clipped;
	/* ofdm_frame_build()'s stages in ofdm_clock() cycles: the coding (CRC, FEC), the symbols. */
	uint32_t prof[2];
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
	static const uint8_t bits[OFDM_MODS] = {1, 2, 4, 6};

	return mod < OFDM_MODS ? bits[mod] : 0U;
}

/* Payload bits per frame at @p mod. */
static inline size_t ofdm_frame_bits(const struct ofdm_ctx *ctx, enum ofdm_mod mod)
{
	return (size_t)ctx->ndata * ctx->cfg.channels * ofdm_mod_bits(mod);
}

/* Length of a header-only frame (preamble, pilots, header), at the transmit and receive rate. */
static inline size_t ofdm_hdr_samples(const struct ofdm_ctx *ctx)
{
	return (size_t)(1U + ctx->cfg.pilots + ctx->nhdr) * ctx->slot;
}

static inline size_t ofdm_hdr_rsamples(const struct ofdm_ctx *ctx)
{
	return (size_t)(1U + ctx->cfg.pilots + ctx->nhdr) * ctx->rslot;
}

/*
 * Receive words that hold a looping frame of @p period receive samples whole: the frame
 * loops without a gap, so one period anywhere holds every sample once (rotated); one FFT, a
 * prefix and the coarse search's step more let the search see a start and every slot lie
 * whole in the window or one period earlier.
 */
#define OFDM_RX_WRAP_MARGIN(nfft, cp) ((nfft) + (cp) + 16U)
static inline size_t ofdm_rx_window(const struct ofdm_ctx *ctx, size_t period)
{
	return period + OFDM_RX_WRAP_MARGIN(ctx->rnfft, ctx->rcp);
}

/* Test payload: whitened bits, a function of @p seed (packed, MSB first). */
void ofdm_test_bits(uint8_t *bits, size_t nbits, uint32_t seed);

/*
 * Build one frame into @p out (ctx->len words): cfg.hdr_bits header bits from
 * @p hdr, ofdm_frame_bits(mod) payload bits from @p bits (packed, MSB first).
 * With @p bits NULL a header-only frame (ofdm_hdr_samples() words). Only
 * reads @p ctx.
 *
 * @return Its length in samples, 0 for a bad modulation.
 */
size_t ofdm_tx_build(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, enum ofdm_mod mod,
		     const uint8_t *hdr, const uint8_t *bits, uint32_t *out);

/*
 * Frames built with @p tb (one work buffer, in internal RAM) take help from other CPUs: a
 * thread there that calls ofdm_tx_help() builds symbols of the frame while it is being built.
 * The 16-bit transmit path (CONFIG_APP_OFDM_ESP_DSP) only. The hand-over uses compare and
 * swap (S32C1I), which is atomic in internal RAM only: not for a build whose statics are in
 * PSRAM (a multiapp extension). Other builds use no atomics.
 */
void ofdm_tx_share(struct ofdm_txbuf *tb);

/*
 * Wait for the next shared build (ofdm_tx_share()) and build its symbols with it, in @p tb's
 * buffer (the caller's own, internal RAM), until none are left: returns then, or once
 * @p keep_going(@p arg) is false (asked before each symbol, about 30 us apart; it may spin
 * until then).
 */
void ofdm_tx_help(struct ofdm_txbuf *tb, bool (*keep_going)(void *), void *arg);

struct ofdm_rx_info {
	/* Frame start in the window, sample index. */
	int start;
	/* Preamble metric of that start (normalized correlation, squared). */
	float metric;
	/* Carrier offset: in subcarrier spacings, and in Hz. */
	float cfo_frac, cfo_hz;
	/* Error of the payload symbols against their decisions, dB. */
	float mer_db;
	/* Cycles (ofdm_clock): search, channel estimate and header, payload. */
	uint32_t prof[3];
};

/* The vector payload decoder (16-QAM and 64-QAM with the 16-bit FFT); false: the float one. */
extern bool ofdm_vec_enable;
/* The payload derotation on the vector unit (ESP32-S3, 16-bit FFT path). */
extern bool ofdm_vec_derot;
/* The channel averaged over frames (default off: no gain measured on the bench); fine timing by turning the channel, not measuring it again (default on). */
extern bool ofdm_chavg;
extern bool ofdm_finerot;
/* Carrier frequency (for the sample clock drift) and the drift correction's sign (0: off). */
extern float ofdm_carrier_hz;
extern int ofdm_sfo;

/* The best metric of a preamble scan (see ofdm_sc_scan()). */
struct ofdm_sc_best {
	float num, den;
	int32_t pr, pi;
	int t, start;
};

/*
 * Schmidl-Cox scan over t = t0, t0 + st, .. t1 (the coarse scan of the preamble search, st > 1).
 * The search splits it in two when ofdm_scan_async is set: that starts the scan of
 * [t0, t1] on another CPU (it calls ofdm_sc_scan() there) and ofdm_scan_wait() returns once it
 * is done. Both stay NULL when there is no second CPU to use.
 */
void ofdm_sc_scan(struct ofdm_ctx *ctx, const uint32_t *w, int t0, int t1, int st, int dci,
		  int dcq, struct ofdm_sc_best *b);
extern void (*ofdm_scan_async)(struct ofdm_ctx *ctx, const uint32_t *w, int t0, int t1, int st,
				int dci, int dcq, struct ofdm_sc_best *b);
extern void (*ofdm_scan_wait)(void);

/* Optional cycle counter for ofdm_rx_info.prof. */
extern uint32_t (*ofdm_clock)(void);

/*
 * Find a whole frame copy in @p n receive words (at the receive rate), set up
 * the channel and decode the header into @p hdr (cfg.hdr_bits bits). The frame
 * loops: n >= ofdm_rx_window(ctx, ctx->rlen) holds it (read around the period).
 *
 * @retval 0 on success.
 * @retval -1 if no preamble was found.
 */
int ofdm_rx_begin(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, uint8_t *hdr,
		  struct ofdm_rx_info *info);

/* The same for a frame looping with @p period receive samples (header-only: ofdm_hdr_rsamples()). */
int ofdm_rx_begin_period(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, size_t period,
			 uint8_t *hdr, struct ofdm_rx_info *info);

/* Then the ofdm_frame_bits(mod) payload bits into @p bits, from the same words. */
void ofdm_rx_finish(struct ofdm_ctx *ctx, const uint32_t *words, enum ofdm_mod mod, uint8_t *bits,
		    struct ofdm_rx_info *info);

/*
 * The payload in two parts, e.g. on two CPUs: ctx->w0 decodes payload
 * symbols 0 .. mid - 1, a second worker set up by ofdm_rx_fork() (which
 * measures the phase on the mid pilot) symbols mid .. ndata - 1.
 * ofdm_rx_part() decodes s0 .. s1 - 1 into @p bits (the frame's bits from
 * symbol s0 on; mid starts a byte) and adds their squared error to @p err.
 * Only the workers are written.
 */
void ofdm_rx_fork(const struct ofdm_ctx *ctx, struct ofdm_worker *wk, const uint32_t *words);
void ofdm_rx_part(const struct ofdm_ctx *ctx, struct ofdm_worker *wk, const uint32_t *words,
		  enum ofdm_mod mod, unsigned int s0, unsigned int s1, uint8_t *bits, float *err);

/* MER of a payload from its squared error. */
float ofdm_rx_mer(const struct ofdm_ctx *ctx, float err);

/* Forward FFT of the receive buffer in place (output bit reversed, see ctx->rev); for tests. */
void ofdm_test_fft(struct ofdm_ctx *ctx);

#endif
