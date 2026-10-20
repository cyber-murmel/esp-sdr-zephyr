# SPDX-License-Identifier: GPL-3.0-or-later
"""Everything behind the pins, in sync and the SPI domains, so simulation can drive it."""

from amaranth.hdl import Cat, Module, Mux, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

from . import bus
from .bus import Decoder
from .dbi import DBIPassthrough
from .dsp import DSP
from .lcd import LCDPhy
from .mem import BusRAM
from .model import FFT_MAX_LOG2
from .proto import Protocol
from .regs import Registers
from .render import Renderer
from .waterfall import Waterfall

__all__ = ["Core", "SCRATCH_RAM", "PALETTE", "COLUMNS", "WINDOW", "AVG_LUT", "COLV",
           "CAPS_DSP"]

SCRATCH_RAM = 0x0100
PALETTE = 0x1000        # 256 RGB565 entries, write-only
WINDOW = 0x1200         # window, Q15, the first N / 2 coefficients, write-only
AVG_LUT = 0x1400        # average step per difference (model.avg_table()), write-only
COLV = 0x1600           # DSP: L per column of the last line, read-only
COLUMNS = 0x1800        # spectrum trace row per column (back bank), write-only

# CAPS: b0 DSP, bits 11:8 the largest log2 N.
CAPS_DSP = 1 << 0


class _WritePort(wiring.Component):
    """A write-only bus window: address, data and a strobe."""

    def __init__(self, depth):
        self.depth = depth
        super().__init__({
            "bus": In(bus.Signature((depth - 1).bit_length())),
            "addr": Out((depth - 1).bit_length()),
            "data": Out(16),
            "we": Out(1),
        })

    def elaborate(self, platform):
        m = Module()
        m.d.comb += [
            self.addr.eq(self.bus.addr),
            self.data.eq(self.bus.w_data),
            self.we.eq(self.bus.w_stb),
            self.bus.r_data.eq(0),
        ]
        return m


class _ReadPort(wiring.Component):
    """A read-only bus window onto a memory read port (data the cycle after)."""

    def __init__(self, depth):
        self.depth = depth
        super().__init__({
            "bus": In(bus.Signature((depth - 1).bit_length())),
            "addr": Out((depth - 1).bit_length()),
            "data": In(16),
        })

    def elaborate(self, platform):
        m = Module()
        m.d.comb += [self.addr.eq(self.bus.addr), self.bus.r_data.eq(self.data)]
        return m


