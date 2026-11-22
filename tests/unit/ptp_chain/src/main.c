/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * apps/ptp_chain/src/chain_math.c: stamps to ps, exchange pairing (lost frames, non-reply
 * frames, sequence wrap, second rollover), offset and delay signs, and the line fit. A simulated
 * pair of nodes X and Y (clock of Y = clock of X + offset, one-way delays d_xy and d_yx) runs the
 * chain; the test feeds the real stamps through chain_rx().
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "chain_math.h"

#define PS_PER_NS 1000LL
#define SEC       CHAIN_PS_PER_SEC

/* A two node chain on a common true time axis (ps). Clock of Y reads true + off_ps. */
struct sim {
	bool digital;
	int64_t off_ps;       /* Y minus X */
	int64_t d_xy_ps, d_yx_ps;
	int64_t t_ps;         /* true time */
	int64_t base_ps;      /* clock epoch added (so second boundaries are reachable) */
	struct chain_pair x, y;
	uint32_t seq_x, seq_y;
	/* what the last frame of the other side carried */
	struct chain_stamp x_last_tx, y_last_tx;
	bool x_have_tx, y_have_tx;
	uint32_t x_last_seq, y_last_seq;
	bool x_have_last, y_have_last;
	struct chain_stamp x_last_rx, y_last_rx;
};

static struct chain_stamp stamp_of(const struct sim *s, int64_t clk_ps)
{
	struct chain_stamp st;
	int64_t t = clk_ps + s->base_ps;
	int64_t sec = t / SEC;
	int64_t frac = t % SEC;

	st.sec = (uint32_t)sec;
	st.valid = true;
	if (s->digital) {
		st.subsec = (uint32_t)(frac / PS_PER_NS);
	} else {
		/* units of 2^-31 s, truncated: frac * 2^31 / 1e12 = frac * 2^19 / 5^12 */
		st.subsec = (uint32_t)(((uint64_t)frac << 19) / CHAIN_BIN_MUL);
	}
	return st;
}

struct frame {
	uint32_t seq, reply_to;
	bool is_reply;
	uint32_t rx_seq, tx_seq;
	struct chain_stamp rx, tx;
	struct chain_stamp tx_stamp;  /* this frame's own tx stamp (stays at the sender) */
};

/*
 * Node X (from_x) sends one frame at the current true time; the other side receives it and
 * returns the receiver's result. clk offset: X clock = true, Y clock = true + off_ps.
 */
static void send_frame(struct sim *s, bool from_x, bool is_reply, uint32_t reply_to,
		       struct chain_rx_res *res, bool drop)
{
	struct chain_pair *me = from_x ? &s->x : &s->y;
	struct chain_pair *peer = from_x ? &s->y : &s->x;
	int64_t my_off_ps = from_x ? 0 : s->off_ps;
	int64_t peer_off_ps = from_x ? s->off_ps : 0;
	int64_t delay_ps = from_x ? s->d_xy_ps : s->d_yx_ps;
	uint32_t *seq = from_x ? &s->seq_x : &s->seq_y;
	bool *have_tx = from_x ? &s->x_have_tx : &s->y_have_tx;
	struct chain_stamp *last_tx = from_x ? &s->x_last_tx : &s->y_last_tx;
	uint32_t *last_tx_seq = from_x ? &s->x_last_seq : &s->y_last_seq;
	bool *have_rx = from_x ? &s->x_have_last : &s->y_have_last;
	struct chain_stamp *last_rx = from_x ? &s->x_last_rx : &s->y_last_rx;
	struct chain_rx_in in;
	struct chain_stamp txs = stamp_of(s, s->t_ps + my_off_ps);
	uint32_t my_seq = (*seq)++;
	/* receiver's last-received record (its chain.last) */
	uint32_t peer_last_seq = peer->last.seq;

	chain_tx_register(me, my_seq, reply_to, is_reply);
	chain_tx_stamp(me, my_seq, &txs);

