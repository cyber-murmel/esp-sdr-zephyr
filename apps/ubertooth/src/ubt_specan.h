/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Spectrum analyser math of the SPECAN mode, portable. The glue captures 80 MS/s
 * bursts of 16380 words at the LO placements this file plans, feeds them frame by
 * frame, and reads one RSSI byte per MHz channel.
 *
 * Signal path of one LO placement:
 *  1. ubt_specan_dc(): offset of the capture (the LO leakage and the ADC offset sit at DC).
 *  2. ubt_specan_decode(): 256 capture words (esp_sdr_rx_i / esp_sdr_rx_q: I in bits 19:10,
 *     Q in bits 9:0, 10 bit signed, x = I + jQ, a carrier above the LO at positive
 *     frequency) to interleaved int16, offset removed, periodic Hann window, times
 *     2^in_shift.
 *  3. The FFT (struct ubt_specan.fft), in place on 256 complex int16. Contract, that of
 *     esp-dsp dsps_fft2r_sc16_aes3_(data, 256, w) as apps/link/src/ofdm.c fft16() calls it:
 *     natural order in, BIT REVERSED order out, forward (e^-j), every stage halves, so the
 *     output is DFT / 256 (rounded per stage), twiddles ubt_specan_w256 (cos, sin in
 *     bit reversed order, Q15, the table ofdm_init() builds for N = 256). ubt_specan_fft_ref()
 *     is the same algorithm in C (esp-dsp dsps_fft2r_sc16_ansi_), used by the tests.
 *     The glue may replace steps 2 to 4 (a vector unit kernel that windows, transforms and
 *     accumulates): struct ubt_specan only needs acc[] and frames filled.
 *  4. ubt_specan_accum(): |X|^2 into 32 bit accumulators, in the FFT's bit reversed order.
 *     At most UBT_SPECAN_FRAMES_MAX frames per placement fit (a full scale tone fills the
 *     accumulator at in_shift 5).
 *  5. ubt_specan_chan_power(): the channel's power as the sum of the bins it covers,
 *     weighted with the fraction of each 312.5 kHz bin cell inside the 1 MHz channel (about
 *     3.2 bins) and a per bin flatness gain; ubt_specan_channel_raw() turns it into the
 *     RSSI byte.
 *
 * Levels: dBFS is relative to a full scale complex tone (|x| = 512, 10 bit I and Q);
 * the calibration constant K is the dBm of that tone at the antenna for the RX gain
 * in use (cal.fs_dbm_q8), measured with a known source. The byte is dBm + 54 like the
 * CC2400 RSSI register's upper byte.
 */

#ifndef UBT_SPECAN_H_
#define UBT_SPECAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ubt_proto.h"

/* FFT size and capture rate: bins are 312.5 kHz = 5/16 MHz apart. */
#define UBT_SPECAN_N          256U
#define UBT_SPECAN_FS_HZ      80000000U
/* Words of one burst capture (ESP_SDR_SAMPLES_MAX), and the frames of it that do not overlap. */
#define UBT_SPECAN_CAPTURE    16380U
#define UBT_SPECAN_FRAMES_MAX 63U
/* Default input gain 2^4: 63 frames of a full scale tone use a quarter of the accumulator. */
#define UBT_SPECAN_IN_SHIFT   4U
/* Triples {frequency, RSSI} in a SPECAN packet. */
#define UBT_SPECAN_TRIPLES    16U

/* LO placements: usable half span of the analog filter, channels closer than excl to the LO are
 * skipped. */
#define UBT_SPECAN_SPAN_MHZ 28U
#define UBT_SPECAN_EXCL_MHZ 2U
#define UBT_SPECAN_LO_MAX   40U

/* FFT contract (see above): 256 complex int16 pairs in place, natural in, bit reversed out, scaled
 * 1/256. */
typedef void (*ubt_specan_fft_fn)(int16_t *iq);

/* Twiddles for the esp-dsp kernel: w[2i] = cos, w[2i + 1] = sin of 2 pi rev7(i) / 256, Q15. */
extern const int16_t ubt_specan_w256[UBT_SPECAN_N];

/* Reference FFT, bit exact with esp-dsp's C version of the 16 bit kernel. */
void ubt_specan_fft_ref(int16_t *iq);

