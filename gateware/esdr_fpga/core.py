# SPDX-License-Identifier: GPL-3.0-or-later
"""Everything behind the pins, in sync and the SPI domains, so simulation can drive it."""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

from .bus import Decoder
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
    # Debug: frames seen, last opcode and byte count.
    dbg_frames: Out(16)
    dbg_last: Out(16)

    def __init__(self, build=0):
        self.build = build
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.proto = proto = Protocol()
        m.submodules.regs = regs = Registers(build=self.build)
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
        nbytes = Signal(8)
        with m.If(proto.spi.start):
            m.d.sync += [self.dbg_frames.eq(self.dbg_frames + 1), nbytes.eq(0)]
        with m.Elif(proto.spi.rx_stb):
            m.d.sync += nbytes.eq(nbytes + 1)
            with m.If(nbytes == 0):
                m.d.sync += self.dbg_last[8:].eq(proto.spi.rx_data)
        with m.If(proto.spi.end):
            m.d.sync += self.dbg_last[:8].eq(nbytes)
        return m