	memset(&in, 0, sizeof(in));
	in.seq = my_seq;
	in.is_reply = is_reply;
	in.reply_to = reply_to;
	/* carried: my rx of the last frame I received, my tx of my previous frame */
	if (*have_rx) {
		in.rx_seq = me->last.seq;
		in.rx = *last_rx;
	}
	if (*have_tx) {
		in.tx_seq = *last_tx_seq;
		in.tx = *last_tx;
	}
	(void)peer_last_seq;

	*last_tx = txs;
	*last_tx_seq = my_seq;
	*have_tx = true;

	s->t_ps += delay_ps;
	if (drop) {
		res->n = 0;
		return;
	}
	in.local_rx = stamp_of(s, s->t_ps + peer_off_ps);
	chain_rx(peer, &in, res);
	/* the receiver's side now carries this rx stamp */
	if (from_x) {
		s->y_last_rx = in.local_rx;
		s->y_have_last = true;
	} else {
		s->x_last_rx = in.local_rx;
		s->x_have_last = true;
	}
}

static void sim_init(struct sim *s, bool digital, int64_t off_ps, int64_t d_xy_ps, int64_t d_yx_ps,
		     int64_t base_ps)
{
	memset(s, 0, sizeof(*s));
	s->digital = digital;
	s->off_ps = off_ps;
	s->d_xy_ps = d_xy_ps;
	s->d_yx_ps = d_yx_ps;
	s->base_ps = base_ps;
	s->t_ps = 5 * SEC;
	chain_pair_init(&s->x, digital);
	chain_pair_init(&s->y, digital);
}

/* One chain frame after the other, X first; each a direct reply to the previous. */
static void run_chain(struct sim *s, int frames, struct chain_rx_res *rx_x,
		      struct chain_rx_res *rx_y, int *nx, int *ny, struct chain_xchg *keep_x,
		      struct chain_xchg *keep_y)
{
	uint32_t prev_seq = 0;
	bool reply = false;

	*nx = *ny = 0;
	for (int i = 0; i < frames; i++) {
		bool from_x = (i % 2) == 0;
		struct chain_rx_res r;

		send_frame(s, from_x, reply, prev_seq, &r, false);
		prev_seq = (from_x ? s->seq_x : s->seq_y) - 1U;
		reply = true;
		s->t_ps += 3 * PS_PER_NS;  /* turnaround */
		/* r: what the receiver (Y if from_x) completed */
		for (int k = 0; k < r.n; k++) {
			if (from_x) {
				keep_y[(*ny)++] = r.x[k];
			} else {
				keep_x[(*nx)++] = r.x[k];
			}
		}
		if (from_x) {
			*rx_y = r;
		} else {
			*rx_x = r;
		}
	}
}

ZTEST(ptp_chain, test_stamp_binary)
{
	/* 2^-31 s units: 2^30 is half a second, 0 is 0, the exact ps of 1 unit is 465.66 ps */
	zassert_equal(chain_subsec_to_ps(0, false), 0);
	zassert_equal(chain_subsec_to_ps(1U << 30, false), SEC / 2);
	zassert_equal(chain_subsec_to_ps(1, false), 465);  /* floor of 465.66 */
	/* the top bit is not a time bit */
	zassert_equal(chain_subsec_to_ps(0x80000000U | (1U << 30), false), SEC / 2);
	/* one unit before the second */
	zassert_equal(chain_subsec_to_ps(0x7fffffffU, false), 999999999534LL);
}

ZTEST(ptp_chain, test_stamp_digital)
{
	zassert_equal(chain_subsec_to_ps(0, true), 0);
	zassert_equal(chain_subsec_to_ps(1, true), 1000);
	zassert_equal(chain_subsec_to_ps(999999999U, true), 999999999000LL);
}

