/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The arithmetic of the PTP chain: MAC stamps to picoseconds, the pairing of frames into
 * exchanges, offset and delay of an exchange, the least-squares line. Only stdint, stdbool and
 * math: the host unit test (tests/unit/ptp_chain) compiles chain_math.c as it is.
 *
 * Time is int64 picoseconds. A stamp is (sec, subsec) from the MAC; seconds are taken relative to
 * a base second (the one of t1), so a clock that is off by up to CHAIN_SEC_LIM seconds (97 days)
 * still fits; beyond that the difference saturates.
 */
#ifndef CHAIN_MATH_H_
#define CHAIN_MATH_H_

#include <stdbool.h>
#include <stdint.h>

/* The hot helpers are part of the caller: it runs from IRAM */
#define CHAIN_INLINE static inline __attribute__((always_inline))

#define CHAIN_PS_PER_SEC 1000000000000LL
#define CHAIN_SEC_LIM    8000000

/* 1e12 = 2^12 * 5^12: a binary sub-second (units of 2^-31 s) is subsec * 5^12 / 2^19 ps, exact */
#define CHAIN_BIN_MUL    244140625ULL
#define CHAIN_BIN_SHIFT  19
#define CHAIN_BIN_MASK   0x7fffffffU
#define CHAIN_NS_PER_SEC 1000000000U

/* Own frames kept for pairing (a power of two); the chain needs two */
#define CHAIN_TXN        8U
/* A frame this far behind the last one is stale (reordered or repeated), further is a peer restart */
#define CHAIN_STALE_WIN  64
/* A jump of this many frames or more is a restart of the peer, not a loss */
#define CHAIN_RESYNC     65536U

struct chain_stamp {
	uint32_t sec;
	uint32_t subsec;
	bool valid;
};

