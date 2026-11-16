# SPDX-License-Identifier: GPL-3.0-or-later
"""Radix-2 decimation-in-time FFT, in place, N = 16..512.

One 512 x 32 memory (re in the low half), one butterfly every two cycles: read A and B,
B * W on two multipliers over two cycles, write A' and B'. The pipeline drains between
stages. Every stage halves (rounded): with input magnitudes below 2**15 nothing overflows,
and the result is X / N. model.fft() is the reference, bit for bit.

Load: write sample n (natural order) at `ld_addr`; it lands bit-reversed. Unload: after
`done`, read bin k at `rd_addr`, data the cycle after.

Timeline of a butterfly issued at t0: t0 read A and sin, t1 read B and cos, t2 data of B
(A and sin captured at the end of t1), t3 and t4 the products, t4 and t5 the rounded real and
imaginary parts of B * W, t6 write A', t7 write B'. Every pipeline register lives two cycles.
"""

from amaranth.hdl import Cat, Module, Mux, Signal, signed
from amaranth.lib import memory, wiring
from amaranth.lib.wiring import In, Out

from .model import FFT_MAX, FFT_MAX_LOG2, TW_QUARTER

__all__ = ["FFT"]


class FFT(wiring.Component):
    log2n: In(4)

    ld_addr: In(FFT_MAX_LOG2)
    ld_re: In(signed(16))
    ld_im: In(signed(16))
    ld_we: In(1)

    start: In(1)
    busy: Out(1)
    done: Out(1)
    exp: Out(5)

    rd_addr: In(FFT_MAX_LOG2)
    rd_re: Out(signed(16))
    rd_im: Out(signed(16))

    def elaborate(self, platform):
        m = Module()
        m.submodules.mem = mem = memory.Memory(shape=32, depth=FFT_MAX, init=[])
        wp = mem.write_port()
        rp = mem.read_port()
        # The quarter wave without its last entry (1.0, made up below).
        q = FFT_MAX // 4
        m.submodules.rom = rom = memory.Memory(shape=16, depth=q, init=TW_QUARTER[:q])
        romp = rom.read_port()
        rom_addr = Signal(FFT_MAX_LOG2 - 1)
        rom_one = Signal()
        rom_data = Signal(16)
        m.d.comb += [romp.addr.eq(rom_addr), rom_data.eq(Mux(rom_one, TW_QUARTER[q],
                                                              romp.data))]

        # Load, bit-reversed for the configured size.
        rev = Signal(FFT_MAX_LOG2)
        rev_shift = Signal(4)
        m.d.comb += [
            rev_shift.eq(FFT_MAX_LOG2 - self.log2n),
            rev.eq(Cat(*reversed(list(self.ld_addr))) >> rev_shift),
        ]

        stage = Signal(4)
        bfly = Signal(FFT_MAX_LOG2)
        issue = Signal()
        ph = Signal()
        half = Signal(FFT_MAX_LOG2)
        log2n_1 = Signal(4)
        m.d.comb += [log2n_1.eq(self.log2n - 1), half.eq(1 << log2n_1)]

        # Butterfly addresses, stepped: j within the group, a skips the B half after each
        # group, t = j * 512 / span.
        A = FFT_MAX_LOG2
        span = Signal(A)
        tstep = Signal(A)
        j = Signal(A - 1)
        a = Signal(A)
        b = Signal(A)
        t = Signal(A - 1)
        m.d.comb += b.eq(a + span)

        # t0 registers.
        a0, b0, c0, neg0 = Signal(A), Signal(A), Signal(A - 1), Signal()
        # Addresses down the pipe (each holds two cycles).
        a2, b2, a4, b4, b6 = (Signal(A, name=n) for n in ("a2", "b2", "a4", "b4", "b6"))
        A_re, A_im = Signal(signed(16)), Signal(signed(16))
        A2_re, A2_im = Signal(signed(16)), Signal(signed(16))
        A3_re, A3_im = Signal(signed(16)), Signal(signed(16))
        S = Signal(signed(16))
        neg1 = Signal()
        B_re, B_im = Signal(signed(16)), Signal(signed(16))
        Wr, Ws = Signal(signed(16)), Signal(signed(16))
        p1, p2 = Signal(signed(32)), Signal(signed(32))
        P_re, P_im = Signal(signed(18)), Signal(signed(18))
        Bo_re, Bo_im = Signal(signed(16)), Signal(signed(16))

        v = Signal(8)           # v[k]: a butterfly is at t(k + 1)
        m.d.sync += v.eq(Cat(issue & ~ph, v[:7]))

        # Read port and ROM addresses.
        with m.If(self.busy):
            with m.If(issue & ~ph):
                # sin at t, cos at q - t (or -(t - q) in the second quadrant): the index q
                # itself is 1.0, outside the table.
                sidx = Mux(t < q, t, 2 * q - t)
                m.d.comb += [rp.addr.eq(a), rom_addr.eq(sidx)]
                m.d.sync += [a0.eq(a), b0.eq(b), c0.eq(Mux(t < q, q - t, t - q)),
                             neg0.eq(t >= q), rom_one.eq(sidx == q)]
            with m.Else():
                m.d.comb += [rp.addr.eq(b0), rom_addr.eq(c0)]
                m.d.sync += rom_one.eq(c0 == q)
        with m.Else():
            m.d.comb += rp.addr.eq(self.rd_addr)
        m.d.comb += [self.rd_re.eq(rp.data[:16]), self.rd_im.eq(rp.data[16:])]

        # t1: A and sin.
        with m.If(v[0]):
            m.d.sync += [A_re.eq(rp.data[:16]), A_im.eq(rp.data[16:]), S.eq(rom_data),
                         neg1.eq(neg0)]
        # t2: B and cos; A and the addresses move on.
        with m.If(v[1]):
            m.d.sync += [
                B_re.eq(rp.data[:16]), B_im.eq(rp.data[16:]),
                Wr.eq(Mux(neg1, -rom_data.as_signed(), rom_data.as_signed())), Ws.eq(S),
                A2_re.eq(A_re), A2_im.eq(A_im),
                a2.eq(a0), b2.eq(b0),
            ]
        # t3, t4: the products, two multipliers. B * W with W = cos - i sin:
        # re = B.re cos + B.im sin, im = B.im cos - B.re sin.
        m1 = Mux(v[2], B_re, B_im) * Wr
        m2 = Mux(v[2], B_im, B_re) * Ws
        with m.If(v[2] | v[3]):
            m.d.sync += [p1.eq(m1), p2.eq(m2)]
        with m.If(v[3]):
            m.d.sync += [
                P_re.eq((p1 + p2 + (1 << 14)) >> 15),
                A3_re.eq(A2_re), A3_im.eq(A2_im),
                a4.eq(a2), b4.eq(b2),
            ]
        with m.If(v[4]):
            m.d.sync += P_im.eq((p1 - p2 + (1 << 14)) >> 15)

        # t6: A' out, B' kept for t7; halved, rounded.
        def out(x):
            r = Signal(signed(18), name="r")
            m.d.comb += r.eq((x + 1) >> 1)
            return r[:16].as_signed()

        ar = out(A3_re + P_re)
        ai = out(A3_im + P_im)
        br = out(A3_re - P_re)
        bi = out(A3_im - P_im)

        w_re = Signal(signed(16))
        w_im = Signal(signed(16))
        with m.If(v[5]):
            m.d.comb += [wp.addr.eq(a4), w_re.eq(ar), w_im.eq(ai), wp.en.eq(1)]
            m.d.sync += [Bo_re.eq(br), Bo_im.eq(bi), b6.eq(b4)]
        with m.Elif(v[6]):
            m.d.comb += [wp.addr.eq(b6), w_re.eq(Bo_re), w_im.eq(Bo_im), wp.en.eq(1)]
        with m.Elif(~self.busy & self.ld_we):
            m.d.comb += [wp.addr.eq(rev), w_re.eq(self.ld_re), w_im.eq(self.ld_im),
                         wp.en.eq(1)]
        m.d.comb += wp.data.eq(Cat(w_re, w_im))

        # Control.
        m.d.sync += self.done.eq(0)
        with m.FSM():
            with m.State("IDLE"):
                with m.If(self.start):
                    m.d.sync += [self.busy.eq(1), stage.eq(0), self.exp.eq(0), span.eq(1),
                                 tstep.eq(1 << (FFT_MAX_LOG2 - 1))]
                    m.next = "STAGE"
            with m.State("STAGE"):
                m.d.sync += [self.exp.eq(self.exp + 1), bfly.eq(0),
                             issue.eq(1), ph.eq(0), j.eq(0), a.eq(0), t.eq(0)]
                m.next = "RUN"
            with m.State("RUN"):
                m.d.sync += ph.eq(~ph)
                with m.If(ph):
                    m.d.sync += bfly.eq(bfly + 1)
                    with m.If(j + 1 == span):
                        m.d.sync += [j.eq(0), t.eq(0), a.eq(a + 1 + span)]
                    with m.Else():
                        m.d.sync += [j.eq(j + 1), t.eq(t + tstep), a.eq(a + 1)]
                    with m.If(bfly + 1 == half):
                        m.d.sync += issue.eq(0)
                        m.next = "DRAIN"
            with m.State("DRAIN"):
                with m.If((v == 0) & ~issue):
                    with m.If(stage + 1 == self.log2n):
                        m.d.sync += [self.busy.eq(0), self.done.eq(1)]
                        m.next = "IDLE"
                    with m.Else():
                        m.d.sync += [stage.eq(stage + 1), span.eq(span << 1),
                                     tstep.eq(tstep >> 1)]
                        m.next = "STAGE"
        return m