ZTEST(ptp_chain, test_stamp_rel_second_boundary)
{
	struct chain_stamp a = {.sec = 10, .subsec = 999999000U, .valid = true};
	struct chain_stamp b = {.sec = 11, .subsec = 1000U, .valid = true};
	struct chain_stamp c = {.sec = 9, .subsec = 500000000U, .valid = true};

	zassert_equal(chain_stamp_rel_ps(&a, 10, true), 999999000000LL);
	/* b is 2 us after a, across the second */
	zassert_equal(chain_stamp_rel_ps(&b, 10, true) - chain_stamp_rel_ps(&a, 10, true),
		      2000000LL);
	zassert_equal(chain_stamp_rel_ps(&c, 10, true), -SEC / 2);
	/* the same across a wrap of the u32 seconds */
	a.sec = 0xffffffffU;
	b.sec = 0;
	zassert_equal(chain_stamp_rel_ps(&b, a.sec, true) - chain_stamp_rel_ps(&a, a.sec, true),
		      2000000LL);
	/* saturation of a huge difference */
	b.sec = 0x7fffffffU;
	zassert_equal(chain_stamp_rel_ps(&b, 0, true),
		      (int64_t)CHAIN_SEC_LIM * SEC + 1000000LL);
}

ZTEST(ptp_chain, test_offset_delay_formula)
{
	/* X sends at 0 (X clock), Y clock = X + 700 ns, delays 100 ns out and 140 ns back,
	 * 50 ns turnaround at Y
	 */
	struct chain_xchg x = {.t_ps = {0, (100 + 700) * 1000LL, (850 + 700) * 1000LL,
					(850 + 140) * 1000LL}};

	/* offset is exact when the delays are equal; here the asymmetry (140-100)/2 = 20 ns
	 * shows up with the sign of the larger back delay: Y minus X = 700 - 20 ns
	 */
	zassert_equal(chain_xchg_offset_ps(&x), 680 * 1000LL);
	zassert_equal(chain_xchg_delay_ps(&x), 120 * 1000LL);
}

static void check_chain(bool digital, int64_t base_ps)
{
	static struct sim s;
	struct chain_rx_res rx, ry;
	struct chain_xchg kx[8], ky[8];
	int nx, ny;
	/* symmetric delay: offset exact */
	int64_t off_ps = 123456789LL;  /* 123.46 ns, not a multiple of the stamp grid on purpose */
	int64_t tol_ps = digital ? 1000 : 500;  /* ps: ns truncation (error up to 1 ns on each stamp,
					      * partly cancelling) or one 2^-31 s unit */

	sim_init(&s, digital, off_ps, 200 * PS_PER_NS, 200 * PS_PER_NS, base_ps);
	run_chain(&s, 8, &rx, &ry, &nx, &ny, kx, ky);

	/* X sees exchanges, with the peer minus local convention */
	zassert_true(nx >= 3, "X exchanges %d", nx);
	zassert_true(ny >= 3, "Y exchanges %d", ny);
	for (int i = 0; i < nx; i++) {
		int64_t o = chain_xchg_offset_ps(&kx[i]);
		int64_t d = chain_xchg_delay_ps(&kx[i]);

		zassert_true(llabs(o - off_ps) <= 2 * tol_ps, "X offset %lld want %lld", (long long)o,
			     (long long)off_ps);
		zassert_true(llabs(d - 200 * PS_PER_NS) <= 2 * tol_ps, "X delay %lld", (long long)d);
	}
	/* Y sees the opposite sign (X minus Y = -off_ps) */
	for (int i = 0; i < ny; i++) {
		int64_t o = chain_xchg_offset_ps(&ky[i]);

		zassert_true(llabs(o + off_ps) <= 2 * tol_ps, "Y offset %lld want %lld", (long long)o,
			     (long long)-off_ps);
	}
}

ZTEST(ptp_chain, test_chain_binary)
{
	check_chain(false, 0);
}

ZTEST(ptp_chain, test_chain_digital)
{
	check_chain(true, 0);
}

