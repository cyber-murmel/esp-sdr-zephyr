# SPDX-License-Identifier: GPL-3.0-or-later
"""The DSP chain: samples -> window -> FFT -> power -> log2 -> average -> columns -> lines.

    0x03 SAMPLES  f8, data...   f[2:0] format: 0 WORD32 (four bytes, little-endian: Q in bits
                                9:0, I in 19:10, the ESP32's dump word), 1 IQ10P (those 20
                                bits packed, esp_sdr_rx_pack_iq10()), 2 IQ8 (I, Q), 3 IQ16
                                (I, Q little-endian); b6 RESYNC: start a new frame here

Samples go into a ring in SPRAM as s16 pairs. A frame
of N (16..512) is windowed (and halved) into the FFT, the bins' power becomes 256 * log2 in
"L" units, plus 512 per halving and L_OFFSET, and is averaged into a state per bin (fftshifted) in the
log domain: s += F(L - s) from a table the host computes (model.avg_table()). Every FPL frames
(or on EMIT) the bins are reduced to columns (the max of each column's bins, model.columns())
and scaled into a waterfall line and the trace's rows.

Load, unload and the scaling run one after another and share one multiplier; the FFT has two.
A frame of 512 takes about 14000 cycles: some 1 MS/s at 30 MHz. The ring writer takes two
cycles per sample, so samples must come at most every four (SPI at 40 MHz brings IQ8 every 12). Model: model.spectrum_frame(), average(), columns(), scale().
"""

from amaranth.hdl import Cat, Const, Module, Mux, Signal, signed
from amaranth.lib import memory, wiring
from amaranth.lib.wiring import In, Out

from .fft import FFT
from .model import LOG2_FRAC, WF_MAX
from .prims import Spram

__all__ = ["DSP", "OP_SAMPLES", "FMT_WORD32", "FMT_IQ10P", "FMT_IQ8", "FMT_IQ16",
           "F_RESYNC", "RING"]

OP_SAMPLES = 0x03
FMT_WORD32 = 0
FMT_IQ10P = 1
FMT_IQ8 = 2
FMT_IQ16 = 3
F_RESYNC = 1 << 6

RING = 8192             # complex samples in the input ring (one SPRAM)


