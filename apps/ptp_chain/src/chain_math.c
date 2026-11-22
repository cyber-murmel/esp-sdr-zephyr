/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See chain_math.h. chain_rx and the tx records run in the receive interrupt: chain_ram.ld puts
 * them in IRAM.
 */
#include <math.h>
#include <string.h>

#include "chain_math.h"

void chain_pair_init(struct chain_pair *p, bool digital)
{
	memset(p, 0, sizeof(*p));
	p->digital = digital;
}

static struct chain_txrec *tx_find(struct chain_pair *p, uint32_t seq)
{
	struct chain_txrec *r = &p->tx[seq & (CHAIN_TXN - 1U)];

	return (r->used && r->seq == seq) ? r : NULL;
}

void chain_tx_register(struct chain_pair *p, uint32_t seq, uint32_t reply_to, bool is_reply)
{
	struct chain_txrec *r = &p->tx[seq & (CHAIN_TXN - 1U)];

	r->used = true;
	r->seq = seq;
	r->reply_to = reply_to;
	r->is_reply = is_reply;
	r->stamp.valid = false;
}

void chain_tx_stamp(struct chain_pair *p, uint32_t seq, const struct chain_stamp *s)
{
	struct chain_txrec *r = tx_find(p, seq);

	if (r != NULL) {
		r->stamp = *s;
	}
}

bool chain_tx_get(const struct chain_pair *p, uint32_t seq, struct chain_stamp *s)
{
	const struct chain_txrec *r = &p->tx[seq & (CHAIN_TXN - 1U)];

	if (!r->used || r->seq != seq || !r->stamp.valid) {
		return false;
	}
	*s = r->stamp;
	return true;
}

static void emit(const struct chain_pair *p, struct chain_rx_res *res, uint32_t seq,
		 const struct chain_stamp *t1, const struct chain_stamp *t2,
		 const struct chain_stamp *t3, const struct chain_stamp *t4)
{
	struct chain_xchg *x = &res->x[res->n++];

	x->seq = seq;
	x->base_sec = t1->sec;
	x->t_ps[0] = chain_stamp_rel_ps(t1, t1->sec, p->digital);
	x->t_ps[1] = chain_stamp_rel_ps(t2, t1->sec, p->digital);
	x->t_ps[2] = chain_stamp_rel_ps(t3, t1->sec, p->digital);
	x->t_ps[3] = chain_stamp_rel_ps(t4, t1->sec, p->digital);
}

void chain_rx(struct chain_pair *p, const struct chain_rx_in *in, struct chain_rx_res *res)
{
	struct chain_rxrec *l = &p->last;
	struct chain_txrec *m;

	res->n = 0;
	res->stale = false;
	res->lost = 0;

	if (l->valid) {
		int32_t d = (int32_t)(in->seq - l->seq);

		if (d <= 0 && d >= -CHAIN_STALE_WIN) {
			res->stale = true;
			return;
		}
		if (d > 0 && (uint32_t)d < CHAIN_RESYNC) {
			res->lost = (uint32_t)d - 1U;
		}
	}

	/*
	 * Pair started by us: own m, peer's direct reply l (carries its rx of m), and now the frame
	 * after l carries the peer's tx stamp of l.
	 */
	if (l->valid && l->is_reply && l->rx.valid && l->crx.valid && in->tx.valid &&
	    in->tx_seq == l->seq && l->crx_seq == l->reply_to) {
		m = tx_find(p, l->reply_to);
		if (m != NULL && m->stamp.valid) {
			emit(p, res, m->seq, &m->stamp, &l->crx, &in->tx, &l->rx);
		}
	}

	/*
	 * Pair started by the peer: its frame l (rx here) and our direct reply m. Any later frame of
	 * the peer (a reply to m or the first frame of the next burst) is the carrier: it holds the
	 * peer's tx stamp of l (tx_seq) and its rx stamp of m (rx_seq).
	 */
	if (l->valid && l->rx.valid && in->tx.valid && in->rx.valid && in->tx_seq == l->seq) {
		m = tx_find(p, in->rx_seq);
		if (m != NULL && m->is_reply && m->reply_to == l->seq && m->stamp.valid) {
			emit(p, res, l->seq, &m->stamp, &in->rx, &in->tx, &l->rx);
		}
	}

	l->valid = true;
	l->seq = in->seq;
	l->is_reply = in->is_reply;
	l->reply_to = in->reply_to;
	l->rx = in->local_rx;
	l->crx_seq = in->rx_seq;
	l->crx = in->rx;
}