ZTEST(ptp_chain, test_chain_second_rollover)
{
	/* The chain straddles a second: t = 5 s + ... so put the epoch so that the first frames
	 * are 3 us before the boundary: every t1 and t2 may land on different sides.
	 */
	/* the sim starts at t = 5 s: the clock reads k s - 3 us at the first frame */
	check_chain(true, 1 * SEC - 5 * SEC - 3 * 1000000LL + 5 * SEC);
	check_chain(false, 1 * SEC - 3 * 1000000LL);
	check_chain(true, 1000000LL * SEC - 5 * SEC - 3 * 1000000LL);
	check_chain(false, 1000000LL * SEC - 5 * SEC - 3 * 1000000LL);
	/* and a Y clock in the next second, X one before: the t1 second differs from t2's */
	{
		static struct sim s;
		struct chain_rx_res rx, ry;
		struct chain_xchg kx[8], ky[8];
		int nx, ny;

		sim_init(&s, true, SEC - 100 * PS_PER_NS, 50 * PS_PER_NS, 50 * PS_PER_NS,
			 SEC - 5 * SEC - 1000LL);
		run_chain(&s, 6, &rx, &ry, &nx, &ny, kx, ky);
		zassert_true(nx >= 2);
		for (int i = 0; i < nx; i++) {
			zassert_true(llabs(chain_xchg_offset_ps(&kx[i]) -
					   (SEC - 100 * PS_PER_NS)) <= 1000);
		}
	}
}

/* Bursts of one round (two frames), the default: X starts each, Y answers, then a gap. */
static void check_bursts(bool digital)
{
	static struct sim s;
	struct chain_rx_res r;
	int64_t off_ps = 777777LL;
	int nx = 0, ny = 0;

	sim_init(&s, digital, off_ps, 150 * PS_PER_NS, 150 * PS_PER_NS, 0);
	for (int b = 0; b < 8; b++) {
		/* first frame: not a reply; the answer: a direct reply to it */
		send_frame(&s, true, false, 0, &r, false);
		for (int k = 0; k < r.n; k++) {
			int64_t o_ps = chain_xchg_offset_ps(&r.x[k]);

			zassert_true(llabs(o_ps - (-off_ps)) <= 2000, "Y %lld", (long long)o_ps);
			ny++;
		}
		s.t_ps += 2 * PS_PER_NS;
		send_frame(&s, false, true, s.seq_x - 1U, &r, false);
		for (int k = 0; k < r.n; k++) {
			int64_t o_ps = chain_xchg_offset_ps(&r.x[k]);

			zassert_true(llabs(o_ps - off_ps) <= 2000, "X %lld", (long long)o_ps);
			nx++;
		}
		s.t_ps += 7812 * 1000000LL;
	}
	/* one exchange per burst after the first two (the stamps of a burst come with the next) */
	zassert_true(nx >= 6, "X exchanges %d", nx);
	zassert_true(ny >= 6, "Y exchanges %d", ny);
}

ZTEST(ptp_chain, test_one_round_bursts_binary)
{
	check_bursts(false);
}

ZTEST(ptp_chain, test_one_round_bursts_digital)
{
	check_bursts(true);
}

ZTEST(ptp_chain, test_asymmetric_delay_error)
{
	static struct sim s;
	struct chain_rx_res rx, ry;
	struct chain_xchg kx[8], ky[8];
	int nx, ny;

	sim_init(&s, true, 1000 * PS_PER_NS, 100 * PS_PER_NS, 160 * PS_PER_NS, 0);
	run_chain(&s, 8, &rx, &ry, &nx, &ny, kx, ky);
	zassert_true(nx >= 3);
	/* X's first pair: out 100 ns, back 160 ns: Y minus X reads off_ps + (out - back)/2 = off_ps - 30 ns */
	for (int i = 0; i < nx; i++) {
		int64_t o = chain_xchg_offset_ps(&kx[i]);
		int64_t d = chain_xchg_delay_ps(&kx[i]);

		/* the pair started by X has out=d_xy, back=d_yx; a pair started by Y is stored
		 * in the same form with t1 = X tx s (d_xy leg) and t4 - t3 the Y -> X leg of p
		 */
		zassert_true(llabs(o - (1000 - 30) * PS_PER_NS) <= 2000, "o %lld", (long long)o);
		zassert_true(llabs(d - 130 * PS_PER_NS) <= 2000, "d %lld", (long long)d);
	}
}