struct ubt_specan {
	ubt_specan_fft_fn fft;
	/* Input gain 2^in_shift (0 to 5). */
	uint8_t in_shift;
	/* Frames accumulated. */
	uint16_t frames;
	/* |X|^2 summed over the frames, bit reversed bin order. */
	uint32_t acc[UBT_SPECAN_N];
	/* Frame buffer (the PIE kernel wants 16 byte alignment). */
	_Alignas(16) int16_t iq[2U * UBT_SPECAN_N];
};

void ubt_specan_init(struct ubt_specan *s, ubt_specan_fft_fn fft, unsigned int in_shift);

/* Clear the accumulators for the next LO placement. */
void ubt_specan_begin(struct ubt_specan *s);

/* Mean of I and Q over @p n capture words (every 8th is read). */
void ubt_specan_dc(const uint32_t *w, size_t n, int *dc_i, int *dc_q);

/* Capture words to windowed int16 pairs (step 2). @p iq: 256 pairs. */
void ubt_specan_decode(const uint32_t *w, int dc_i, int dc_q, unsigned int in_shift, int16_t *iq);

/* Add |X|^2 of the 256 transformed pairs to @p acc. */
void ubt_specan_accum(uint32_t *acc, const int16_t *iq);

/* One frame: 256 words from @p w. Returns false (nothing done) when UBT_SPECAN_FRAMES_MAX frames
 * are in. */
bool ubt_specan_frame(struct ubt_specan *s, const uint32_t *w, int dc_i, int dc_q);

/*
 * Frames of one capture of @p n words, every @p stride words (256 for none overlapping, 128
 * for Hann 50 % overlap), at most @p max_frames. Returns the frames added.
 */
unsigned int ubt_specan_capture(struct ubt_specan *s, const uint32_t *w, size_t n,
				unsigned int stride, unsigned int max_frames);

/* Calibration of a gain setting. */
struct ubt_specan_cal {
	/* dBm of a full scale tone (0 dBFS), times 256. */
	int32_t fs_dbm_q8;
	/* Power gain per FFT bin in natural order (k, 256 - k for -k), Q12, NULL for flat. */
	const uint16_t *flat;
	/* The spectrum is mirrored (I and Q swapped): offset d is read at -d. */
	bool mirror;
};

/*
 * Sum over the channel centred @p offset_mhz from the LO of accumulator times fraction of
 * the bin cell inside the channel (0 to 10 per bin) times the flatness gain: about 32
 * times the power of one bin for a flat spectrum.
 */
uint64_t ubt_specan_chan_power(const struct ubt_specan *s, int offset_mhz,
			       const struct ubt_specan_cal *cal);

/* Power in dB * 256 relative to a full scale tone for a ubt_specan_chan_power() value; INT32_MIN
 * for 0. */
int32_t ubt_specan_dbfs_q8(uint64_t power, unsigned int frames, unsigned int in_shift);

/* RSSI byte (dBm + UBT_RSSI_OFFSET, saturating to int8) of a channel. */
int8_t ubt_specan_channel_raw(const struct ubt_specan *s, int offset_mhz,
			      const struct ubt_specan_cal *cal);

/*
 * Calibration from one measurement: the accumulated frames of a known source in the channel at
 * @p offset_mhz (a signal generator at @p known_dbm_q8, dBm times 256, at the antenna with the gain
 * in use). Returns the fs_dbm_q8 that makes this channel read the source; the rest of the band
 * follows from the flatness table. INT32_MIN if the channel has no power.
 */
int32_t ubt_specan_cal_fs_dbm(const struct ubt_specan *s, int offset_mhz,
			      const struct ubt_specan_cal *cal, int32_t known_dbm_q8);

/* ---------------------------------------------------------------- planning */

struct ubt_specan_cfg {
	/* Usable half span and skipped half width of a placement (UBT_SPECAN_SPAN_MHZ, _EXCL_MHZ).
	 */
	uint8_t span_mhz, excl_mhz;
	/* Tunable range of the LO in MHz. */
	uint16_t lo_min_mhz, lo_max_mhz;
};

void ubt_specan_cfg_default(struct ubt_specan_cfg *cfg);