class Core(wiring.Component):
    sck: In(1)
    copi: In(1)
    cs: In(1)
    cipo: Out(1)

    # Panel bus: wr active high (the pin is inverted); drive while lcd_oe.
    lcd_d: Out(8)
    lcd_rs: Out(1)
    lcd_wr: Out(1)
    lcd_oe: Out(1)
    lcd_mode: In(1)
    lcd_fmark: In(1)

    # Debug: frames seen; the last frame's first two bytes and its length.
    dbg_frames: Out(16)
    dbg_last: Out(16)
    dbg_len: Out(16)

    def __init__(self, build=0, hold_timeout=1 << 20, debug=False):
        self.build = build
        self.hold_timeout = hold_timeout
        self.debug = debug
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.proto = proto = Protocol()
        caps = CAPS_DSP | (FFT_MAX_LOG2 << 8)
        m.submodules.regs = regs = Registers({"BUILD": self.build, "CAPS": caps})
        m.submodules.ram = ram = BusRAM(256)
        m.submodules.pal_port = pal_port = _WritePort(256)
        m.submodules.col_port = col_port = _WritePort(512)
        m.submodules.win_port = win_port = _WritePort(512)
        m.submodules.lut_port = lut_port = _WritePort(512)
        m.submodules.colv_port = colv_port = _ReadPort(512)
        m.submodules.dec = dec = Decoder([(0x0000, 256), (SCRATCH_RAM, 256), (PALETTE, 256),
                                          (COLUMNS, 512), (WINDOW, 512), (AVG_LUT, 512),
                                          (COLV, 512)])
        wiring.connect(m, proto.bus, dec.bus)
        wiring.connect(m, dec.w0, regs.bus)
        wiring.connect(m, dec.w1, ram.bus)
        wiring.connect(m, dec.w2, pal_port.bus)
        wiring.connect(m, dec.w3, col_port.bus)
        wiring.connect(m, dec.w4, win_port.bus)
        wiring.connect(m, dec.w5, lut_port.bus)
        wiring.connect(m, dec.w6, colv_port.bus)
        m.d.comb += [
            proto.spi.sck.eq(self.sck),
            proto.spi.copi.eq(self.copi),
            proto.spi.cs.eq(self.cs),
            self.cipo.eq(proto.spi.cipo),
        ]
        # The byte stream of the other opcodes, registered: it fans out across the chip.
        stream = (Signal(8, name="st_data"), Signal(name="st_first"), Signal(name="st_stb"),
                  Signal(name="st_end"))
        m.d.sync += [stream[0].eq(proto.s_data), stream[1].eq(proto.s_first),
                     stream[2].eq(proto.s_stb), stream[3].eq(proto.s_end)]

        # Panel.
        m.submodules.dbi = dbi = DBIPassthrough(hold_timeout=self.hold_timeout)
        m.submodules.phy = phy = LCDPhy()
        m.submodules.wf = wf = Waterfall()
        m.submodules.render = rd = Renderer()
        m.submodules.dsp = dsp = DSP()
        for blk in (dbi, dsp):
            m.d.comb += [blk.s_data.eq(stream[0]), blk.s_first.eq(stream[1]),
                         blk.s_stb.eq(stream[2]), blk.s_end.eq(stream[3])]
        # The waterfall takes WFLINE frames, or the DSP's lines while it writes one.
        with m.If(dsp.l_active | dsp.l_end):
            m.d.comb += [wf.s_data.eq(dsp.l_data), wf.s_first.eq(dsp.l_first),
                         wf.s_stb.eq(dsp.l_stb), wf.s_end.eq(dsp.l_end)]
        with m.Else():
            m.d.comb += [wf.s_data.eq(stream[0]), wf.s_first.eq(stream[1]),
                         wf.s_stb.eq(stream[2]), wf.s_end.eq(stream[3])]
        m.d.comb += [
            phy.mode_in.eq(self.lcd_mode),
            phy.fmark_in.eq(self.lcd_fmark),
            self.lcd_d.eq(phy.d),
            self.lcd_rs.eq(phy.rs),
            self.lcd_wr.eq(phy.wr),
            self.lcd_oe.eq(phy.oe),
        ]

        # The panel bus: the renderer's while it owns it, the host's otherwise.
        with m.If(rd.own):
            m.d.comb += [phy.data.payload.eq(rd.out.payload), phy.data.valid.eq(rd.out.valid),
                         rd.out.ready.eq(phy.data.ready)]
        with m.Else():
            m.d.comb += [phy.data.payload.eq(dbi.out.payload),
                         phy.data.valid.eq(dbi.out.valid), dbi.out.ready.eq(phy.data.ready)]

        c = regs.control
        sp_flags = c["SP_FLAGS"]
        m.d.comb += [
            rd.host_busy.eq(dbi.pending | dbi.hold),
            rd.te.eq(phy.te),
            rd.enable.eq(c["CTRL"][0] & phy.mode),
            rd.te_sync.eq(c["CTRL"][1]),
            rd.madctl_fpga.eq(c["MADCTL_FPGA"]),
            rd.madctl_host.eq(dbi.madctl),
            rd.dirty_set.eq((regs.strobe["DIRTY"][0][:2] & regs.strobe["DIRTY"][1].replicate(2))
                            | (wf.line_stb & c["CTRL"][2])),
            rd.wf_on.eq(c["WF_FLAGS"][0]),
            rd.wf_page0.eq(c["WF_PAGE0"]),
            rd.wf_col0.eq(c["WF_COL0"]),
            rd.wf_w.eq(c["WF_W"]),
            rd.wf_h.eq(c["WF_H"]),
            rd.wf_ring.eq(c["WF_RING"]),
            rd.wf_head.eq(wf.head),
            rd.sp_page0.eq(c["SP_PAGE0"]),
            rd.sp_col0.eq(c["SP_COL0"]),
            rd.sp_w.eq(c["SP_W"]),
            rd.sp_h.eq(c["SP_H"]),
            rd.sp_flags.eq(sp_flags),
            wf.ring.eq(c["WF_RING"]),
            wf.r_addr.eq(rd.wf_addr),
            wf.r_req.eq(rd.wf_req),
            rd.wf_ack.eq(wf.r_ack),
            rd.wf_data.eq(wf.r_data),
            rd.pal_addr.eq(pal_port.addr),
            rd.pal_data.eq(pal_port.data),
            rd.pal_we.eq(pal_port.we),
            rd.col_addr.eq(Mux(dsp.col_we, dsp.col_addr, col_port.addr)),
            rd.col_data.eq(Mux(dsp.col_we, dsp.col_data, col_port.data)),
            rd.col_we.eq(dsp.col_we | col_port.we),
            rd.col_commit.eq(regs.strobe["COL_COMMIT"][1] | dsp.col_commit),
        ]

        dc = c["DSP_CTRL"]
        cfg = c["FFT_CFG"]
        m.d.comb += [
            dsp.on.eq(dc[0]),
            dsp.avg_on.eq(dc[1]),
            dsp.overlap.eq(dc[2]),
            dsp.log2n.eq(cfg[:4]),
            dsp.fpl.eq(cfg[8:]),
            dsp.l_offset.eq(c["L_OFFSET"]),
            dsp.c_hi.eq(c["AVG_C_HI"]),
            dsp.c_lo.eq(c["AVG_C_LO"]),
            dsp.cols.eq(c["COLS"]),
            dsp.zoom_start.eq(c["ZOOM_START"]),
            dsp.zoom_step.eq(c["ZOOM_STEP"]),
            dsp.wf_lo.eq(c["WF_LO"]),
            dsp.wf_gain.eq(c["WF_GAIN"]),
            dsp.tr_top.eq(c["TR_TOP"]),
            dsp.tr_gain.eq(c["TR_GAIN"]),
            dsp.tr_rows.eq(c["SP_H"]),
            dsp.avg_reset.eq(regs.strobe["AVG_RESET"][1]),
            dsp.emit.eq(regs.strobe["EMIT"][1]),
            dsp.win_addr.eq(win_port.addr),
            dsp.win_data.eq(win_port.data),
            dsp.win_we.eq(win_port.we),
            dsp.lut_addr.eq(lut_port.addr),
            dsp.lut_data.eq(lut_port.data),
            dsp.lut_we.eq(lut_port.we),
            dsp.colv_addr.eq(colv_port.addr),
            colv_port.data.eq(dsp.colv_data),
        ]

        te_cnt = Signal(16)
        with m.If(phy.te):
            m.d.sync += te_cnt.eq(te_cnt + 1)
        m.d.comb += [
            regs.status["STATUS"].eq(phy.mode | (dbi.pending << 1) | (dbi.hold << 2)
                                     | (rd.own << 3) | (dbi.overflow << 7)),
            regs.status["TE_CNT"].eq(te_cnt),
            regs.status["DBI_BYTES"].eq(phy.count),
            regs.status["MADCTL_HOST"].eq(dbi.madctl),
            regs.status["LINES"].eq(wf.lines),
            regs.status["WF_HEAD"].eq(wf.head),
            regs.status["RENDERS"].eq(rd.renders),
            regs.status["R_TIME"].eq(rd.busy_cycles[8:]),
            regs.status["IN_FREE"].eq(dsp.in_free),
            regs.status["FRAMES"].eq(dsp.frames),
            regs.status["OVERRUNS"].eq(dsp.overruns),
            regs.status["DROPPED"].eq(dsp.dropped),
        ]

        if not self.debug:
            return m
        nbytes = Signal(16)
        with m.If(proto.spi.start):
            m.d.sync += [self.dbg_frames.eq(self.dbg_frames + 1), nbytes.eq(0)]
        with m.Elif(proto.spi.rx_stb):
            m.d.sync += nbytes.eq(nbytes + 1)
            with m.If(nbytes == 0):
                m.d.sync += self.dbg_last[8:].eq(proto.spi.rx_data)
            with m.If(nbytes == 1):
                m.d.sync += self.dbg_last[:8].eq(proto.spi.rx_data)
        with m.If(proto.spi.end):
            m.d.sync += self.dbg_len.eq(nbytes)
        return m