ZTEST(ptp_chain, test_seq_wrap)
{
	zassert_true(chain_seq_lt(1, 2));
	zassert_false(chain_seq_lt(2, 1));
	zassert_false(chain_seq_lt(5, 5));
	zassert_true(chain_seq_lt(0xffffffffU, 0));
	zassert_true(chain_seq_lt(0xfffffff0U, 5));
	zassert_false(chain_seq_lt(5, 0xfffffff0U));
	zassert_true(chain_seq_lt(0x7fffffffU, 0x80000000U));

	/* a chain whose sequence numbers wrap produces exchanges all the same */
	static struct sim s;
	struct chain_rx_res rx, ry;
	struct chain_xchg kx[8], ky[8];
	int nx, ny;

	sim_init(&s, true, 5000, 100 * PS_PER_NS, 100 * PS_PER_NS, 0);
	s.seq_x = 0xfffffffeU;
	s.seq_y = 0xffffffffU;
	run_chain(&s, 8, &rx, &ry, &nx, &ny, kx, ky);
	zassert_true(nx >= 3 && ny >= 3, "nx %d ny %d", nx, ny);
	zassert_equal(rx.lost, 0);
	zassert_true(llabs(chain_xchg_offset_ps(&kx[nx - 1]) - 5000) <= 1000);
}

ZTEST(ptp_chain, test_lost_frame_no_exchange)
{
	static struct sim s;
	struct chain_rx_res r;
	struct chain_xchg kx[8], ky[8];
	struct chain_rx_res rx, ry;
	int nx, ny;
	uint32_t prev;
	int got = 0;

	sim_init(&s, true, 4000, 100 * PS_PER_NS, 100 * PS_PER_NS, 0);
	run_chain(&s, 6, &rx, &ry, &nx, &ny, kx, ky);
	zassert_true(nx + ny >= 4);

	/* the next frame (from X, seq_x) is lost on the way: Y never sees it */
	send_frame(&s, true, true, s.seq_y - 1U, &r, true);
	prev = s.seq_x - 1U;
	zassert_equal(r.n, 0);
	/* Y's chain is dead; the timer restarts it with a new, non-reply frame of X */
	s.t_ps += 7812 * 1000000LL;
	send_frame(&s, true, false, 0, &r, false);
	/* it skipped one X frame: Y counts it lost, and no exchange completes */
	zassert_equal(r.lost, 1U);
	zassert_equal(r.n, 0);
	(void)prev;

	/* Y answers it directly, X completes nothing (Y's carried tx stamp is of an older frame
	 * than X's record needs) or an exchange that is right; every one must be correct
	 */
	prev = s.seq_x - 1U;
	for (int i = 0; i < 6; i++) {
		bool from_x = (i % 2) == 1;

		send_frame(&s, from_x, true, from_x ? s.seq_y - 1U : s.seq_x - 1U, &r, false);
		s.t_ps += 3 * PS_PER_NS;
		for (int k = 0; k < r.n; k++) {
			zassert_true(llabs(chain_xchg_offset_ps(&r.x[k]) -
					   (from_x ? -4000 : 4000)) <= 2000,
				     "wrong exchange %lld", (long long)chain_xchg_offset_ps(&r.x[k]));
			got++;
		}
	}
	zassert_true(got >= 2, "chain recovers, got %d", got);
}

ZTEST(ptp_chain, test_non_reply_frame_no_pair)
{
	static struct sim s;
	struct chain_rx_res r;

	sim_init(&s, true, 4000, 100 * PS_PER_NS, 100 * PS_PER_NS, 0);
	/* X, Y, X, Y all sent as plain (non-reply) frames: nothing pairs */
	for (int i = 0; i < 8; i++) {
		send_frame(&s, (i % 2) == 0, false, 0, &r, false);
		s.t_ps += 1000;
		zassert_equal(r.n, 0, "frame %d produced %d exchanges", i, r.n);
	}
	/* a reply that names a frame we do not know (reply_to garbage) pairs with nothing */
	send_frame(&s, true, true, 0x1234, &r, false);
	zassert_equal(r.n, 0);
	send_frame(&s, false, true, 0x4321, &r, false);
	zassert_equal(r.n, 0);
}

