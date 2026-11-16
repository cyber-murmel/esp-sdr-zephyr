/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_rx_sess.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_xtensa.h>

#include "ubt_attr.h"
#include "ubt_radio.h"
#include "ubt_rssi.h"
#include "ubt_rx_sess.h"
#include "ubt_usb.h"

LOG_MODULE_DECLARE(ubt_mode);

/* The session is touched every sample: internal RAM (the high region). */
static struct ubt_rx_sess sess ESP_SDR_DATA_F(STATE, sess, NOINIT);

int ubt_rx_gain = UBT_RX_GAIN;

struct ubt_rx_sess *ubt_rx_sess_open(struct ubt_dev *d, enum gfsk_modulation mod)
{
	struct ubt_rx_sess *s = &sess;

	/* The high region is not zeroed at boot; the demodulator and the finder set themselves up. */
	memset(s, 0, offsetof(struct ubt_rx_sess, bi));
	memset(&s->mhz, 0, sizeof(*s) - offsetof(struct ubt_rx_sess, mhz));
	s->d = d;
	s->gain = ubt_rx_gain == UBT_RX_GAIN_GUARD ? UBT_RX_GAIN : ubt_rx_gain;
	gfsk_demod_init(&s->dm, mod);
	return s;
}

uint64_t ubt_rx_sess_wall(const struct ubt_rx_sess *s)
{
	return ubt_clock_t100(&s->d->clock, s->d->plat->cycles());
}

int ubt_rx_sess_tune(struct ubt_rx_sess *s, uint16_t mhz)
{
	int ret;

	s->t_tune = ubt_rx_sess_wall(s);
	ret = ubt_radio_rx_start(mhz, s->gain);
	s->mhz = mhz;
	s->next = 0;
	s->n64 = 0;
	s->tunes++;
	gfsk_demod_reset(&s->dm);
	if (ret != 0) {
		LOG_ERR("receive stream did not start on %u MHz (%d)", mhz, ret);
		return ret;
	}
	s->t_first = ubt_clock_t100(&s->d->clock, ubt_radio_rx_cycles(0));
	if (s->t_first > s->t_tune && s->t_first - s->t_tune > s->tune_max) {
		s->tune_max = (uint32_t)(s->t_first - s->t_tune);
	}
	return 0;
}

void ubt_rx_sess_close(struct ubt_rx_sess *s)
{
	ARG_UNUSED(s);
	ubt_radio_rx_stop();
}

UBT_HOT(CHUNK, ubt_rx_sess_poll)
void ubt_rx_sess_poll(struct ubt_rx_sess *s, struct ubt_rx_poll *p)
{
	uint32_t first;
	uint32_t gap;
	/*
	 * The statistics' clock: this CPU's cycle counter (the mode thread is pinned to CPU 0);
	 * plat->cycles() reads the system timer under a spinlock, some 1.7 % of CPU 0 at 4 a poll.
	 */
	uint32_t c0 = esp_sdr_ccount();
	size_t off = 0;
	size_t n = ubt_radio_rx_read8(s->bi, s->bq, UBT_RX_CHUNK, GFSK_IN_HIST, &first, &gap,
				      &off);
	uint32_t dc;
	bool was_open;

	s->rd_cycles += esp_sdr_ccount() - c0;
	memset(p, 0, sizeof(*p));
	if (n == 0U) {
		if (gap != 0U) {
			/* A damaged chunk was dropped: the samples are gone, the index moves on. */
			s->gaps_open += s->dm.open != 0U ? 1U : 0U;
			gfsk_demod_gap(&s->dm, gap);
			s->next += gap;
			s->n64 += gap;
			s->gaps++;
			s->gap_samples += gap;
			p->gap = gap;
		}
		return;
	}
	if (first != s->next + gap) {
		/* A new run of the ring (it restarts after an error): the stream starts over. */
		s->restarts++;
		gfsk_demod_reset(&s->dm);
		s->next = 0;
		s->n64 = 0;
		gap = first;
		p->restarted = true;
	}
	if (gap != 0U) {
		s->gaps_open += s->dm.open != 0U ? 1U : 0U;
		gfsk_demod_gap(&s->dm, gap);
		s->n64 += gap;
		s->gaps++;
		s->gap_samples += gap;
		p->gap = gap;
	}
	p->samples = n;
	was_open = s->dm.open != 0U;
	c0 = esp_sdr_ccount();
	p->nsym = gfsk_demod_push(&s->dm, s->bi + off, s->bq + off, n, s->syms, ARRAY_SIZE(s->syms));
	dc = esp_sdr_ccount() - c0;
	s->dm_cycles += dc;
	s->dm_samples += n;
	if (was_open || s->dm.open != 0U || p->nsym != 0U) {
		s->dm_open_cycles += dc;
		s->dm_open_samples += n;
	}
	s->next = first + (uint32_t)n;
	s->n64 += n;
}