void chain_fit_reset(struct chain_fit *f)
{
	memset(f, 0, sizeof(*f));
}

void chain_fit_add(struct chain_fit *f, int64_t x_ps, int64_t y_ps)
{
	double dx_ps, dy_ps;

	if (f->n == 0) {
		f->x0_ps = (double)x_ps;
		f->y0_ps = (double)y_ps;
	}
	dx_ps = (double)x_ps - f->x0_ps;
	dy_ps = (double)y_ps - f->y0_ps;
	f->n++;
	f->sx_ps += dx_ps;
	f->sy_ps += dy_ps;
	f->sxx_ps2 += dx_ps * dx_ps;
	f->sxy_ps2 += dx_ps * dy_ps;
	f->syy_ps2 += dy_ps * dy_ps;
}

int chain_fit_end(const struct chain_fit *f, struct chain_fit_res *r)
{
	double n = (double)f->n;
	double cxx_ps2, cxy_ps2, cyy_ps2, slope, var;

	memset(r, 0, sizeof(*r));
	r->n = f->n;
	if (f->n == 0) {
		return -1;
	}
	r->x_mean_ps = f->x0_ps + f->sx_ps / n;
	r->y_mean_ps = f->y0_ps + f->sy_ps / n;
	cxx_ps2 = f->sxx_ps2 - f->sx_ps * f->sx_ps / n;
	cxy_ps2 = f->sxy_ps2 - f->sx_ps * f->sy_ps / n;
	cyy_ps2 = f->syy_ps2 - f->sy_ps * f->sy_ps / n;
	if (f->n < 2 || cxx_ps2 <= 0.0) {
		r->rms_ps = sqrt(cyy_ps2 > 0.0 ? cyy_ps2 / n : 0.0);
		return -1;
	}
	slope = cxy_ps2 / cxx_ps2;
	var = (cyy_ps2 - slope * cxy_ps2) / n;
	r->rate_ppb = slope * 1e9;
	r->rms_ps = sqrt(var > 0.0 ? var : 0.0);
	return 0;
}

void chain_servo_init(struct chain_servo *s, double period_s, double bw_hz, double clamp_ps)
{
	double wn = CHAIN_TAU * bw_hz;

	memset(s, 0, sizeof(*s));
	s->kp = 2.0 * wn / 1e3;
	s->ki = wn * wn / 1e3;
	s->period_s = period_s;
	s->clamp_ps = clamp_ps;
}

double chain_servo_update(struct chain_servo *s, double offset_ps, int64_t *step_ps)
{
	double integ, out;

	*step_ps = 0;
	if (fabs(offset_ps) > s->clamp_ps) {
		*step_ps = (int64_t)llround(offset_ps);
		s->steps++;
		return s->rate_ppb;
	}
	integ = s->integ_ppb + s->ki * offset_ps * s->period_s;
	out = s->kp * offset_ps + integ;
	/* the output limit also stops the integrator (no windup against it) */
	if (out > CHAIN_SERVO_MAX_PPB) {
		integ -= out - CHAIN_SERVO_MAX_PPB;
		out = CHAIN_SERVO_MAX_PPB;
	} else if (out < -CHAIN_SERVO_MAX_PPB) {
		integ -= out + CHAIN_SERVO_MAX_PPB;
		out = -CHAIN_SERVO_MAX_PPB;
	}
	s->integ_ppb = integ;
	s->rate_ppb = out;
	return out;
}