ZTEST(ptp_chain, test_stale_and_restart)
{
	struct chain_pair p;
	struct chain_rx_in in;
	struct chain_rx_res r;

	chain_pair_init(&p, true);
	memset(&in, 0, sizeof(in));
	in.seq = 100;
	chain_rx(&p, &in, &r);
	zassert_false(r.stale);
	in.seq = 100;
	chain_rx(&p, &in, &r);
	zassert_true(r.stale, "repeat");
	in.seq = 99;
	chain_rx(&p, &in, &r);
	zassert_true(r.stale, "reorder");
	in.seq = 104;
	chain_rx(&p, &in, &r);
	zassert_false(r.stale);
	zassert_equal(r.lost, 3U);
	/* the peer rebooted: its sequence starts again at 2, far below: a resync, no stale */
	in.seq = 2000000000U;
	chain_rx(&p, &in, &r);
	zassert_false(r.stale);
	zassert_equal(r.lost, 0U);
	in.seq = 3;
	chain_rx(&p, &in, &r);
	zassert_false(r.stale);
	zassert_equal(r.lost, 0U);
}

ZTEST(ptp_chain, test_fit_known_line)
{
	struct chain_fit f;
	struct chain_fit_res r;
	/* offset(t) = 500 ns + 12 ppb * t (ps over ps), t over one second, with a +-3 ps alternating
	 * residual of known rms 3 ps
	 */
	int64_t x0 = 7 * SEC;

	chain_fit_reset(&f);
	for (int i = 0; i < 1000; i++) {
		int64_t x = x0 + i * 1000000000LL;  /* every ms */
		int64_t y = 500000LL + 12LL * i + ((i & 1) ? 3 : -3);  /* 12 ps per 1e9 ps */

		chain_fit_add(&f, x, y);
	}
	zassert_equal(chain_fit_end(&f, &r), 0);
	zassert_equal(r.n, 1000U);
	zassert_within(r.rate_ppb, 12.0, 0.01, "rate %f", r.rate_ppb);
	zassert_within(r.rms_ps, 3.0, 0.01, "rms %f", r.rms_ps);
	/* the line at the window centre */
	zassert_within(r.x_mean_ps, (double)x0 + 499.5e9, 1.0);
	zassert_within(r.y_mean_ps, 500000.0 + 12.0e-9 * 499.5e9, 0.5, "y %f", r.y_mean_ps);

	/* negative rate, no noise */
	chain_fit_reset(&f);
	for (int i = 0; i < 100; i++) {
		chain_fit_add(&f, i * 10000000000LL, 1000000LL - i * 10000000000LL / 1000000);
	}
	zassert_equal(chain_fit_end(&f, &r), 0);
	zassert_within(r.rate_ppb, -1000.0, 0.001);
	zassert_within(r.rms_ps, 0.0, 0.01);

	/* too few points */
	chain_fit_reset(&f);
	chain_fit_add(&f, 5, 7);
	zassert_equal(chain_fit_end(&f, &r), -1);
	zassert_within(r.y_mean_ps, 7.0, 1e-9);
}

/* PI servo against a clock with a rate error of -1400 ppb and a random-walk rate. */
struct plant {
	double offset_ps;        /* peer minus local */
	double err_ppb;          /* local clock rate error against the peer, without the servo */
	uint32_t rng;
};

static double plant_noise(struct plant *p)
{
	/* sum of 4 uniforms, about unit variance times 1/sqrt(3) */
	double a = 0;

	for (int i = 0; i < 4; i++) {
		p->rng = p->rng * 1664525U + 1013904223U;
		a += (double)(p->rng >> 8) / 16777216.0 - 0.5;
	}
	return a;
}