uint8_t ubt_rx_sess_floor(struct ubt_rx_sess *s, uint32_t samples, int64_t timeout_ms,
			  uint8_t *tracker)
{
	struct ubt_floor_hist h;
	uint64_t done = 0;
	int64_t end = k_uptime_get() + timeout_ms;
	uint8_t floor;

	ubt_floor_hist_init(&h);
	while (done < samples && k_uptime_get() < end) {
		struct ubt_rx_poll p;

		ubt_rx_sess_poll(s, &p);
		if (p.samples == 0U) {
			k_sleep(K_USEC(100));
			continue;
		}
		done += p.samples;
		for (size_t k = 0; k < p.nsym; k++) {
			ubt_floor_hist_add(&h, s->syms[k].pwr);
		}
	}
	/* The low percentile, not the tracker (it follows the mean, Wi-Fi bursts and all). */
	*tracker = gfsk_demod_noise_floor(&s->dm);
	floor = ubt_floor_hist_get(&h);
	return floor != 0U ? floor : *tracker;
}

uint64_t ubt_rx_sess_t100(const struct ubt_rx_sess *s, uint32_t index)
{
	/* The index lies at or before the newest sample: count back from the 64 bit position. */
	uint32_t back = (uint32_t)s->n64 - index;
	uint64_t i64 = back <= s->n64 ? s->n64 - back : 0U;

	return ubt_clock_t100(&s->d->clock, ubt_radio_rx_cycles(i64));
}

uint64_t ubt_rx_sess_now(const struct ubt_rx_sess *s)
{
	if (s->n64 == 0U) {
		return ubt_rx_sess_wall(s);
	}
	return ubt_rx_sess_t100(s, (uint32_t)s->n64);
}

void ubt_rx_sess_wait_until(const struct ubt_rx_sess *s, uint64_t t100)
{
	uint64_t cyc = ubt_clock_cyc_at(&s->d->clock, t100);

	for (;;) {
		uint64_t now = s->d->plat->cycles();
		uint64_t us;

		if (now >= cyc) {
			return;
		}
		us = (cyc - now) / (ESP_SDR_CPU_HZ / 1000000U);
		if (us > 1500U) {
			/* Sleep most of it (tick granularity), spin the rest. */
			k_sleep(K_USEC((int32_t)(us - 1000U)));
		} else {
			k_busy_wait((uint32_t)us);
		}
	}
}

void ubt_rx_sess_split(const struct ubt_rx_sess *s, uint32_t index, uint8_t *clkn_high,
		       uint32_t *clk100ns)
{
	ubt_clock_split(ubt_rx_sess_t100(s, index), clkn_high, clk100ns);
}

void ubt_rx_sess_stats_reset(struct ubt_rx_sess *s)
{
#ifdef GFSK_PROF
	memset(&gfsk_prof, 0, sizeof(gfsk_prof));
#endif
	s->gap_samples = 0;
	s->dm_cycles = 0;
	s->dm_samples = 0;
	s->dm_open_cycles = 0;
	s->dm_open_samples = 0;
	s->rd_cycles = 0;
	s->gaps_open = 0;
}

void ubt_rx_sess_log_cpu(const struct ubt_rx_sess *s)
{
	uint64_t closed_n = s->dm_samples - s->dm_open_samples;

	LOG_INF("CPU 0 a sample: reader %u cycles, demodulator %u closed and %u open (%u %% of the "
		"samples open; %u of the gaps found open)",
		(unsigned int)(s->rd_cycles / MAX(s->dm_samples, 1U)),
		(unsigned int)((s->dm_cycles - s->dm_open_cycles) / MAX(closed_n, 1U)),
		(unsigned int)(s->dm_open_cycles / MAX(s->dm_open_samples, 1U)),
		(unsigned int)(100U * s->dm_open_samples / MAX(s->dm_samples, 1U)), s->gaps_open);
#ifdef GFSK_PROF
	/* The full path's runs (each opening of the squelch to its closing) and their samples. */
	LOG_INF("full path: %u runs, %u samples a run; sample loop %u, groups %u, slicer %u cycles a "
		"sample of it", gfsk_prof.opens, gfsk_prof.open / MAX(gfsk_prof.opens, 1U),
		gfsk_prof.hot / MAX(gfsk_prof.open, 1U),
		(gfsk_prof.groups - gfsk_prof.slice) / MAX(gfsk_prof.open, 1U),
		gfsk_prof.slice / MAX(gfsk_prof.open, 1U));
#endif
}

void ubt_rx_sess_emit(struct ubt_dev *d, const usb_pkt_rx *pkt)
{
	usb_pkt_rx *slot = ubt_queue_alloc(&d->queue);

	if (slot == NULL) {
		return;
	}
	*slot = *pkt;
	ubt_queue_commit(&d->queue);
	ubt_usb_kick();
}