/** a is older than b, u32 sequence numbers with wrap */
CHAIN_INLINE bool chain_seq_lt(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

/** Sub-second field to ps: digital rollover counts ns, binary counts 2^-31 s. */
CHAIN_INLINE int64_t chain_subsec_to_ps(uint32_t subsec, bool digital)
{
	if (digital) {
		if (subsec >= CHAIN_NS_PER_SEC) {
			subsec = CHAIN_NS_PER_SEC - 1U;
		}
		return (int64_t)subsec * 1000;
	}
	return (int64_t)(((uint64_t)(subsec & CHAIN_BIN_MASK) * CHAIN_BIN_MUL) >> CHAIN_BIN_SHIFT);
}

/** Stamp in ps counted from the start of second base_sec (negative before it; seconds saturate). */
CHAIN_INLINE int64_t chain_stamp_rel_ps(const struct chain_stamp *s, uint32_t base_sec,
					 bool digital)
{
	int32_t ds = (int32_t)(s->sec - base_sec);

	if (ds > CHAIN_SEC_LIM) {
		ds = CHAIN_SEC_LIM;
	} else if (ds < -CHAIN_SEC_LIM) {
		ds = -CHAIN_SEC_LIM;
	}
	return (int64_t)ds * CHAIN_PS_PER_SEC + chain_subsec_to_ps(s->subsec, digital);
}

/*
 * An exchange as seen from the local node X, the peer being Y:
 * t1 = X tx, t2 = Y rx, t3 = Y tx, t4 = X rx (all relative to base_sec).
 * A pair started by X is natural order. A pair started by Y (Y tx p, X rx p, X tx s, Y rx s) is
 * stored as t1 = X tx s, t2 = Y rx s, t3 = Y tx p, t4 = X rx p: the same formulas give Y minus X.
 */
struct chain_xchg {
	uint32_t seq;      /* first frame of the pair */
	uint32_t base_sec;
	int64_t t_ps[4];
};

/** Y minus X, ps (floor of half ps) */
CHAIN_INLINE int64_t chain_xchg_offset_ps(const struct chain_xchg *x)
{
	return ((x->t_ps[1] - x->t_ps[0]) - (x->t_ps[3] - x->t_ps[2])) >> 1;
}

/** Mean of the two one-way delays, ps */
CHAIN_INLINE int64_t chain_xchg_delay_ps(const struct chain_xchg *x)
{
	return ((x->t_ps[1] - x->t_ps[0]) + (x->t_ps[3] - x->t_ps[2])) >> 1;
}

/* Pairing */

struct chain_txrec {
	bool used;
	bool is_reply;
	uint32_t seq;
	uint32_t reply_to;
	struct chain_stamp stamp;
};

struct chain_rxrec {
	bool valid;
	bool is_reply;
	uint32_t seq;
	uint32_t reply_to;
	struct chain_stamp rx;  /* local rx stamp of this frame */
	uint32_t crx_seq;       /* the stamp the peer carried: its rx of frame crx_seq */
	struct chain_stamp crx;
};

struct chain_pair {
	bool digital;
	struct chain_txrec tx[CHAIN_TXN];
	struct chain_rxrec last;
};

/** A received chain frame: its header, the stamps it carries, and the local rx stamp. */
struct chain_rx_in {
	uint32_t seq;
	bool is_reply;
	uint32_t reply_to;
	uint32_t rx_seq;
	struct chain_stamp rx;  /* peer's rx stamp of its last received frame rx_seq */
	uint32_t tx_seq;
	struct chain_stamp tx;  /* peer's tx stamp of its previous frame tx_seq */
	struct chain_stamp local_rx;
};

struct chain_rx_res {
	uint8_t n;              /* exchanges completed by this frame (0 to 2) */
	bool stale;             /* repeated or reordered: ignored */
	uint32_t lost;          /* peer frames missing since the last one */
	struct chain_xchg x[2];
};

void chain_pair_init(struct chain_pair *p, bool digital);
/** Own frame seq is queued (reply_to meaningful if is_reply). */
void chain_tx_register(struct chain_pair *p, uint32_t seq, uint32_t reply_to, bool is_reply);
/** The MAC's tx stamp of own frame seq arrived. */
void chain_tx_stamp(struct chain_pair *p, uint32_t seq, const struct chain_stamp *s);
/** The tx stamp of own frame seq, false if unknown or the stamp is invalid. */
bool chain_tx_get(const struct chain_pair *p, uint32_t seq, struct chain_stamp *s);
/** Process a received frame: completes the exchanges it allows, then becomes the last frame. */
void chain_rx(struct chain_pair *p, const struct chain_rx_in *in, struct chain_rx_res *res);

/* Least squares line y = y_mean + slope (x - x_mean), accumulated sample by sample */

struct chain_fit {
	uint32_t n;
	double x0_ps, y0_ps;                 /* first sample: the sums are about it */
	double sx_ps, sy_ps;
	double sxx_ps2, sxy_ps2, syy_ps2;
};

struct chain_fit_res {
	uint32_t n;
	double x_mean_ps;
	double y_mean_ps;    /* the line at x_mean_ps */
	double rate_ppb;     /* slope of y over x */
	double rms_ps;       /* residual */
};

void chain_fit_reset(struct chain_fit *f);
void chain_fit_add(struct chain_fit *f, int64_t x_ps, int64_t y_ps);
/** 0 if the line is defined; -1 (means filled, rate 0) for fewer than 2 points or equal x. */
int chain_fit_end(const struct chain_fit *f, struct chain_fit_res *r);

/*
 * Rate-only PI servo of the uke. Input: offset of the seme (peer minus local, ps), the mean over
 * one update period. Output: the absolute rate of the local clock in ppb of nominal (ratio
 * 1 + rate_ppb * 1e-9). Positive offset means the local clock is behind: speed up.
 *
 * Plant: offset' = -1e3 (rate_ppb + clock error) ps/s. With rate = kp e + integ and
 * integ' = ki e this is a second order loop of natural frequency wn = TAU bw_hz,
 * critically damped: kp = 2 wn / 1e3, ki = wn^2 / 1e3 (ppb per ps, ppb per ps and s).
 * A step (|offset| above the clamp) is returned to the caller and the update leaves rate and
 * integrator alone: the learned rate survives a step and nothing winds up.
 */
#define CHAIN_TAU 6.283185307179586
#define CHAIN_SERVO_MAX_PPB 100000.0

struct chain_servo {
	double kp;          /* ppb per ps */
	double ki;          /* ppb per ps and s */
	double period_s;
	double clamp_ps;    /* above this the clock is stepped */
	double integ_ppb;
	double rate_ppb;    /* last output */
	uint32_t steps;
};

void chain_servo_init(struct chain_servo *s, double period_s, double bw_hz, double clamp_ps);
/** One update; *step_ps is the step to apply to the local clock (0: none). Returns rate_ppb. */
double chain_servo_update(struct chain_servo *s, double offset_ps, int64_t *step_ps);

#endif /* CHAIN_MATH_H_ */