/* One period of 50 ms in 10 sub steps; returns the mean offset over the period. */
static double plant_run(struct plant *p, double rate_ppb, double period_s, double rw_ppb)
{
	double sum = 0;

	for (int i = 0; i < 10; i++) {
		p->err_ppb += rw_ppb * plant_noise(p) / 10.0;
		p->offset_ps -= (p->err_ppb + rate_ppb) * 1e3 * period_s / 10.0;
		sum += p->offset_ps;
	}
	return sum / 10.0;
}

ZTEST(ptp_chain, test_servo_converges)
{
	struct chain_servo s;
	struct plant p = {.offset_ps = 200000.0, .err_ppb = -1400.0, .rng = 7};
	double T_s = 0.05;
	double sq = 0, rate_ppb = 0;
	int n = 0;

	chain_servo_init(&s, T_s, 2.0, 500000.0);
	for (int k = 0; k < 1200; k++) {  /* 60 s */
		int64_t step_ps;
		double m_ps = plant_run(&p, rate_ppb, T_s, 0.2);

		rate_ppb = chain_servo_update(&s, m_ps, &step_ps);
		zassert_equal(step_ps, 0);
		if (k >= 800) {  /* the last 20 s */
			sq += p.offset_ps * p.offset_ps;
			n++;
		}
	}
	/* learned the rate: it cancels the clock error, up to the random walk */
	zassert_within(rate_ppb, -p.err_ppb, 5.0, "rate %f", rate_ppb);
	zassert_true(sqrt(sq / n) < 20000.0, "rms %f ps", sqrt(sq / n));
	zassert_equal(s.steps, 0U);
}

ZTEST(ptp_chain, test_servo_step_no_windup)
{
	struct chain_servo s;
	struct plant p = {.offset_ps = 3e6, .err_ppb = -1400.0, .rng = 3};
	double T_s = 0.05;
	double rate_ppb = 0, integ_before = 0;
	bool stepped_again = false;

	chain_servo_init(&s, T_s, 2.0, 500000.0);
	/* start 3 us off: the first update steps, and leaves the integrator at 0 */
	for (int k = 0; k < 1200; k++) {
		int64_t step_ps;
		double m_ps = plant_run(&p, rate_ppb, T_s, 0.0);

		if (k == 0) {
			zassert_true(fabs(m_ps) > 500000.0);
		}
		rate_ppb = chain_servo_update(&s, m_ps, &step_ps);
		if (step_ps != 0) {
			p.offset_ps -= (double)step_ps;
			if (k == 0) {
				zassert_equal(s.integ_ppb, 0.0, "integrator moved by the step");
				zassert_equal(rate_ppb, 0.0);
			}
		}
		if (k == 599) {
			/* converged: now the clock is bumped by 2 us (a step is due) */
			integ_before = s.integ_ppb;
			zassert_within(integ_before, 1400.0, 5.0);
			p.offset_ps += 2e6;
		}
		if (k == 600) {
			zassert_true(step_ps != 0, "step after the bump");
			stepped_again = true;
			zassert_equal(s.integ_ppb, integ_before, "step changed the integrator");
		}
	}
	zassert_true(stepped_again);
	zassert_true(s.steps >= 2U);
	/* after the step the loop is where it was: small offset, same rate, no overshoot */
	zassert_true(fabs(p.offset_ps) < 5000.0, "offset %f ps", p.offset_ps);
	zassert_within(rate_ppb, 1400.0, 1.0);
}

ZTEST(ptp_chain, test_servo_limit)
{
	struct chain_servo s;
	int64_t step_ps;
	double r;

	chain_servo_init(&s, 0.05, 2.0, 1e12);
	for (int i = 0; i < 2000; i++) {
		r = chain_servo_update(&s, 5e8, &step_ps);  /* a huge constant offset */
	}
	zassert_equal(r, CHAIN_SERVO_MAX_PPB);
	/* the integrator stayed at the limit: one update with the opposite sign leaves it at once */
	r = chain_servo_update(&s, -5e8, &step_ps);
	zassert_true(r < CHAIN_SERVO_MAX_PPB - 1000.0, "wound up: %f", r);
}

ZTEST_SUITE(ptp_chain, NULL, NULL, NULL, NULL, NULL);
