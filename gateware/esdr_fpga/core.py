# SPDX-License-Identifier: GPL-3.0-or-later
"""Everything behind the pins, in sync and the SPI domains, so simulation can drive it."""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

from .bus import Decoder
from .dbi import DBIPassthrough
from .lcd import LCDPhy
from .mem import BusRAM
from .proto import Protocol
from .regs import Registers

__all__ = ["Core", "SCRATCH_RAM"]

SCRATCH_RAM = 0x1000


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

    def __init__(self, build=0):
        self.build = build
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.proto = proto = Protocol()
        m.submodules.regs = regs = Registers({"BUILD": self.build})
        m.submodules.ram = ram = BusRAM(256)
        m.submodules.dec = dec = Decoder([(0x0000, 256), (SCRATCH_RAM, 256)])
        wiring.connect(m, proto.bus, dec.bus)
        wiring.connect(m, dec.w0, regs.bus)
        wiring.connect(m, dec.w1, ram.bus)
        m.d.comb += [
            proto.spi.sck.eq(self.sck),
            proto.spi.copi.eq(self.copi),
            proto.spi.cs.eq(self.cs),
            self.cipo.eq(proto.spi.cipo),
        ]

        # Panel.
        m.submodules.dbi = dbi = DBIPassthrough()
        m.submodules.phy = phy = LCDPhy()
        m.d.comb += [
            dbi.s_data.eq(proto.s_data),
            dbi.s_first.eq(proto.s_first),
            dbi.s_stb.eq(proto.s_stb),
            dbi.s_end.eq(proto.s_end),
            phy.mode_in.eq(self.lcd_mode),
            phy.fmark_in.eq(self.lcd_fmark),
            self.lcd_d.eq(phy.d),
            self.lcd_rs.eq(phy.rs),
            self.lcd_wr.eq(phy.wr),
            self.lcd_oe.eq(phy.oe),
        ]
        wiring.connect(m, dbi.out, phy.data)

        te_cnt = Signal(16)
        with m.If(phy.te):
            m.d.sync += te_cnt.eq(te_cnt + 1)
        m.d.comb += [
            regs.status["STATUS"].eq(phy.mode | (dbi.pending << 1)),
            regs.status["TE_CNT"].eq(te_cnt),
            regs.status["DBI_BYTES"].eq(phy.count),
            regs.status["MADCTL_HOST"].eq(dbi.madctl),
        ]

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
