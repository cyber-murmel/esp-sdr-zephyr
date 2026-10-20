# SPDX-License-Identifier: GPL-3.0-or-later
"""The internal register bus: 16-bit words, one access per cycle, read data the next cycle."""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

__all__ = ["Signature", "Decoder"]


class Signature(wiring.Signature):
    def __init__(self, addr_width=16):
        super().__init__({
            "addr": Out(addr_width),
            "w_data": Out(16),
            "w_stb": Out(1),
            "r_stb": Out(1),
            "r_data": In(16),
        })


class Decoder(wiring.Component):
    """Routes a bus to windows at (base, size) with size a power of two and base aligned."""

    def __init__(self, windows):
        self.windows = windows
        super().__init__({
            "bus": In(Signature()),
            **{f"w{i}": Out(Signature((size - 1).bit_length() if size > 1 else 1))
               for i, (base, size) in enumerate(windows)},
        })

    def elaborate(self, platform):
        m = Module()
        sel = Signal(range(len(self.windows) + 1))
        for i, (base, size) in enumerate(self.windows):
            port = getattr(self, f"w{i}")
            bits = (size - 1).bit_length()
            hit = Signal(name=f"hit{i}")
            m.d.comb += [
                hit.eq((self.bus.addr >> bits) == (base >> bits)),
                port.addr.eq(self.bus.addr),
                port.w_data.eq(self.bus.w_data),
                port.w_stb.eq(self.bus.w_stb & hit),
                port.r_stb.eq(self.bus.r_stb & hit),
            ]
            with m.If(self.bus.r_stb & hit):
                m.d.sync += sel.eq(i)
        with m.Switch(sel):
            for i in range(len(self.windows)):
                with m.Case(i):
                    m.d.comb += self.bus.r_data.eq(getattr(self, f"w{i}").r_data)
        return m
