/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S3 vector unit (PIE) kernels of the channelized burst capture's 8-channel filter bank
 * at decimation 8, bit-exact with the C reference in esp_sdr_chancap_dsp.h: products accumulate
 * in the 40-bit QACC lanes, EE.SRCMB.S16.QACC shifts right arithmetically and saturates to
 * int16, as chancap_sat16(acc >> shift) does; EE.VADDS/VSUBS saturate like chancap_add16().
 * The work is split over the two CPUs: the ring's sink computes the branch sums
 * (esp_sdr_chancap_sums_n(), chancap_pfb_sums()), the channelizer thread the rest
 * (esp_sdr_chancap_post_n(), chancap_pfb_post()) and the powers of the gate and the detectors
 * (esp_sdr_chancap_power(), chancap_power_n()).
 *
 * A frame is 16 int16 (32 bytes): the 8 lanes' I, then their Q. All pointers 16-byte aligned.
 * The kernels clobber q0..q7, QACC, SAR and SAR_BYTE, which Zephyr does not save per thread:
 * on each CPU only one thread may use the vector unit.
 *
 * void esp_sdr_chancap_sums_n(const struct chancap_sums_job *job);
 *   job: +0 xi, +4 xq (frame 0's window, 8 vectors per plane), +8 hk (the 8 taps in window
 *        order), +12 k (8), +16 n (frames), +20 out (n frames), +24 shift, +28 stride (bytes
 *        from one frame's window to the next).
 *
 * void esp_sdr_chancap_post_n(const struct chancap_post_job *job);
 *   job: +0 in (n frames of branch sums), +4 out (n frames of channels), +8 n (even: frames
 *        alternate between no sign and a negation, the rotation exp(-j pi (2 mc + 1) j) of
 *        decimation 8), +12 tab: c1..c4 (the twiddle t8 of chancap_dft8_odd() in stage 1's
 *        layout), m_even, m_lo, m_hi (the -j of stage 2), 16 bytes each. The first frame is
 *        even.
 *
 * void esp_sdr_chancap_power(const struct chancap_power_job *job);
 *   job: +0 frames, +4 fine, +8 coarse (8 lanes each), +12 n (frames, even), +16 sf, +20 sc:
 *        per lane the sum over the n frames of I^2 + Q^2 as fine = sat16(sum >> sf) and
 *        coarse = sat16(sum >> (sf + sc)) (chancap_power_n()).
 *
 * uint32_t esp_sdr_chancap_energy(const frame_t *frames, uint32_t n, uint32_t shift);
 *   The sum over n frames (even) and their 8 lanes of I^2 + Q^2, sat32(sum >> shift)
 *   (chancap_energy()): the gate pass's power, one horizontal accumulation in ACCX.
 *
 * void esp_sdr_chancap_transpose8(const struct chancap_transpose_job *job);
 *   job: +0 frames (8), +4 out (8 lanes x 32 bytes: the lane's 8 samples, I and Q
 *        interleaved), +8 si, +12 sq (128 bytes of scratch each).
 *
 * void esp_sdr_chancap_mix(const struct chancap_mix_job *job);
 *   job: +0 xi, +4 xq (n input pairs, 16-byte aligned), +8 tab (per 8 pairs: cr, ci, -ci of the
 *        Q14 mixer, 48 bytes), +12 n (pairs, a multiple of 8), +16 mi, +20 mq (the mixed
 *        planes). m = sat16((x c) >> 14) per lane (chancap_ddc_mix()).
 *
 * void esp_sdr_chancap_ddc_fir(const struct chancap_fir_job *job);
 *   job: +0 mi, +4 mq (the mixed window buffer, 16-byte aligned: 32 pairs of history, then
 *        the sub-block), +8 h (4 vectors: the 32 taps in window order), +12 hs (5 vectors: the
 *        same taps 4 lanes on, zeros around), +16 out (n outputs, I and Q interleaved), +20 n
 *        (outputs, even), +24 shift, +28 d (4 or 8). Output j's window is the mixed pairs
 *        d j + d to d j + d + 31: at d = 8 vectors j + 1 to j + 4 with h; at d = 4 odd j the
 *        vectors (j + 1) / 2 on with h, even j the 5 vectors from j / 2 with hs.
 *        out = sat16(sum >> shift) (chancap_ddc_fir()); ACCX holds the 32 products exactly.
 *
 * void esp_sdr_chancap_copy(int16_t *dst, const int16_t *src, uint32_t n);
 *   n int16 (a multiple of 8), both 16-byte aligned: the C paths' hand-over copy on the ring's
 *   CPU (memcpy() is the ROM's).
 *
 * uint32_t esp_sdr_chancap_energy2(const int16_t *i, const int16_t *q, uint32_t n,
 *                                  uint32_t shift);
 *   n pairs (a multiple of 8) of planes i and q, 16-byte aligned: sat32(sum of I^2 + Q^2 >>
 *   shift), the C paths' sub-block power on the ring's CPU.
 *
 * The sums kernel is unrolled for 8 taps a branch, its loads two instructions ahead of their
 * use: the multiply-accumulates form one chain through QACC.
 */

	.section .iram1.esp_sdr_chancap_pie, "ax"

/*
 * One plane of a frame: a12 the window (advances), a10 the taps (advances), the sum
 * sat16(acc >> a9) to a8 (advances 16). Every load two instructions ahead of its use.
 */
	.macro	sums_plane
	ee.zero.qacc
	ee.vld.128.ip	q0, a12, 16	/* x0 */
	ee.vld.128.ip	q1, a10, 16	/* h0 */
	ee.vld.128.ip	q2, a12, 16	/* x1 */
	ee.vld.128.ip	q3, a10, 16	/* h1 */
	ee.vmulas.s16.qacc.ld.ip	q4, a12, 16, q0, q1	/* x0 h0, load x2 */
	ee.vld.128.ip	q5, a10, 16	/* h2 */
	ee.vmulas.s16.qacc.ld.ip	q0, a12, 16, q2, q3	/* x1 h1, load x3 */
	ee.vld.128.ip	q1, a10, 16	/* h3 */
	ee.vmulas.s16.qacc.ld.ip	q2, a12, 16, q4, q5	/* x2 h2, load x4 */
	ee.vld.128.ip	q3, a10, 16	/* h4 */
	ee.vmulas.s16.qacc.ld.ip	q4, a12, 16, q0, q1	/* x3 h3, load x5 */
	ee.vld.128.ip	q5, a10, 16	/* h5 */
	ee.vmulas.s16.qacc.ld.ip	q0, a12, 16, q2, q3	/* x4 h4, load x6 */
	ee.vld.128.ip	q1, a10, 16	/* h6 */
	ee.vmulas.s16.qacc.ld.ip	q2, a12, 16, q4, q5	/* x5 h5, load x7 */
	ee.vld.128.ip	q3, a10, 16	/* h7 */
	ee.vmulas.s16.qacc	q0, q1	/* x6 h6 */
	ee.vmulas.s16.qacc	q2, q3	/* x7 h7 */
	ee.srcmb.s16.qacc	q6, a9, 0
	ee.vst.128.ip	q6, a8, 16
	.endm

	.align	4
	.global	esp_sdr_chancap_sums_n
	.type	esp_sdr_chancap_sums_n, @function
esp_sdr_chancap_sums_n:
	entry	a1, 32
	l32i	a3, a2, 0		/* xi */
	l32i	a4, a2, 4		/* xq */
	l32i	a5, a2, 8		/* hk */
	l32i	a7, a2, 16		/* n */
	l32i	a8, a2, 20		/* out */
	l32i	a9, a2, 24		/* shift */
	l32i	a11, a2, 28		/* stride */
	beqz	a7, .Lsums_done
.Lsums_frame:
	mov	a10, a5
	mov	a12, a3
	sums_plane
	mov	a10, a5
	mov	a12, a4
	sums_plane
	add	a3, a3, a11
	add	a4, a4, a11
	addi	a7, a7, -1
	bnez	a7, .Lsums_frame
.Lsums_done:
	retw.n
	.size	esp_sdr_chancap_sums_n, . - esp_sdr_chancap_sums_n

	.align	4
	.global	esp_sdr_chancap_copy
	.type	esp_sdr_chancap_copy, @function
esp_sdr_chancap_copy:
	entry	a1, 32
	srli	a4, a4, 3		/* vectors */
	bbci	a4, 0, .Lcopy_pairs
	ee.vld.128.ip	q0, a3, 16
	ee.vst.128.ip	q0, a2, 16
.Lcopy_pairs:
	srli	a4, a4, 1
	loopnez	a4, .Lcopy_done
	ee.vld.128.ip	q0, a3, 16
	ee.vld.128.ip	q1, a3, 16
	ee.vst.128.ip	q0, a2, 16
	ee.vst.128.ip	q1, a2, 16
.Lcopy_done:
	retw.n
	.size	esp_sdr_chancap_copy, . - esp_sdr_chancap_copy

	.align	4
	.global	esp_sdr_chancap_energy2
	.type	esp_sdr_chancap_energy2, @function
esp_sdr_chancap_energy2:
	entry	a1, 32
	ee.zero.accx
	srli	a4, a4, 3		/* vectors per plane */
	loopnez	a4, .Lenergy2_done
	ee.vld.128.ip	q0, a2, 16
	ee.vld.128.ip	q1, a3, 16
	ee.vmulas.s16.accx	q0, q0
	ee.vmulas.s16.accx	q1, q1
.Lenergy2_done:
	ee.srs.accx	a2, a5, 0
	retw.n
	.size	esp_sdr_chancap_energy2, . - esp_sdr_chancap_energy2

/*
 * The channelizer thread's kernels: flash code, cached (only the sink's kernel above runs
 * on the ring's CPU, interrupts masked, from IRAM).
 */
	.section .text.esp_sdr_chancap_pie, "ax"

/*
 * One frame of esp_sdr_chancap_post_n() (chancap_dft8_odd(), bit-exact): a3 in (advances 32),
 * a4 out (advances 32), a5 tab, a6 = 14; SAR_BYTE 8 (EE.SRC.Q qa, x, y = [x lanes 4..7,
 * y lanes 0..3]) and SAR 0.
 *
 * Stage 1: U = [I lanes 0..3, Q lanes 0..3] and V = [Q lanes 4..7, I lanes 4..7] give
 * U + V = [sI, eQ] and U - V = [eI, sQ] (s = u_i - j u_{i+4}, e = u_i + j u_{i+4}). With the
 * latter's halves swapped, [sQ, eI], every lane holds one stage-1 value's I and Q, so the twiddle
 * takes two products per plane with per-lane coefficients c1..c4 (t8r and t8i, roles swapped
 * between the halves).
 * Stage 2: EE.VUNZIP.32 of the twiddled I and Q lines up the pairs (0,2) (1,3) (4,6) (5,7) of
 * both planes: S holds the sums (I's lanes, then Q's), D the differences. The -j on lanes 5
 * and 7 (d13 and d57): their I from D's Q half, their Q minus D's I half, by the 0/1 masks
 * m_even, m_lo, m_hi (exact products at SAR 0; the negation saturates in EE.VSUBS).
 * Stage 3: EE.VUNZIP.16 of S and that D lines up the pairs (0,1) (2,3) (4,5) (6,7); the sums
 * and differences, unzipped by 32 bits, are the bins' I and Q in natural order.
 */
	.macro	post_frame neg
	ee.vld.128.ip	q0, a3, 16		/* I sums */
	ee.vld.128.ip	q1, a3, 16		/* Q sums */
	mov	a7, a5
	ee.src.q	q2, q0, q0		/* I, halves swapped */
	ee.src.q	q3, q2, q1		/* U = [I lo, Q lo] */
	ee.src.q	q4, q1, q2		/* V = [Q hi, I hi] */
	ee.vadds.s16.ld.incp	q0, a7, q5, q3, q4	/* [sI, eQ]; c1 */
	ee.vsubs.s16.ld.incp	q1, a7, q6, q3, q4	/* [eI, sQ]; c2 */
	ee.src.q.ld.ip	q2, a7, 16, q6, q6	/* [sQ, eI]; c3 */
	ee.vld.128.ip	q3, a7, 16		/* c4 */
	ee.zero.qacc
	ee.vmulas.s16.qacc.ld.ip	q4, a7, 16, q5, q0	/* m_even */
	ee.vmulas.s16.qacc.ld.ip	q7, a7, 16, q6, q1	/* m_lo */
	ee.srcmb.s16.qacc	q0, a6, 0	/* twiddled I */
	ee.zero.qacc
	ee.vmulas.s16.qacc.ld.ip	q1, a7, 16, q5, q2	/* m_hi */
	ee.vmulas.s16.qacc	q6, q3
	ee.srcmb.s16.qacc	q2, a6, 0	/* twiddled Q */
	/* Stage 2. */
	ee.vunzip.32	q0, q2			/* q0 lanes [0,1,4,5] of I then Q, q2 [2,3,6,7] */
	ee.vadds.s16	q3, q0, q2		/* S */
	ee.vsubs.s16	q5, q0, q2		/* D */
	ee.src.q	q6, q5, q5		/* D, halves swapped */
	ee.vmul.s16	q0, q5, q4		/* D on lanes 0, 2, 4, 6 */
	ee.vmul.s16	q2, q6, q7		/* its Q half on lanes 1, 3 */
	ee.vmul.s16	q4, q6, q1		/* its I half on lanes 5, 7 */
	ee.vadds.s16	q0, q0, q2
	ee.vsubs.s16	q0, q0, q4		/* D times -j on lanes 5 and 7 */
	/* Stage 3. */
	ee.vunzip.16	q3, q0			/* pairs' first lanes in q3, second in q0 */
	ee.vadds.s16	q1, q3, q0
	ee.vsubs.s16	q2, q3, q0
	ee.vunzip.32	q1, q2			/* the bins' I in q1, Q in q2 */
	.if \neg
	/* Odd frame: the rotation is a negation (saturating, as the reference's Q14 -1). */
	ee.zero.q	q3
	ee.vsubs.s16	q1, q3, q1
	ee.vsubs.s16	q2, q3, q2
	.endif
	ee.vst.128.ip	q1, a4, 16
	ee.vst.128.ip	q2, a4, 16
	.endm

	.align	4
	.global	esp_sdr_chancap_post_n
	.type	esp_sdr_chancap_post_n, @function
esp_sdr_chancap_post_n:
	entry	a1, 32
	l32i	a3, a2, 0		/* in */
	l32i	a4, a2, 4		/* out */
	l32i	a8, a2, 8		/* n */
	l32i	a5, a2, 12		/* tab */
	movi	a6, 8
	wur.sar_byte	a6
	ssai	0			/* EE.VMUL.S16 by the 0 / 1 masks: exact */
	movi	a6, 14
	srli	a8, a8, 1
	beqz	a8, .Lpost_done
.Lpost_pair:
	post_frame 0
	post_frame 1
	addi	a8, a8, -1
	bnez	a8, .Lpost_pair
.Lpost_done:
	retw.n
	.size	esp_sdr_chancap_post_n, . - esp_sdr_chancap_post_n

	.align	4
	.global	esp_sdr_chancap_power
	.type	esp_sdr_chancap_power, @function
esp_sdr_chancap_power:
	entry	a1, 32
	l32i	a3, a2, 0		/* frames: 2 n vectors, I and Q of each */
	l32i	a4, a2, 12		/* n */
	ee.zero.qacc
	ee.vld.128.ip	q0, a3, 16
	ee.vld.128.ip	q1, a3, 16
	srli	a4, a4, 1
	addi	a4, a4, -1		/* pairs of frames before the last */
	/* Every load two instructions ahead of its use. */
	loopnez	a4, .Lpower_last
	ee.vmulas.s16.qacc.ld.ip	q2, a3, 16, q0, q0
	ee.vmulas.s16.qacc.ld.ip	q3, a3, 16, q1, q1
	ee.vmulas.s16.qacc.ld.ip	q0, a3, 16, q2, q2
	ee.vmulas.s16.qacc.ld.ip	q1, a3, 16, q3, q3
.Lpower_last:
	ee.vmulas.s16.qacc.ld.ip	q2, a3, 16, q0, q0
	ee.vmulas.s16.qacc.ld.ip	q3, a3, 16, q1, q1
	ee.vmulas.s16.qacc	q2, q2
	ee.vmulas.s16.qacc	q3, q3
	l32i	a5, a2, 16		/* sf */
	ee.srcmb.s16.qacc	q4, a5, 0	/* fine = sat16(sum >> sf); QACC now sum >> sf */
	l32i	a6, a2, 4
	ee.vst.128.ip	q4, a6, 0
	l32i	a5, a2, 20		/* sc */
	ee.srcmb.s16.qacc	q4, a5, 0	/* coarse = sat16(sum >> (sf + sc)) */
	l32i	a6, a2, 8
	ee.vst.128.ip	q4, a6, 0
	retw.n
	.size	esp_sdr_chancap_power, . - esp_sdr_chancap_power

	.align	4
	.global	esp_sdr_chancap_energy
	.type	esp_sdr_chancap_energy, @function
esp_sdr_chancap_energy:
	entry	a1, 32
	ee.zero.accx
	ee.vld.128.ip	q0, a2, 16
	ee.vld.128.ip	q1, a2, 16
	srli	a3, a3, 1
	addi	a3, a3, -1		/* pairs of frames before the last */
	loopnez	a3, .Lenergy_last
	ee.vmulas.s16.accx.ld.ip	q2, a2, 16, q0, q0
	ee.vmulas.s16.accx.ld.ip	q3, a2, 16, q1, q1
	ee.vmulas.s16.accx.ld.ip	q0, a2, 16, q2, q2
	ee.vmulas.s16.accx.ld.ip	q1, a2, 16, q3, q3
.Lenergy_last:
	ee.vmulas.s16.accx.ld.ip	q2, a2, 16, q0, q0
	ee.vmulas.s16.accx.ld.ip	q3, a2, 16, q1, q1
	ee.vmulas.s16.accx	q2, q2
	ee.vmulas.s16.accx	q3, q3
	ee.srs.accx	a2, a4, 0
	retw.n
	.size	esp_sdr_chancap_energy, . - esp_sdr_chancap_energy

/*
 * Stages 1 and 2 of an 8 x 8 transpose on q0..q7 (row f = frame f, lane = channel): zip.16 of
 * row pairs, then zip.32 of their results. Afterwards q0 = [column 0 rows 0..3 | column 1 rows
 * 0..3], q4 the same rows 4..7; q2/q6 columns 2, 3; q1/q5 columns 4, 5; q3/q7 columns 6, 7.
 */
	.macro	transpose12
	ee.vzip.16	q0, q1
	ee.vzip.16	q2, q3
	ee.vzip.16	q4, q5
	ee.vzip.16	q6, q7
	ee.vzip.32	q0, q2
	ee.vzip.32	q1, q3
	ee.vzip.32	q4, q6
	ee.vzip.32	q5, q7
	.endm

/* One plane's 8 rows, 32 bytes apart (frame-major), from as into q0..q7. */
	.macro	load8 as
	ee.vld.128.ip	q0, \as, 32
	ee.vld.128.ip	q1, \as, 32
	ee.vld.128.ip	q2, \as, 32
	ee.vld.128.ip	q3, \as, 32
	ee.vld.128.ip	q4, \as, 32
	ee.vld.128.ip	q5, \as, 32
	ee.vld.128.ip	q6, \as, 32
	ee.vld.128.ip	q7, \as, 32
	.endm

/* The transposed halves to the scratch at as, in column-pair order (0,1) (2,3) (4,5) (6,7). */
	.macro	store8 as
	ee.vst.128.ip	q0, \as, 16
	ee.vst.128.ip	q4, \as, 16
	ee.vst.128.ip	q2, \as, 16
	ee.vst.128.ip	q6, \as, 16
	ee.vst.128.ip	q1, \as, 16
	ee.vst.128.ip	q5, \as, 16
	ee.vst.128.ip	q3, \as, 16
	ee.vst.128.ip	q7, \as, 16
	.endm

/*
 * One column pair from the scratch (a3: I halves, a4: Q halves, each lo then hi) to the two
 * lanes' outputs at a5 and a6. With SAR_BYTE 8, SRC.Q qa, x, y = [x high, y low].
 */
	.macro	join_pair
	ee.vld.128.ip	q0, a3, 16	/* I lo: [ce rows 0..3 | co rows 0..3] */
	ee.vld.128.ip	q1, a3, 16	/* I hi: [ce rows 4..7 | co rows 4..7] */
	ee.vld.128.ip	q2, a4, 16	/* Q lo */
	ee.vld.128.ip	q3, a4, 16	/* Q hi */
	ee.src.q	q4, q0, q0	/* [I lo high, I lo low] */
	ee.src.q	q5, q4, q1	/* I column even: [ce rows 0..3, ce rows 4..7] */
	ee.src.q	q4, q1, q1
	ee.src.q	q6, q0, q4	/* I column odd: [co rows 0..3, co rows 4..7] */
	ee.src.q	q4, q2, q2
	ee.src.q	q7, q4, q3	/* Q column even */
	ee.src.q	q4, q3, q3
	ee.src.q	q0, q2, q4	/* Q column odd */
	ee.vzip.16	q5, q7		/* even: I/Q pairs of frames 0..3 in q5, 4..7 in q7 */
	ee.vzip.16	q6, q0
	ee.vst.128.ip	q5, a5, 16
	ee.vst.128.ip	q7, a5, 0
	ee.vst.128.ip	q6, a6, 16
	ee.vst.128.ip	q0, a6, 0
	.endm

	.align	4
	.global	esp_sdr_chancap_transpose8
	.type	esp_sdr_chancap_transpose8, @function
esp_sdr_chancap_transpose8:
	entry	a1, 32
	movi	a5, 8
	wur.sar_byte	a5
	/* The I plane's transpose halves to si, then the Q plane's to sq. */
	l32i	a3, a2, 0
	l32i	a7, a2, 8
	load8	a3
	transpose12
	store8	a7
	l32i	a4, a2, 0
	addi	a4, a4, 16
	l32i	a7, a2, 12
	load8	a4
	transpose12
	store8	a7
	/* Join and interleave, a column pair at a time: lane c to out + 32 c. */
	l32i	a3, a2, 8
	l32i	a4, a2, 12
	l32i	a5, a2, 4
	addi	a6, a5, 32
	join_pair
	addi	a5, a5, 48
	addi	a6, a6, 48
	join_pair
	addi	a5, a5, 48
	addi	a6, a6, 48
	join_pair
	addi	a5, a5, 48
	addi	a6, a6, 48
	join_pair
	retw.n
	.size	esp_sdr_chancap_transpose8, . - esp_sdr_chancap_transpose8
/*
 * Draft: the decimator bank's kernels (CPU 0, flash code), to go into esp_sdr_chancap_pie.S.
 *
 * void esp_sdr_chancap_mix(const struct chancap_mix_job *job);
 *   job: +0 xi, +4 xq (n input pairs), +8 tab (per 8 pairs: cr, ci, -ci, 48 bytes, the period
 *        unrolled so that a call never wraps), +12 n (pairs, a multiple of 8), +16 mi, +20 mq
 *        (the mixed planes). m = sat16((x c) >> 14) per lane (chancap_ddc_mix()).
 *
 * void esp_sdr_chancap_ddc_fir(const struct chancap_fir_job *job);
 *   job: +0 mi, +4 mq (the mixed window buffer, 16-byte aligned), +8 h (4 vectors: the 32 taps in
 *        window order), +12 hs (5 vectors: the same taps 4 lanes on, zeros around), +16 out
 *        (n outputs, I and Q interleaved), +20 n (outputs, even), +24 shift: output j's window
 *        is the mixed samples 4 j + 4 to 4 j + 35 (odd j: vectors from (j + 1) / 2 with h; even
 *        j: from j / 2 with hs), out = sat16(sum >> shift) (chancap_ddc_fir()).
 */
	.align	4
	.global	esp_sdr_chancap_mix
	.type	esp_sdr_chancap_mix, @function
esp_sdr_chancap_mix:
	entry	a1, 32
	l32i	a3, a2, 0		/* xi */
	l32i	a4, a2, 4		/* xq */
	l32i	a5, a2, 8		/* tab */
	l32i	a6, a2, 12		/* n */
	l32i	a7, a2, 16		/* mi */
	l32i	a8, a2, 20		/* mq */
	movi	a9, 14
	srli	a6, a6, 3
	loopnez	a6, .Lmix_done
	ee.vld.128.ip	q0, a3, 16	/* I */
	ee.vld.128.ip	q2, a5, 16	/* cr */
	ee.vld.128.ip	q1, a4, 16	/* Q */
	ee.vld.128.ip	q4, a5, 16	/* ci */
	ee.zero.qacc
	ee.vmulas.s16.qacc.ld.ip	q3, a5, 16, q0, q2	/* I cr; -ci */
	ee.vmulas.s16.qacc	q1, q3			/* Q (-ci) */
	ee.srcmb.s16.qacc	q5, a9, 0
	ee.zero.qacc
	ee.vmulas.s16.qacc	q0, q4			/* I ci */
	ee.vmulas.s16.qacc	q1, q2			/* Q cr */
	ee.srcmb.s16.qacc	q6, a9, 0
	ee.vst.128.ip	q5, a7, 16
	ee.vst.128.ip	q6, a8, 16
.Lmix_done:
	retw.n
	.size	esp_sdr_chancap_mix, . - esp_sdr_chancap_mix

/* One plane of an odd output: a10 window (advances 64), taps q4..q7, sat16 to a11 (+= 2). */
	.macro	fir_plane4
	ee.zero.accx
	ee.vld.128.ip	q0, a10, 16
	ee.vmulas.s16.accx.ld.ip	q1, a10, 16, q0, q4
	ee.vmulas.s16.accx.ld.ip	q0, a10, 16, q1, q5
	ee.vmulas.s16.accx.ld.ip	q1, a10, 16, q0, q6
	ee.vmulas.s16.accx	q1, q7
	ee.srs.accx	a12, a9, 0
	clamps	a12, a12, 15
	s16i	a12, a11, 0
	addi	a11, a11, 2
	.endm

/* One plane of an even output: a10 window (advances 80), taps q3..q7. */
	.macro	fir_plane5
	ee.zero.accx
	ee.vld.128.ip	q0, a10, 16
	ee.vmulas.s16.accx.ld.ip	q1, a10, 16, q0, q3
	ee.vmulas.s16.accx.ld.ip	q0, a10, 16, q1, q4
	ee.vmulas.s16.accx.ld.ip	q1, a10, 16, q0, q5
	ee.vmulas.s16.accx.ld.ip	q0, a10, 16, q1, q6
	ee.vmulas.s16.accx	q0, q7
	ee.srs.accx	a12, a9, 0
	clamps	a12, a12, 15
	s16i	a12, a11, 0
	addi	a11, a11, 2
	.endm

	.align	4
	.global	esp_sdr_chancap_ddc_fir
	.type	esp_sdr_chancap_ddc_fir, @function
esp_sdr_chancap_ddc_fir:
	entry	a1, 32
	l32i	a3, a2, 0		/* mi */
	l32i	a4, a2, 4		/* mq */
	l32i	a8, a2, 20		/* n */
	l32i	a9, a2, 24		/* shift */
	srli	a8, a8, 1		/* outputs of each parity */
	/* Odd outputs: taps h in q4..q7, windows from vector 1, a vector apart. */
	l32i	a5, a2, 8
	ee.vld.128.ip	q4, a5, 16
	ee.vld.128.ip	q5, a5, 16
	ee.vld.128.ip	q6, a5, 16
	ee.vld.128.ip	q7, a5, 16
	l32i	a11, a2, 16
	addi	a11, a11, 4		/* output 1 */
	addi	a6, a3, 16
	addi	a7, a4, 16
	loopnez	a8, .Lfir_odd_done
	mov	a10, a6
	fir_plane4
	mov	a10, a7
	fir_plane4
	addi	a6, a6, 16
	addi	a7, a7, 16
	addi	a11, a11, 4		/* past the even output */
.Lfir_odd_done:
	/* Even outputs: taps hs in q3..q7, windows from vector 0. */
	l32i	a5, a2, 12
	ee.vld.128.ip	q3, a5, 16
	ee.vld.128.ip	q4, a5, 16
	ee.vld.128.ip	q5, a5, 16
	ee.vld.128.ip	q6, a5, 16
	ee.vld.128.ip	q7, a5, 16
	l32i	a11, a2, 16
	mov	a6, a3
	mov	a7, a4
	l32i	a8, a2, 20
	srli	a8, a8, 1
	loopnez	a8, .Lfir_even_done
	mov	a10, a6
	fir_plane5
	mov	a10, a7
	fir_plane5
	addi	a6, a6, 16
	addi	a7, a7, 16
	addi	a11, a11, 4
.Lfir_even_done:
	retw.n
	.size	esp_sdr_chancap_ddc_fir, . - esp_sdr_chancap_ddc_fir
