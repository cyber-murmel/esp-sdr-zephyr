/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MODE_RX_SYMBOLS and MODE_AFH (upstream bt_stream_rx()): the Bluetooth BR symbol stream of
 * ubertooth-rx, -scan, -afh and -dump -b. The 4 MS/s stream goes through the demodulator in BR
 * mode, its symbols into 400 symbol BR_PACKETs (ubt_br) with the RSSI fields and the carrier
 * sense flags of SET_SQUELCH (ubt_rssi).
 *
 * The channel stays put or moves as upstream's timer moves it: SET_CHANNEL 9999 sweeps the band
 * 32 MHz at a time every 128 CLKN ticks (40 ms, the 25 Hz sweep; channels missing from an AFH
 * map are passed over), and the AFH mode visits the channels its map does not hold yet every
 * 158 ticks (49 ms). The Bluetooth hop sequence (1600 hops/s, START_HOPPING) is out of reach of
 * a 3 to 4 ms retune: MODE_BT_FOLLOW stalls. Upstream's AFH mode never ran its loop (the loop
 * condition lists RX_SYMBOLS and BT_FOLLOW only); here it streams.
 *
 * Squelch: the demodulator runs behind its own squelch, at least SQUELCH_MARGIN over the noise
 * floor, so the symbols of the noise between packets are not computed (the CC2400 streamed them;
 * no access code search finds anything in them). The blocks of that time carry idle symbols, and
 * after IDLE_SEND of them in a row they are not sent at all (ubt_br.h).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ubt_br.h"
#include "ubt_mode.h"
#include "ubt_radio.h"
#include "ubt_rssi.h"
#include "ubt_rx_sess.h"

LOG_MODULE_DECLARE(ubt_mode);

/* The demodulator's squelch: at least this far over the noise floor (0.5 dB units). */
#define SQUELCH_MARGIN 14U
#define SQUELCH_HOLD   16U
#define FLOOR_SAMPLES  120000U

/* Idle blocks sent in a row before further ones are dropped (the host sees the channel quiet). */
#define IDLE_SEND 4U

/* Dwell per channel of the sweep (128 CLKN ticks) and of the AFH mode (158). */
#define SWEEP_TICKS (128U * UBT_CLK100NS_PER_CLKN)
#define AFH_TICKS   (158U * UBT_CLK100NS_PER_CLKN)

struct br_run {
	struct ubt_dev *d;
	struct ubt_rx_sess *s;
	struct ubt_br br;
	struct ubt_rssi rssi;
	uint8_t floor;
	uint32_t hops;
};

/* Per block state: ordinary internal RAM. */
static struct br_run brun;

static void stamp(void *ctx, uint32_t t_end, uint8_t *clkn_high, uint32_t *clk100ns)
{
	struct br_run *r = ctx;

	ubt_rx_sess_split(r->s, t_end, clkn_high, clk100ns);
}

static void emit(void *ctx, const usb_pkt_rx *pkt)
{
	struct br_run *r = ctx;

	ubt_rx_sess_emit(r->d, pkt);
}

/* The demodulator's squelch: the host's level, but never below the noise floor's margin. */
static void set_squelch(struct br_run *r)
{
	uint8_t host = ubt_rssi_squelch_pwr(&r->rssi);
	uint8_t own = (uint8_t)MIN(255U, r->floor + SQUELCH_MARGIN);

	gfsk_demod_set_squelch(&r->s->dm, MAX(host, own), SQUELCH_HOLD, false);
}

static int retune(struct br_run *r, uint16_t mhz)
{
	int ret = ubt_rx_sess_tune(r->s, mhz);

	ubt_br_retune(&r->br, mhz);
	set_squelch(r);
	r->hops++;
	return ret;
}

