# SPDX-License-Identifier: GPL-3.0-or-later
"""The host's panel traffic, passed through to the 8080 bus.

    0x04 DBI  f8, [c8], data...   f bit 0 HOLD: the host keeps the bus after this frame (a
                                  memory write continues in the next one); bit 1 NOCMD: no
                                  command byte, the data continues the open memory write

Command bytes go out with RS low, data with RS high, through a FIFO (the SPI delivers at most
5 MB/s, the bus takes 15). The last MADCTL (0x36) data byte is kept for the renderer, which
writes with its own and puts the host's back.
"""

from amaranth.hdl import Module, Mux, Signal
from amaranth.lib import fifo, stream, wiring
from amaranth.lib.wiring import In, Out

from .lcd import BYTE_RS

__all__ = ["DBIPassthrough", "OP_DBI", "F_HOLD", "F_NOCMD", "MADCTL"]

OP_DBI = 0x04
F_HOLD = 1 << 0
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
    hold: Out(1)        # the host holds the bus between frames
    overflow: Out(1)    # sticky: a byte found the FIFO full

    def __init__(self, depth=512, hold_timeout=1 << 20):
        self.depth = depth
        # A HOLD that no frame ends lapses after this many cycles (35 ms at 30 MHz).
        self.hold_timeout = hold_timeout
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.fifo = q = fifo.SyncFIFOBuffered(width=9, depth=self.depth)

        active = Signal()       # inside a DBI frame
        state = Signal(2)       # 0 flags, 1 command, 2 data
        cmd = Signal(8)
        first_data = Signal()   # the next data byte is the first after a command
        hold_flag = Signal()
        idle = Signal(range(self.hold_timeout + 1))

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
                    m.d.sync += [
                        state.eq(Mux((self.s_data & F_NOCMD) != 0, 2, 1)),
                        hold_flag.eq(self.s_data[0]),
                        self.hold.eq(1),
                    ]
                with m.Case(1):
                    m.d.comb += [q.w_data.eq(self.s_data), q.w_en.eq(1)]
                    m.d.sync += [cmd.eq(self.s_data), first_data.eq(1), state.eq(2)]
                with m.Case(2):
                    m.d.comb += [q.w_data.eq(BYTE_RS | self.s_data), q.w_en.eq(1)]
                    m.d.sync += first_data.eq(0)
                    with m.If(first_data & (cmd == MADCTL)):
                        m.d.sync += self.madctl.eq(self.s_data)
        with m.If(q.w_en & ~q.w_rdy):
            m.d.sync += self.overflow.eq(1)

        with m.If(self.s_end & active):
            m.d.sync += [active.eq(0), self.hold.eq(hold_flag), idle.eq(0)]
        with m.Elif(self.s_end):
            m.d.sync += active.eq(0)
        with m.Elif(self.hold & ~active):
            with m.If(idle == self.hold_timeout):
                m.d.sync += self.hold.eq(0)
            with m.Else():
                m.d.sync += idle.eq(idle + 1)
        return m

