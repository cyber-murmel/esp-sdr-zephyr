# SPDX-License-Identifier: GPL-3.0-or-later
"""The waterfall: a ring of lines of 8-bit palette indices in SPRAM, column-major.

Byte address x * ring + slot holds column x of the line in `slot`; the renderer walks one
column of consecutive slots per panel row. A new line goes into slot head + 1, which is
never on screen, and head moves when the line is complete.

    0x05 WFLINE  index bytes for x = 0, 1, ...   (software lines; the DSP writes its own)
"""

from amaranth.hdl import Module, Mux, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

from .prims import Spram

__all__ = ["Waterfall", "OP_WFLINE"]

OP_WFLINE = 0x05


class Waterfall(wiring.Component):
    # Line bytes, from the protocol's stream or the DSP.
    s_data: In(8)
    s_first: In(1)
    s_stb: In(1)
    s_end: In(1)

    ring: In(16)        # lines in the ring (ring * width <= 65536)

    # Renderer read port: request with address, data the cycle after the grant.
    r_addr: In(16)      # byte address
    r_req: In(1)
    r_ack: Out(1)
    r_data: Out(8)

    head: Out(16)
    lines: Out(16)      # lines written
    line_stb: Out(1)    # a line is complete

    def __init__(self, banks=2):
        self.banks = banks
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.ram = ram = Spram(self.banks)

        active = Signal()
        slot = Signal(16)       # slot of the line being written
        base = Signal(16)       # x * ring
        x = Signal(16)
        w_pending = Signal()
        w_addr = Signal(16)
        w_byte = Signal(8)
        r_low = Signal()

        with m.If(self.s_stb & self.s_first):
            m.d.sync += [
                active.eq(self.s_data == OP_WFLINE),
                slot.eq(Mux(self.head + 1 == self.ring, 0, self.head + 1)),
                base.eq(0),
                x.eq(0),
            ]
        with m.Elif(self.s_stb & active):
            m.d.sync += [
                w_pending.eq(1),
                w_addr.eq(base + slot),
                w_byte.eq(self.s_data),
                base.eq(base + self.ring),
                x.eq(x + 1),
            ]

        m.d.sync += self.line_stb.eq(0)
        with m.If(self.s_end & active):
            m.d.sync += active.eq(0)
            with m.If(x != 0):
                m.d.sync += [
                    self.head.eq(slot),
                    self.lines.eq(self.lines + 1),
                    self.line_stb.eq(1),
                ]

        # The port: a pending write goes first, the renderer waits a cycle.
        with m.If(w_pending):
            m.d.comb += [
                ram.addr.eq(w_addr >> 1),
                ram.w_data.eq(w_byte.replicate(2)),
                ram.mask.eq(Mux(w_addr[0], 0b10, 0b01)),
                ram.we.eq(1),
            ]
            m.d.sync += w_pending.eq(0)
        with m.Else():
            m.d.comb += [ram.addr.eq(self.r_addr >> 1), self.r_ack.eq(self.r_req)]
        m.d.sync += r_low.eq(~self.r_addr[0])
        m.d.comb += self.r_data.eq(Mux(r_low, ram.r_data[:8], ram.r_data[8:]))
        return m