void ubt_symbols_run(struct ubt_dev *d)
{
	struct br_run *r = &brun;
	uint8_t mode = d->mode;
	uint32_t cfg_seen = d->cfg_gen;
	uint64_t next_hop = 0;
	struct ubt_rx_sess *s;
	uint16_t mhz;
	uint8_t tracker;

	memset(r, 0, sizeof(*r));
	r->d = d;
	s = ubt_rx_sess_open(d, GFSK_BR);
	r->s = s;
	ubt_rssi_init(&r->rssi);
	ubt_rssi_set_squelch(&r->rssi, d->cs_threshold_req);
	ubt_br_init(&r->br, emit, stamp, r, &r->rssi);
	ubt_br_set_drop(&r->br, IDLE_SEND);

	/* SET_CHANNEL 9999 (the sweep) and the AFH mode's host leave the channel at 2402. */
	mhz = d->channel;
	if (mhz < 2402U || mhz > 2480U) {
		LOG_WRN("channel %u MHz is outside the Bluetooth band", mhz);
		d->requested_mode = MODE_IDLE;
		return;
	}
	if (ubt_rx_sess_tune(s, mhz) != 0) {
		d->requested_mode = MODE_IDLE;
		return;
	}
	r->floor = ubt_rx_sess_floor(s, FLOOR_SAMPLES, 400, &tracker);
	ubt_br_retune(&r->br, mhz);
	set_squelch(r);
	LOG_INF("BR symbols on %u MHz%s, noise floor pwr %u (tracker %u), squelch %d dBm (%u "
		"samples lost measuring)", mhz,
		d->hop_mode == HOP_SWEEP ? " (sweep)" : (d->hop_mode == HOP_AFH ? " (AFH)" : ""),
		r->floor, tracker, r->rssi.cs_cur, (unsigned int)s->gap_samples);
	ubt_rx_sess_stats_reset(s);

	while (d->requested_mode == mode) {
		struct ubt_rx_poll p;
		uint64_t now = ubt_rx_sess_wall(s);

		if (cfg_seen != d->cfg_gen) {
			cfg_seen = d->cfg_gen;
			ubt_rssi_set_squelch(&r->rssi, d->cs_threshold_req);
			ubt_rssi_squelch_update(&r->rssi, mhz);
			set_squelch(r);
			if (d->hop_mode == HOP_NONE && d->channel != mhz && d->channel >= 2402U &&
			    d->channel <= 2480U) {
				mhz = d->channel;
				if (retune(r, mhz) != 0) {
					break;
				}
				continue;
			}
		}
		if (d->hop_mode == HOP_SWEEP || d->hop_mode == HOP_AFH) {
			/* The timer's hop, or the host's (UBERTOOTH_HOP). */
			if (next_hop == 0U) {
				next_hop = now + (d->hop_mode == HOP_AFH ? AFH_TICKS : SWEEP_TICKS);
			} else if (now >= next_hop || d->do_hop) {
				d->do_hop = 0;
				next_hop = now + (d->hop_mode == HOP_AFH ? AFH_TICKS : SWEEP_TICKS);
				mhz = ubt_br_next_channel(d->hop_mode, d->afh_enabled, d->afh_map, mhz);
				d->channel = mhz;
				if (retune(r, mhz) != 0) {
					break;
				}
				continue;
			}
		} else {
			/* A fixed channel: upstream's hop() returns when the channel is the one set. */
			d->do_hop = 0;
			next_hop = 0;
		}
		ubt_rx_sess_poll(s, &p);
		if (p.samples == 0U && p.gap == 0U) {
			k_sleep(K_USEC(100));
			continue;
		}
		if (p.restarted) {
			ubt_br_retune(&r->br, mhz);
		} else if (p.gap != 0U) {
			ubt_br_gap(&r->br);
		}
		if (p.nsym != 0U) {
			ubt_br_push(&r->br, s->syms, p.nsym);
		}
		/* Turn the squelched time into idle blocks (not before the pre-roll history exists). */
		if (gfsk_demod_now(&s->dm) >= UBT_RX_CHUNK) {
			ubt_br_advance(&r->br, gfsk_demod_horizon(&s->dm), gfsk_demod_level(&s->dm));
		}
	}
	LOG_INF("BR symbols stopped: %u blocks (%u idle, %u not sent), %u hops, %u of %u samples "
		"lost (%u gaps, %u restarts), demodulator %u cycles a sample", r->br.blocks,
		r->br.idle_blocks, r->br.dropped_blocks, r->hops, (unsigned int)s->gap_samples,
		(unsigned int)(s->dm_samples + s->gap_samples), s->gaps, s->restarts,
		(unsigned int)(s->dm_cycles / MAX(s->dm_samples, 1U)));
	ubt_rx_sess_log_cpu(s);
	ubt_rx_sess_close(s);
}
