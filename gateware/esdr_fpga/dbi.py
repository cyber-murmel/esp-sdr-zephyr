# SPDX-License-Identifier: GPL-3.0-or-later
"""The host's panel traffic, passed through to the 8080 bus.

    0x04 DBI  f8, [c8], data...   f bit 1 NOCMD: no command byte, the data continues the
                                  memory write the previous frame opened

Command bytes go out with RS low, data with RS high, through a FIFO (the SPI delivers at most
5 MB/s, the bus takes 15). The last MADCTL (0x36) data byte is kept for the renderer, which
writes with its own and puts the host's back.
"""

from amaranth.hdl import Module, Mux, Signal
from amaranth.lib import fifo, stream, wiring
from amaranth.lib.wiring import In, Out

from .lcd import BYTE_RS

__all__ = ["DBIPassthrough", "OP_DBI", "F_NOCMD", "MADCTL"]

OP_DBI = 0x04
F_NOCMD = 1 << 1
MADCTL = 0x36


class DBIPassthrough(wiring.Component):
    s_data: In(8)
    s_first: In(1)
    s_stb: In(1)
    s_end: In(1)

    out: Out(stream.Signature(9))
    madctl: Out(8)
    pending: Out(1)     # bytes still in the FIFO

    def __init__(self, depth=512):
        self.depth = depth
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.fifo = q = fifo.SyncFIFOBuffered(width=9, depth=self.depth)

        active = Signal()       # inside a DBI frame
        state = Signal(2)       # 0 flags, 1 command, 2 data
        cmd = Signal(8)
        first_data = Signal()   # the next data byte is the first after a command

        m.d.comb += [
            self.out.payload.eq(q.r_data),
            self.out.valid.eq(q.r_rdy),
            q.r_en.eq(self.out.ready),
            self.pending.eq(q.r_rdy),
        ]

        with m.If(self.s_stb & self.s_first):
            m.d.sync += [active.eq(self.s_data == OP_DBI), state.eq(0)]
        with m.Elif(self.s_stb & active):
            with m.Switch(state):
                with m.Case(0):
                    m.d.sync += state.eq(Mux((self.s_data & F_NOCMD) != 0, 2, 1))
                with m.Case(1):
                    m.d.comb += [q.w_data.eq(self.s_data), q.w_en.eq(1)]
                    m.d.sync += [cmd.eq(self.s_data), first_data.eq(1), state.eq(2)]
                with m.Case(2):
                    m.d.comb += [q.w_data.eq(BYTE_RS | self.s_data), q.w_en.eq(1)]
                    m.d.sync += first_data.eq(0)
                    with m.If(first_data & (cmd == MADCTL)):
                        m.d.sync += self.madctl.eq(self.s_data)
        with m.If(self.s_end):
            m.d.sync += active.eq(0)
        return m