struct ubt_specan_plan {
	uint16_t low, high;
	uint8_t n_lo;
	uint8_t span_mhz, excl_mhz;
	uint16_t lo_mhz[UBT_SPECAN_LO_MAX];
};

/*
 * LO placements (whole MHz) so that every channel of [low, high] has a placement with
 * excl <= |channel - LO| <= span; the channels near one LO are read from another. @p cfg
 * may be NULL for the defaults. Returns 0, -EINVAL for low > high or an empty range, -ERANGE if
 * the LO range or UBT_SPECAN_LO_MAX does not allow it.
 */
int ubt_specan_plan(struct ubt_specan_plan *plan, uint16_t low, uint16_t high,
		    const struct ubt_specan_cfg *cfg);

/* Index of the placement channel @p mhz is read from (nearest the middle of the usable window), or
 * -1. */
int ubt_specan_lo_for(const struct ubt_specan_plan *plan, uint16_t mhz);

/*
 * Read the channels assigned to placement @p lo_idx from @p s into raw[mhz - plan->low]
 * (high - low + 1 bytes). Returns the channels written.
 */
unsigned int ubt_specan_collect(const struct ubt_specan *s, const struct ubt_specan_plan *plan,
				unsigned int lo_idx, const struct ubt_specan_cal *cal, int8_t *raw);

/* ----------------------------------------------------------------- packets */

/*
 * SPECAN packets as upstream specan() makes them: triples {frequency high, frequency
 * low, RSSI byte} in ascending order, 16 per packet. A partial packet is kept across
 * sweeps (upstream's counter does not reset at the end of a sweep).
 */
struct ubt_specan_packer {
	usb_pkt_rx pkt;
	uint8_t n;
	/* clkn_high and clk100ns of the packet when it is completed (upstream: when it is queued).
	 */
	void (*clock)(void *user, uint8_t *clkn_high, uint32_t *clk100ns);
	/* A complete packet; copy it before returning. */
	void (*emit)(void *user, const usb_pkt_rx *pkt);
	void *user;
};

void ubt_specan_packer_init(struct ubt_specan_packer *p,
			    void (*clock)(void *, uint8_t *, uint32_t *),
			    void (*emit)(void *, const usb_pkt_rx *), void *user);

/* Add one triple; calls emit() when it was the 16th. */
void ubt_specan_pack(struct ubt_specan_packer *p, uint16_t mhz, int8_t rssi);

/* The channels low to high of a sweep in ascending order. */
void ubt_specan_pack_sweep(struct ubt_specan_packer *p, uint16_t low, uint16_t high,
			   const int8_t *raw);

/* -------------------------------------------------------------- cost model */

/*
 * Work per 256 sample frame (multiplications, the target does them 1 per instruction in C or 4 to 8
 * lanes per instruction on the vector unit): decode 512 (window), FFT 4096 (8 stages x 128
 * butterflies x 4) with 6144 additions, accumulate 512 (|X|^2) with 256 additions; per sweep about
 * 5 x 3 multiplications and two table interpolations per channel. Instruction counts of the C
 * loops, Xtensa gcc -O2: decode 20 per sample, accumulate 10 per bin, i.e. 5.1 k and 2.6 k cycles.
 */

/* Times in us for the sweep rate estimate. */
struct ubt_specan_model {
	/* Retune (esp_sdr_set_freq() with its settle) and one burst capture. */
	uint32_t retune_us, capture_us;
	/* Per frame: decode and window, FFT, power accumulation. */
	uint32_t decode_us, fft_us, accum_us;
	/* Per sweep: channel binning, dB conversion and packing (hidden when the DSP overlaps). */
	uint32_t sweep_us;
	/* The DSP of a placement runs while the next one retunes and captures (other CPU or
	 * interleaved). */
	bool overlap;
};

/*
 * Time of one sweep of @p n_lo placements with @p frames frames each: with overlap
 * the longest of retune + capture and the DSP per placement, else their sum
 * and the per sweep work.
 */
uint32_t ubt_specan_sweep_us(unsigned int n_lo, unsigned int frames,
			     const struct ubt_specan_model *m);

#endif /* UBT_SPECAN_H_ */