class DSP(wiring.Component):
    s_data: In(8)
    s_first: In(1)
    s_stb: In(1)
    s_end: In(1)

    on: In(1)
    avg_on: In(1)
    overlap: In(1)
    log2n: In(4)
    fpl: In(8)
    l_offset: In(signed(16))
    c_hi: In(signed(16))
    c_lo: In(signed(16))
    cols: In(9)
    zoom_start: In(10)
    zoom_step: In(16)       # bins per column, 8.8
    wf_lo: In(signed(16))
    wf_gain: In(16)
    tr_top: In(signed(16))
    tr_gain: In(16)
    tr_rows: In(9)
    avg_reset: In(1)
    emit: In(1)

    # Host writes and reads.
    win_addr: In(9)
    win_data: In(16)
    win_we: In(1)
    lut_addr: In(9)
    lut_data: In(16)
    lut_we: In(1)
    colv_addr: In(9)
    colv_data: Out(16)

    # Waterfall line (WFLINE-style stream) and trace rows.
    l_data: Out(8)
    l_first: Out(1)
    l_stb: Out(1)
    l_end: Out(1)
    l_active: Out(1)
    col_addr: Out(9)
    col_data: Out(8)
    col_we: Out(1)
    col_commit: Out(1)

    frames: Out(16)
    overruns: Out(16)
    dropped: Out(16)        # samples the full ring refused
    in_free: Out(16)
    busy: Out(1)

    def elaborate(self, platform):
        m = Module()
        m.submodules.fft = fft = FFT()
        m.submodules.ring = ring = Spram(1)
        m.submodules.state = state = Spram(1)
        m.submodules.win = win = memory.Memory(shape=16, depth=512, init=[])
        m.submodules.lut = lut = memory.Memory(shape=16, depth=512, init=[])
        m.submodules.log2 = log2 = memory.Memory(shape=8, depth=256, init=LOG2_FRAC)
        m.submodules.colv = colv = memory.Memory(shape=16, depth=512, init=[])
        win_w, win_r = win.write_port(), win.read_port()
        lut_w, lut_r = lut.write_port(), lut.read_port()
        log2_r = log2.read_port()
        colv_w, colv_r = colv.write_port(), colv.read_port()
        m.d.comb += [
            win_w.addr.eq(self.win_addr), win_w.data.eq(self.win_data), win_w.en.eq(self.win_we),
            lut_w.addr.eq(self.lut_addr), lut_w.data.eq(self.lut_data), lut_w.en.eq(self.lut_we),
            fft.log2n.eq(self.log2n),
        ]
        n = Signal(11)
        m.d.comb += n.eq(1 << self.log2n)

        # Samples in ----------------------------------------------------------------------
        in_i = Signal(signed(16))
        in_q = Signal(signed(16))
        in_stb = Signal()
        resync = Signal()

        active = Signal()
        fmt = Signal(3)
        sr = Signal(40)         # the latest bytes, newest at the top
        nbytes = Signal(3)      # bytes of the current group (a sample, or two for IQ10P)
        hdr = Signal()          # the format byte is next
        m.d.sync += [in_stb.eq(0), resync.eq(0)]
        with m.If(self.s_stb & self.s_first):
            m.d.sync += [active.eq(self.s_data == OP_SAMPLES), hdr.eq(1), nbytes.eq(0)]
        with m.Elif(self.s_stb & active & hdr):
            m.d.sync += [fmt.eq(self.s_data[:3]), hdr.eq(0), resync.eq(self.s_data[6])]
        with m.Elif(self.s_stb & active):
            new = Cat(sr[8:], self.s_data)
            m.d.sync += [sr.eq(new), nbytes.eq(nbytes + 1)]

            def word(bits):
                """A dump word (Q in 9:0, I in 19:10) from 20 bits of the stream."""
                return [in_i.eq(bits[10:20].as_signed() << 6),
                        in_q.eq(bits[:10].as_signed() << 6),
                        in_stb.eq(1)]
            with m.Switch(fmt):
                with m.Case(FMT_WORD32):
                    with m.If(nbytes == 3):
                        m.d.sync += [*word(new[8:28]), nbytes.eq(0)]
                with m.Case(FMT_IQ10P):
                    with m.If(nbytes == 2):
                        m.d.sync += word(new[16:36])
                    with m.If(nbytes == 4):
                        m.d.sync += [*word(new[20:40]), nbytes.eq(0)]
                with m.Case(FMT_IQ8):
                    with m.If(nbytes == 1):
                        m.d.sync += [in_i.eq(new[24:32].as_signed() << 8),
                                     in_q.eq(new[32:40].as_signed() << 8),
                                     in_stb.eq(1), nbytes.eq(0)]
                with m.Default():
                    with m.If(nbytes == 3):
                        m.d.sync += [in_i.eq(new[8:24].as_signed()),
                                     in_q.eq(new[24:40].as_signed()),
                                     in_stb.eq(1), nbytes.eq(0)]
        with m.If(self.s_end):
            m.d.sync += active.eq(0)

        # The ring: writes (I then Q) win the port over reads.
        wp = Signal(13)
        fp = Signal(13)
        fill = Signal(13)
        ring_full = Signal()
        w_step = Signal(2)      # 1: write I, 2: write Q (in_i, in_q hold until written)
        rd_addr = Signal(14)
        rd_req = Signal()
        rd_ack = Signal()
        # A full ring drops samples (and counts them): the writer must not lap the frame
        # being read. fill is a cycle late, hence the margin.
        with m.If(in_stb & ring_full):
            m.d.sync += self.dropped.eq(self.dropped + 1)
        with m.Elif(in_stb):
            m.d.sync += w_step.eq(1)
        with m.If(w_step == 1):
            m.d.comb += [ring.addr.eq(Cat(0, wp)), ring.w_data.eq(in_i), ring.we.eq(1)]
            m.d.sync += w_step.eq(2)
        with m.Elif(w_step == 2):
            m.d.comb += [ring.addr.eq(Cat(1, wp)), ring.w_data.eq(in_q), ring.we.eq(1)]
            m.d.sync += [w_step.eq(0), wp.eq(wp + 1)]
        with m.Else():
            m.d.comb += [ring.addr.eq(rd_addr), rd_ack.eq(rd_req)]

        # The ring's fill and what the scheduler asks of it, a cycle late (timing).
        frame_ready = Signal()
        near_full = Signal()
        m.d.sync += [
            fill.eq(wp - fp),
            frame_ready.eq((wp - fp)[:13] >= n),
            near_full.eq((wp - fp)[:13] > RING - 64),
            ring_full.eq((wp - fp)[:13] >= RING - 4),
        ]
        m.d.comb += self.in_free.eq(RING - 1 - fill)

        # Shared multiplier.
        mul_a = Signal(signed(16))
        mul_b = Signal(signed(16))
        mul_p = Signal(signed(32))
        m.d.sync += mul_p.eq(mul_a * mul_b)

        # Main sequence --------------------------------------------------------------------
        k = Signal(11)
        fresh = Signal()
        x_i = Signal(signed(16))
        x_q = Signal(signed(16))
        w = Signal(16)
        line_frames = Signal(8)
        emit_pending = Signal()
        first = Signal(init=1)  # the average starts from this frame
        exp = Signal(5)
        psum = Signal(32)
        e = Signal(5)
        nstep = Signal(3)
        ks = Signal(10)
        s_old = Signal(signed(16))
        lval = Signal(signed(17))
        d = Signal(signed(18))
        c = Signal(9)
        pos = Signal(18)
        b = Signal(11)
        b_end = Signal(11)
        b_next = Signal(11)
        vmax = Signal(signed(16))

        with m.If(self.emit):
            m.d.sync += emit_pending.eq(1)
        with m.If(self.avg_reset):
            m.d.sync += first.eq(1)
        # After fp moves, frame_ready and near_full are a cycle stale.
        settle = Signal()
        m.d.sync += settle.eq(0)
        with m.If(resync):
            m.d.sync += [fp.eq(wp), settle.eq(1)]

        m.d.sync += [fft.start.eq(0), fft.ld_we.eq(0), self.l_stb.eq(0), self.l_first.eq(0),
                     self.l_end.eq(0), self.col_we.eq(0), self.col_commit.eq(0)]
        m.d.comb += [
            state.mask.eq(0b11),
            self.busy.eq(0),
            colv_r.addr.eq(self.colv_addr),
            self.colv_data.eq(colv_r.data),
        ]

        def sat16(x):
            """A 17-bit difference saturated to s16 by its top bits (no comparators)."""
            x17 = Signal(signed(17), name="sat_in")
            m.d.comb += x17.eq(x)
            return Mux(x17[16] == x17[15], x17[:16], Mux(x17[16], -32768, 32767))

        def clamp_u(x, top, bits):
            """x (signed) clamped to 0..top, where top < 2**bits."""
            xs = Signal(signed(len(x)), name="clamp_in")
            m.d.comb += xs.eq(x)
            over = xs[bits:-1] != 0
            return Mux(xs[-1], 0, Mux(over | (xs[:bits] > top), top, xs[:bits]))

        with m.FSM():
            with m.State("IDLE"):
                with m.If(near_full):
                    # The writer is about to lap the frame: drop to the newest samples.
                    m.d.sync += [fp.eq(wp - n), self.overruns.eq(self.overruns + 1),
                                 settle.eq(1)]
                with m.Elif(self.on & frame_ready & (resync == 0) & ~settle):
                    m.d.sync += k.eq(0)
                    m.next = "LOAD_I"

            # Load: I, Q, window, two products, into the FFT bit-reversed.
            with m.State("LOAD_I"):
                m.d.comb += [rd_addr.eq(Cat(0, fp + k)), rd_req.eq(1),
                             win_r.addr.eq(Mux(k < (n >> 1), k, n - 1 - k)), self.busy.eq(1)]
                with m.If(rd_ack):
                    m.d.sync += fresh.eq(1)
                    m.next = "LOAD_Q"
            with m.State("LOAD_Q"):
                # I and the window arrive the cycle after their read; the Q read may wait
                # for the writer.
                m.d.comb += [rd_addr.eq(Cat(1, fp + k)), rd_req.eq(1), self.busy.eq(1)]
                m.d.sync += fresh.eq(0)
                with m.If(fresh):
                    m.d.sync += [x_i.eq(ring.r_data), w.eq(win_r.data)]
                with m.If(rd_ack):
                    m.next = "LOAD_MI"
            with m.State("LOAD_MI"):
                m.d.comb += [mul_a.eq(x_i), mul_b.eq(w), self.busy.eq(1)]
                m.d.sync += x_q.eq(ring.r_data)
                m.next = "LOAD_MQ"
            with m.State("LOAD_MQ"):
                m.d.comb += [mul_a.eq(x_q), mul_b.eq(w), self.busy.eq(1)]
                # Window and a half: magnitudes stay below 2**15 through the FFT.
                m.d.sync += [fft.ld_re.eq((mul_p + (1 << 15)) >> 16)]
                m.next = "LOAD_W"
            with m.State("LOAD_W"):
                m.d.comb += self.busy.eq(1)
                m.d.sync += [fft.ld_im.eq((mul_p + (1 << 15)) >> 16), fft.ld_addr.eq(k),
                             fft.ld_we.eq(1), k.eq(k + 1)]
                with m.If(k + 1 == n):
                    m.d.sync += [fp.eq(fp + Mux(self.overlap, n >> 1, n)), fft.start.eq(1),
                                 settle.eq(1)]
                    m.next = "FFT"
                with m.Else():
                    m.next = "LOAD_I"

            with m.State("FFT"):
                m.d.comb += self.busy.eq(1)
                with m.If(fft.done):
                    m.d.sync += [k.eq(0), exp.eq(fft.exp + 1)]
                    m.next = "U_READ"

            # Unload: power, log2, average, per bin.
            with m.State("U_READ"):
                m.d.comb += [fft.rd_addr.eq(k), self.busy.eq(1)]
                m.d.sync += ks.eq(k ^ (n >> 1))
                m.next = "U_RE"
            with m.State("U_RE"):
                m.d.comb += [mul_a.eq(fft.rd_re), mul_b.eq(fft.rd_re), self.busy.eq(1),
                             state.addr.eq(ks)]
                m.d.sync += x_q.eq(fft.rd_im)
                m.next = "U_IM"
            with m.State("U_IM"):
                m.d.comb += [mul_a.eq(x_q), mul_b.eq(x_q), self.busy.eq(1)]
                m.d.sync += [psum.eq(mul_p), s_old.eq(state.r_data)]
                m.next = "U_SUM"
            with m.State("U_SUM"):
                m.d.comb += self.busy.eq(1)
                m.d.sync += [psum.eq(psum + mul_p), e.eq(31), nstep.eq(0)]
                m.next = "U_E"
            with m.State("U_E"):
                # Normalise: shift the leading one to bit 31 in five halving steps, e counting
                # down from 31; then the 8 bits below it address the table.
                m.d.comb += self.busy.eq(1)
                amount = Mux(nstep == 0, 16, Mux(nstep == 1, 8, Mux(nstep == 2, 4,
                             Mux(nstep == 3, 2, 1))))
                top = Mux(nstep == 0, psum[16:] == 0, Mux(nstep == 1, psum[24:] == 0,
                          Mux(nstep == 2, psum[28:] == 0, Mux(nstep == 3, psum[30:] == 0,
                              ~psum[31]))))
                with m.If(top & (psum != 0)):
                    m.d.sync += [psum.eq(psum << amount), e.eq(e - amount)]
                m.d.sync += nstep.eq(nstep + 1)
                with m.If(nstep == 4):
                    m.d.sync += nstep.eq(0)
                    m.next = "U_M"
            with m.State("U_M"):
                m.d.comb += [self.busy.eq(1), log2_r.addr.eq(psum[23:31])]
                m.next = "U_L"
            with m.State("U_L"):
                m.d.comb += self.busy.eq(1)
                lv = (Mux(psum == 0, 0, e) << 8) + log2_r.data + (exp << 9) + self.l_offset
                m.d.sync += [lval.eq(lv), d.eq(lv - s_old)]
                m.next = "U_LUT"
            with m.State("U_LUT"):
                m.d.comb += [self.busy.eq(1), lut_r.addr.eq((d >> 3) + 256)]
                m.next = "U_AVG"
            with m.State("U_AVG"):
                new = Signal(signed(18))
                with m.If(first | ~self.avg_on):
                    m.d.comb += new.eq(lval)
                # |d| beyond the table: the bits above 11 are not all the sign.
                beyond = d[11:] != d[17].replicate(7)
                with m.Elif(beyond & ~d[17]):
                    m.d.comb += new.eq(lval + self.c_hi)
                with m.Elif(beyond & d[17] & (d != -2048)):
                    m.d.comb += new.eq(s_old + self.c_lo)
                with m.Else():
                    m.d.comb += new.eq(s_old + lut_r.data.as_signed())
                m.d.comb += [self.busy.eq(1), state.addr.eq(ks), state.w_data.eq(new[:16]),
                             state.we.eq(1)]
                m.d.sync += k.eq(k + 1)
                with m.If(k + 1 == n):
                    m.d.sync += [self.frames.eq(self.frames + 1), first.eq(0),
                                 line_frames.eq(line_frames + 1)]
                    m.next = "LINE?"
                with m.Else():
                    m.next = "U_READ"

            with m.State("LINE?"):
                with m.If(emit_pending | ((self.fpl != 0) & (line_frames >= self.fpl))):
                    m.d.sync += [emit_pending.eq(0), line_frames.eq(0), c.eq(0), pos.eq(0),
                                 b.eq(self.zoom_start), self.l_active.eq(1),
                                 self.l_data.eq(0x05), self.l_first.eq(1), self.l_stb.eq(1)]
                    m.next = "C_START"
                with m.Else():
                    m.next = "IDLE"

            # Columns: the max of each column's bins.
            with m.State("C_START"):
                m.d.comb += self.busy.eq(1)
                nxt = pos + self.zoom_step
                end = self.zoom_start + (nxt >> 8)
                m.d.sync += [pos.eq(nxt), b_end.eq(Mux(end > b, end, b + 1)), b_next.eq(end),
                             vmax.eq(-32768)]
                m.next = "C_READ"
            with m.State("C_READ"):
                m.d.comb += [self.busy.eq(1), state.addr.eq(b)]
                m.next = "C_MAX"
            with m.State("C_MAX"):
                m.d.comb += self.busy.eq(1)
                val = state.r_data.as_signed()
                newmax = Mux((b < n) & (val > vmax), val, vmax)
                m.d.sync += [vmax.eq(newmax), b.eq(b + 1)]
                with m.If((b + 1 >= b_end) | (b + 1 >= n)):
                    m.d.comb += [colv_w.addr.eq(c), colv_w.data.eq(newmax), colv_w.en.eq(1)]
                    m.d.sync += b.eq(b_next)
                    m.next = "S_MWF"
                with m.Else():
                    m.next = "C_READ"

            # Scale the column: waterfall index and trace row.
            with m.State("S_MWF"):
                m.d.comb += [self.busy.eq(1), mul_a.eq(sat16(vmax - self.wf_lo)),
                             mul_b.eq(self.wf_gain)]
                m.next = "S_MTR"
            with m.State("S_MTR"):
                m.d.comb += [self.busy.eq(1), mul_a.eq(sat16(self.tr_top - vmax)),
                             mul_b.eq(self.tr_gain)]
                m.d.sync += [self.l_data.eq(clamp_u(mul_p[8:], WF_MAX, 8)), self.l_stb.eq(1)]
                m.next = "S_TR"
            with m.State("S_TR"):
                m.d.comb += self.busy.eq(1)
                m.d.sync += [self.col_addr.eq(c),
                             self.col_data.eq(clamp_u(mul_p[8:], self.tr_rows - 1, 8)),
                             self.col_we.eq(1), c.eq(c + 1)]
                with m.If(c + 1 == self.cols):
                    m.next = "S_END"
                with m.Else():
                    m.next = "C_START"
            with m.State("S_END"):
                m.d.sync += [self.l_end.eq(1), self.l_active.eq(0), self.col_commit.eq(1)]
                m.next = "IDLE"
        return m
