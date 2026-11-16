/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The BLE receive modes on the 4 MS/s stream (ubt_rx_sess):
 *  - MODE_BT_FOLLOW_LE: the packets of one advertising channel (ubertooth-btle -n); with
 *    following on (-f) a CONNECT_IND sends the radio after the connection (le_conn), back to
 *    the advertising channel when it ends;
 *  - MODE_BT_PROMISC_LE (-p): the access address of a connection on a data channel, its
 *    CRCInit, interval and hop increment (ubt_promisc), then the connection is followed like
 *    one from a CONNECT_IND; when it ends the search starts over (upstream's bt_promisc_le()).
 * The demodulator, the packet finder and the planning run on the mode thread; a retune stops the
 * stream for 3 to 4 ms, so connection events the retune cannot reach are passed over (le_conn).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr_place.h>

#include "le_conn.h"
#include "ubt_le.h"
#include "ubt_mode.h"
#include "ubt_radio.h"
#include "ubt_rssi.h"
#include "ubt_rx_sess.h"

LOG_MODULE_DECLARE(ubt_mode);

/* The squelch opens this far above the noise floor (0.5 dB units) and closes 3 dB under that. */
#define SQUELCH_MARGIN 14U
#define SQUELCH_HOLD   16U

/* The noise floor is measured over this many samples before the squelch is set. */
#define FLOOR_SAMPLES 120000U

/* Promiscuous mode starts on this data channel when the one set is not a data channel. */
#define PROMISC_DEFAULT_MHZ 2440U

/* Packets of one poll handed to le_conn (a 64 us chunk holds two at most). */
#define CONN_PKTS 4U

/*
 * Margin on the measured retune time (tune request to the stream's first sample) that le_conn
 * plans with, and its floor: the time stamps of the stream are exact, the start is not.
 */
#define RETUNE_MARGIN_TICKS (500U * LE_TICKS_PER_US)
#define RETUNE_MIN_TICKS    (4500U * LE_TICKS_PER_US)

/* Longest sleep while waiting for a planned retune (stays responsive to requests). */
#define WAIT_SLICE_MS 20

struct conn_pkt {
	struct le_conn_pkt p;
	uint8_t pdu[UBT_LE_USB_BODY];
};

struct le_run {
	struct ubt_dev *d;
	struct ubt_rx_sess *s;
	uint8_t mode;
	bool follow;
	uint16_t adv_mhz;
	uint8_t squelch;
	struct le_conn conn;
	struct ubt_promisc pm;
	/* A retune le_conn planned and its time. */
	bool tune_pending;
	struct le_conn_action pending;
	uint8_t npk;
	struct conn_pkt pk[CONN_PKTS];
	uint32_t follows, ended[LE_REASON_PHY + 1];
	/* The clip guard (ubt_rx_gain UBT_RX_GAIN_GUARD) and the noise floor at each of its levels. */
	bool guard;
	struct esp_sdr_gain_guard gg;
	uint8_t floors[UBT_GUARD_LEVELS];
	uint32_t level_changes;
};

/* Per packet state only: ordinary internal RAM (place.h; the high region holds the session). */
static ESP_SDR_DATA_F(STATE, run, NOINIT) struct le_run run;

ESP_SDR_CODE(EVENT, clk_of_sample)
static uint32_t clk_of_sample(void *ctx, uint32_t sample_index)
{
	/* Not ctx: the finder passes le->ctx, which promiscuous mode takes over for its emit hook. */
	struct le_run *r = &run;
	uint8_t high;

	ARG_UNUSED(ctx);
	uint32_t clk;

	ubt_rx_sess_split(r->s, sample_index, &high, &clk);
	return clk;
}

ESP_SDR_CODE(EVENT, time_of_sample)
static uint64_t time_of_sample(void *ctx, uint32_t sample_index)
{
	struct le_run *r = ctx;

	return ubt_rx_sess_t100(r->s, sample_index);
}

/* Every packet of the finder: to the host, and to le_conn when following. */
ESP_SDR_CODE(EVENT, emit_pkt)
static void emit_pkt(void *ctx, const usb_pkt_rx *pkt)
{
	struct le_run *r = ctx;
	const struct ubt_le_info *info = &r->s->le.info;
	struct conn_pkt *c;

	ubt_rx_sess_emit(r->d, pkt);
	if (!r->follow || r->npk >= CONN_PKTS) {
		return;
	}
	c = &r->pk[r->npk++];
	c->p.mhz = r->s->le.mhz;
	c->p.t = ubt_rx_sess_t100(r->s, info->t_start);
	c->p.len = info->pdu_len < UBT_LE_USB_BODY ? info->pdu_len : (uint16_t)UBT_LE_USB_BODY;
	c->p.crc_ok = info->crc_ok;
	memcpy(c->pdu, &pkt->data[4], c->p.len);
	c->p.pdu = c->pdu;
}

static void apply_squelch(struct le_run *r)
{
	gfsk_demod_set_squelch(&r->s->dm, r->squelch, SQUELCH_HOLD, false);
}

/* The stream needs a tune for the guard's level (its gain differs from the running one's). */
static bool level_stale(const struct le_run *r)
{
	return r->guard && r->s->gain != ubt_guard_gain[r->gg.level];
}

/* Tune to @p mhz; under the guard at its level, with that level's squelch and RSSI calibration. */
static int tune(struct le_run *r, uint16_t mhz)
{
	if (r->guard) {
		unsigned int l = r->gg.level;

		r->s->gain = ubt_guard_gain[l];
		r->squelch = (uint8_t)MIN(255U, r->floors[l] + SQUELCH_MARGIN);
		apply_squelch(r);
		ubt_rssi_set_cal(UBT_RSSI_K_DEFAULT + ubt_guard_drop_x2[l]);
	}
	return ubt_rx_sess_tune(r->s, mhz);
}

/* Listen on @p mhz for @p aa and @p crc_init. */
static int listen(struct le_run *r, uint16_t mhz, uint32_t aa, uint32_t crc_init)
{
	struct ubt_le *le = &r->s->le;
	int ret = 0;

	if (mhz != r->s->mhz || !ubt_radio_rx_running() || level_stale(r)) {
		ret = tune(r, mhz);
	}
	(void)ubt_le_set_channel(le, mhz);
	ubt_le_set_aa(le, aa);
	ubt_le_set_crc_init(le, crc_init);
	/* le_conn plans with the slowest retune seen. */
	r->conn.cfg.retune_ticks = MAX(RETUNE_MIN_TICKS, r->s->tune_max + RETUNE_MARGIN_TICKS);
	return ret;
}

static int listen_adv(struct le_run *r)
{
	return listen(r, r->adv_mhz, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
}

static int promisc_restart(struct le_run *r)
{
	struct ubt_le *le = &r->s->le;
	uint16_t mhz = r->pm.scan_mhz;
	int ret = tune(r, mhz);

	ubt_promisc_start(&r->pm, mhz);
	ubt_le_set_crc_verify(le, false);
	return ret;
}

static const char *const reason_str[] = {
	"none", "cancelled", "timeout", "terminated", "too fast", "bad parameters", "PHY change",
};

/* Carry out le_conn's plan. Returns false when the radio cannot be had. */
static bool apply(struct le_run *r, const struct le_conn_action *a)
{
	switch (a->act) {
	case LE_ACT_TUNE:
		if (a->mhz == r->s->mhz && ubt_radio_rx_running() && !level_stale(r)) {
			/* The same channel: nothing to retune, the finder keeps its state. */
			ubt_le_set_aa(&r->s->le, a->aa);
			ubt_le_set_crc_init(&r->s->le, a->crc_init);
			return true;
		}
		r->pending = *a;
		r->tune_pending = true;
		return true;
	case LE_ACT_ADV:
		r->tune_pending = false;
		if (a->reason <= LE_REASON_PHY) {
			r->ended[a->reason]++;
		}
		LOG_INF("connection %08x no longer followed: %s (%u events, %u missed, %u skipped)",
			r->conn.aa, reason_str[a->reason <= LE_REASON_PHY ? a->reason : 0],
			r->conn.stats.followed, r->conn.stats.missed, r->conn.stats.skipped);
		if (r->mode == MODE_BT_PROMISC_LE) {
			r->follow = false;
			return promisc_restart(r) == 0;
		}
		return listen_adv(r) == 0;
	default:
		return true;
	}
}

/* Wait for the planned retune's time, then retune. False if the mode is to end meanwhile. */
static bool do_pending_tune(struct le_run *r)
{
	struct ubt_dev *d = r->d;
	const struct le_conn_action *a = &r->pending;

	for (;;) {
		uint64_t wall = ubt_rx_sess_wall(r->s);

		if (d->requested_mode != r->mode || d->cancel_follow) {
			return false;
		}
		if (wall >= a->t_retune) {
			break;
		}
		if (a->t_retune - wall > (uint64_t)WAIT_SLICE_MS * 10000U) {
			k_sleep(K_MSEC(WAIT_SLICE_MS));
			continue;
		}
		ubt_rx_sess_wait_until(r->s, a->t_retune);
	}
	r->tune_pending = false;
	return listen(r, a->mhz, a->aa, a->crc_init) == 0;
}

/* The packets of the last poll, then the time: le_conn's plan. */
static bool conn_step(struct le_run *r)
{
	uint64_t now;
	struct le_conn_action last = {0};

	/*
	 * Nothing for le_conn without a packet or a connection to tick. The time of the stream
	 * (64 bit divisions) cost some 800 cycles a poll, 3 a sample: on the advertising channel
	 * that margin decided whether the ring overflowed during the advertisement and the
	 * CONNECT_IND right behind it (2026-11-11: 3 of 24 CONNECT_INDs caught, 15 of 24 without).
	 */
	if (r->npk == 0U && !le_conn_following(&r->conn)) {
		return true;
	}
	now = ubt_rx_sess_now(r->s);

	for (uint8_t k = 0; k < r->npk; k++) {
		struct le_conn_action a = le_conn_packet(&r->conn, &r->pk[k].p, now);

		if (a.act != LE_ACT_NONE) {
			last = a;
		}
		if (a.act == LE_ACT_TUNE && r->conn.stats.connects != r->follows) {
			r->follows = r->conn.stats.connects;
			LOG_INF("following %08x, crc init %06x, interval %u us, hop %u%s", r->conn.aa,
				r->conn.crc_init, (unsigned int)(r->conn.interval / LE_TICKS_PER_US),
				r->conn.hop, r->conn.csa2 ? ", CSA #2" : "");
		}
	}
	r->npk = 0;
	if (last.act == LE_ACT_NONE && le_conn_following(&r->conn) && !ubt_le_busy(&r->s->le)) {
		last = le_conn_tick(&r->conn, now);
	}
	return apply(r, &last);
}

/* A configuration change from the host while sniffing on the advertising channel. */
static void le_config(struct le_run *r)
{
	struct ubt_dev *d = r->d;
	struct ubt_le *le = &r->s->le;
	uint16_t adv = d->le_adv_channel;

	ubt_le_set_crc_verify(le, d->le.crc_verify);
	ubt_le_set_target_raw(le, d->le.target.set, d->le.target.addr, d->le.target.mask);
	if (r->mode != MODE_BT_FOLLOW_LE) {
		return;
	}
	r->follow = d->le.follow;
	if (adv != r->adv_mhz && ubt_le_chan_idx(adv) >= 37U && ubt_le_chan_idx(adv) <= 39U) {
		/* SET_CHANNEL while sniffing: the advertising channel to listen on (and return to). */
		r->adv_mhz = adv;
		r->conn.cfg.adv_mhz = adv;
		if (!le_conn_following(&r->conn)) {
			(void)listen_adv(r);
		}
	}
}

ESP_SDR_CODE(CHUNK, le_run)
static void le_run(struct ubt_dev *d, uint8_t mode)
{
	struct le_run *r = &run;
	struct ubt_rx_sess *s;
	struct le_conn_cfg cfg;
	uint32_t cfg_seen = d->cfg_gen;
	uint16_t mhz = d->channel;
	uint8_t floor, tracker;

	memset(r, 0, sizeof(*r));
	r->d = d;
	r->mode = mode;
	s = ubt_rx_sess_open(d, GFSK_BLE);
	r->s = s;
	ubt_le_init(&s->le, emit_pkt, clk_of_sample, ubt_rssi_raw, r);
	le_conn_cfg_default(&cfg);
	if (mode == MODE_BT_FOLLOW_LE) {
		if (ubt_le_chan_idx(mhz) > 39U) {
			LOG_WRN("channel %u MHz is not a BLE channel", mhz);
			d->requested_mode = MODE_IDLE;
			return;
		}
		r->follow = d->le.follow;
	} else if (ubt_le_chan_idx(mhz) > 36U) {
		/* Upstream jumps to a data channel when an odd one is set; any non data channel here. */
		mhz = PROMISC_DEFAULT_MHZ;
	}
	r->adv_mhz = mode == MODE_BT_FOLLOW_LE && ubt_le_chan_idx(mhz) >= 37U ? mhz : 2402U;
	cfg.adv_mhz = r->adv_mhz;
	le_conn_init(&r->conn, &cfg);
	d->cancel_follow = false;

	/*
	 * The squelch follows the noise floor measured on this channel, for the whole mode. Under
	 * the guard, at each of its levels (the lowest first, so the stream ends at the first).
	 */
	r->guard = ubt_rx_gain == UBT_RX_GAIN_GUARD;
	esp_sdr_gain_guard_init(&r->gg, &ubt_guard_cfg, k_uptime_get());
	for (int l = r->guard ? (int)UBT_GUARD_LEVELS - 1 : 0; l >= 0; l--) {
		if (r->guard) {
			s->gain = ubt_guard_gain[l];
		}
		if (ubt_rx_sess_tune(s, mhz) != 0) {
			d->requested_mode = MODE_IDLE;
			return;
		}
		r->floors[l] = ubt_rx_sess_floor(s, l == 0 ? FLOOR_SAMPLES : FLOOR_SAMPLES / 2U, 400,
						 &tracker);
	}
	(void)ubt_le_set_channel(&s->le, mhz);
	le_config(r);
	floor = r->floors[0];
	r->squelch = (uint8_t)MIN(255U, floor + SQUELCH_MARGIN);
	apply_squelch(r);
	ubt_rssi_set_cal(UBT_RSSI_K_DEFAULT);

	if (mode == MODE_BT_PROMISC_LE) {
		ubt_promisc_init(&r->pm, &s->le, emit_pkt, r);
		ubt_promisc_set_time(&r->pm, time_of_sample, r);
		if (d->le.aa_set) {
			/* The host gave the access address (ubertooth-btle -p -a): CRCInit next. */
			d->le.aa_set = false;
			ubt_promisc_start_aa(&r->pm, mhz, d->le.access_address);
		} else {
			ubt_promisc_start(&r->pm, mhz);
		}
		ubt_le_set_crc_verify(&s->le, false);
	}
	LOG_INF("BLE %s on %u MHz, gain %d%s, noise floor pwr %u (tracker %u), squelch %u (%u "
		"samples lost measuring)",
		mode == MODE_BT_PROMISC_LE ? "promiscuous" : (r->follow ? "following" : "sniffing"),
		mhz, s->gain, r->guard ? " (clip guard)" : "", floor, tracker, r->squelch,
		(unsigned int)s->gap_samples);
	if (r->guard) {
		LOG_INF("clip guard floors: gain %u pwr %u, %u %u, %u %u, %u %u", ubt_guard_gain[0],
			r->floors[0], ubt_guard_gain[1], r->floors[1], ubt_guard_gain[2],
			r->floors[2], ubt_guard_gain[3], r->floors[3]);
	}
	ubt_rx_sess_stats_reset(s);

	while (d->requested_mode == mode) {
		struct ubt_rx_poll p;
		bool following = le_conn_following(&r->conn);

		if (cfg_seen != d->cfg_gen) {
			cfg_seen = d->cfg_gen;
			le_config(r);
		}
		if (d->cancel_follow) {
			d->cancel_follow = false;
			if (following) {
				struct le_conn_action a = le_conn_cancel(&r->conn);

				if (!apply(r, &a)) {
					break;
				}
			}
			continue;
		}
		if (r->tune_pending) {
			if (!do_pending_tune(r) && d->requested_mode != mode) {
				break;
			}
			continue;
		}
		ubt_rx_sess_poll(s, &p);
		if (p.samples == 0U) {
			k_sleep(K_USEC(100));
			if (!following) {
				continue;
			}
		}
		if (p.restarted) {
			ubt_le_reset(&s->le);
		}
		r->npk = 0;
		if (mode == MODE_BT_PROMISC_LE && r->pm.phase != UBT_PM_DONE) {
			unsigned int want;

			if (p.nsym != 0U) {
				(void)ubt_promisc_feed(&r->pm, s->syms, p.nsym);
			}
			want = ubt_promisc_take_retune(&r->pm);
			if (want == 0U) {
				want = ubt_promisc_tick(&r->pm, ubt_rx_sess_now(s));
				if (want != 0U) {
					LOG_INF("promiscuous: connection %08x lost, searching again",
						r->pm.aa);
					want = ubt_promisc_take_retune(&r->pm);
				}
			}
			if (want != 0U && (want != s->mhz || !ubt_radio_rx_running() || level_stale(r)) &&
			    tune(r, (uint16_t)want) != 0) {
				break;
			}
			if (r->pm.phase == UBT_PM_DONE) {
				struct le_conn_known k = le_conn_known_from_promisc(&r->pm);
				struct le_conn_action a;

				r->follow = true;
				a = le_conn_start_known(&r->conn, &k, ubt_rx_sess_now(s));
				if (!apply(r, &a)) {
					break;
				}
			}
			continue;
		}
		if (p.nsym != 0U) {
			(void)ubt_le_feed(&s->le, s->syms, p.nsym);
		}
		if (r->guard && esp_sdr_gain_guard_update(&r->gg, k_uptime_get(),
							  s->le.stats.clipped, s->le.stats.loud)) {
			r->level_changes++;
			LOG_INF("clip guard: gain %u", ubt_guard_gain[r->gg.level]);
			/*
			 * Following (and in promiscuous mode, which retunes by itself) the next tune
			 * takes the new gain; on the advertising channel the stream starts over at it
			 * now.
			 */
			if (mode != MODE_BT_PROMISC_LE && !le_conn_following(&r->conn) &&
			    !r->tune_pending && listen(r, s->mhz, s->le.aa, s->le.crc_init) != 0) {
				break;
			}
		}
		if ((r->follow || following) && !conn_step(r)) {
			break;
		}
	}
	LOG_INF("BLE receive stopped: %u hits, %u packets, %u bad CRC, %u of %u samples lost (%u "
		"gaps, %u restarts), demodulator %u cycles a sample, %u tunes (slowest %u us), "
		"%u connections followed (%u events, %u missed, %u skipped)",
		s->le.stats.hits, s->le.stats.packets, s->le.stats.crc_bad,
		(unsigned int)s->gap_samples, (unsigned int)(s->dm_samples + s->gap_samples),
		s->gaps, s->restarts, (unsigned int)(s->dm_cycles / MAX(s->dm_samples, 1U)), s->tunes,
		s->tune_max / LE_TICKS_PER_US,
		r->conn.stats.connects, r->conn.stats.followed, r->conn.stats.missed,
		r->conn.stats.skipped);
	if (r->guard) {
		LOG_INF("clip guard: %u packets at full scale, %u gain changes, gain %u at the end",
			s->le.stats.clipped, r->level_changes, ubt_guard_gain[r->gg.level]);
	}
	ubt_rx_sess_log_cpu(s);
	ubt_rssi_set_cal(UBT_RSSI_K_DEFAULT);
	ubt_rx_sess_close(s);
}

void ubt_btle_run(struct ubt_dev *d)
{
	le_run(d, MODE_BT_FOLLOW_LE);
}

void ubt_promisc_run(struct ubt_dev *d)
{
	le_run(d, MODE_BT_PROMISC_LE);
}
