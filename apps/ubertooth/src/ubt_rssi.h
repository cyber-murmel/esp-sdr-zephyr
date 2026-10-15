/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from Project Ubertooth, firmware/bluetooth_rxtx/ubertooth_cs.c and ubertooth_rssi.c
 * (Copyright 2015 Hannes Ellinger, GPL version 2 or later, see upstream/ubertooth_interface.h
 * for the licence text), with the changes listed below.
 */
/*
 * RSSI conventions and carrier sense of the Ubertooth firmware on top of the demodulator's
 * symbol power: the raw RSSI byte of the packets, the CC2400 style squelch (SET_SQUELCH /
 * GET_SQUELCH), and the block statistics and per channel average of upstream's
 * ubertooth_rssi.c and ubertooth_cs.c, which this file ports (hence the licence) with these
 * fixes:
 *  - rssi_iir_update() and rssi_get_avg() tested "channel < 2402 || channel < 2480", always
 *    true below 2480, so there was one average for the whole band and channels above 2480
 *    (SET_CHANNEL goes up to 2794) indexed past the 79 entries. Here: below 2402 or above 2480.
 *  - rssi_count wrapped at 256 samples; here it saturates, and the average divides by the true
 *    count.
 *  - The average started at 0 (raw RSSI 0, -54 dBm) and needed a few hundred blocks to get to
 *    the real level, which a relative squelch used meanwhile; here the first block seeds it and
 *    a channel without history reads as a typical noise floor (UBT_RSSI_NOISE_RAW).
 *  - Divisions of negative numbers truncated towards zero (a bias of up to one count for the
 *    average); here they round to nearest.
 *  - The squelch compared against the unrounded requested level while the radio applied the
 *    4 dB rounded one (the CC2400 register); here the rounded level is used for both the
 *    carrier sense and RSSI_TRIGGER.
 *
 * Conversions. The demodulator's pwr (gfsk_sym.h) is 2 * (dBFS + 100); a calibration constant
 * k (half dB) turns it into dBm: dBm_x2 = pwr - 200 + k, set per receive gain. The raw byte of
 * LE and spectrum packets is dBm + 54, as the CC2400 register read. BR_PACKET's byte is mapped
 * by the host with its piecewise cc2400_rssi_to_dbm(); ubt_rssi_br_raw() gives the byte that
 * makes the host print the true dBm.
 */

#ifndef UBT_RSSI_H_
#define UBT_RSSI_H_

#include <stdbool.h>
#include <stdint.h>

/* The 79 BR channels, 2402 to 2480 MHz. */
#define UBT_RSSI_CHANNELS 79U
/* Squelch levels the firmware accepts (dBm); at or below the minimum there is no squelch. */
#define UBT_SQUELCH_MIN (-120)
#define UBT_SQUELCH_MAX (-20)
/* Raw byte of a channel with no history: -100 dBm. */
#define UBT_RSSI_NOISE_RAW (-46)
/*
 * Placeholder calibration, half dB: 0 dBFS at -30 dBm. Unmeasured; replace per gain index with
 * ubt_rssi_set_cal() once the noise floor and a known source are measured.
 */
#define UBT_RSSI_K_DEFAULT (-60)

/**
 * The calibration of the single receiver, applied by ubt_rssi_raw() and friends: dBm in half
 * dB = pwr - 200 + k. A global because the per symbol users hold only a function pointer.
 */
void ubt_rssi_set_cal(int k_half_db);
int ubt_rssi_get_cal(void);

/** Symbol power to dBm in half dB steps. */
int ubt_rssi_dbm_x2(uint8_t pwr);

/** Symbol power to the raw byte of LE and spectrum packets: dBm + 54, saturating. */
int8_t ubt_rssi_raw(uint8_t pwr);

/** dBm to the byte the host maps back to that dBm in BR_PACKETs (within 1 dB, -95 to -30 dBm). */
int8_t ubt_rssi_br_raw_from_dbm(int dbm);

/** Symbol power to that byte. */
int8_t ubt_rssi_br_raw(uint8_t pwr);

/** The host's cc2400_rssi_to_dbm() (host/libubertooth/src/ubertooth_callback.c), unchanged. */
int8_t ubt_rssi_br_to_dbm(int8_t raw);

/** Statistics of the RSSI over one USB block, as upstream's rssi_add(). */
struct ubt_rssi_blk {
	int8_t max, min;
	/* Samples (exact), the reported count saturates at 255. */
	uint16_t n;
	/* Sum of the raw bytes, scaled by 256. */
	int32_t sum;
};

void ubt_rssi_blk_reset(struct ubt_rssi_blk *b);
void ubt_rssi_blk_add(struct ubt_rssi_blk *b, int8_t raw);
uint8_t ubt_rssi_blk_count(const struct ubt_rssi_blk *b);
/** The average, scaled by 256 (0 if empty). */
int32_t ubt_rssi_blk_avg_x256(const struct ubt_rssi_blk *b);

/** Carrier sense and the per channel average of upstream's ubertooth_cs.c and ubertooth_rssi.c. */
struct ubt_rssi {
	/* SET_SQUELCH's value: dBm level, or > 0 for "this many dB over the channel's average". */
	int8_t cs_req;
	/* The level in force: clamped, rounded to the CC2400's 4 dB steps (cs_threshold_cur). */
	int8_t cs_cur;
	bool cs_no_squelch;
	/* Averages of the blocks' mean raw byte per channel, scaled by 256; INT16_MIN: none yet. */
	int16_t iir[UBT_RSSI_CHANNELS];
};

/** cs_reset() and rssi_reset(): no squelch, no history. */
void ubt_rssi_init(struct ubt_rssi *r);

/** SET_SQUELCH: remember the request; ubt_rssi_squelch_update() applies it. */
void ubt_rssi_set_squelch(struct ubt_rssi *r, int8_t req);

/** GET_SQUELCH: the request as made. */
int8_t ubt_rssi_get_squelch(const struct ubt_rssi *r);

/**
 * cs_threshold_calc_and_set(): apply the request for @p channel_mhz (a relative request needs
 * the channel's average). Call after SET_SQUELCH, a channel change and a hop.
 */
void ubt_rssi_squelch_update(struct ubt_rssi *r, uint16_t channel_mhz);

/** The demodulator's squelch threshold (pwr units) for the level in force; 0: no squelch. */
uint8_t ubt_rssi_squelch_pwr(const struct ubt_rssi *r);

/** Raw byte of the threshold in force (cs_cur + 54), as upstream compared rssi_max with. */
int ubt_rssi_squelch_raw(const struct ubt_rssi *r);

/** rssi_iir_update(): fold the block into its channel's average. Empty blocks are skipped. */
void ubt_rssi_iir_update(struct ubt_rssi *r, uint16_t channel_mhz, const struct ubt_rssi_blk *b);

/** rssi_get_avg(): the channel's average raw byte. */
int8_t ubt_rssi_iir_get(const struct ubt_rssi *r, uint16_t channel_mhz);

#endif /* UBT_RSSI_H_ */
