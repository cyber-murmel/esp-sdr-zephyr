# SPDX-License-Identifier: GPL-3.0-or-later
"""Bus-attached block RAM."""

from amaranth.hdl import Module
from amaranth.lib import memory, wiring
from amaranth.lib.wiring import In

from . import bus

__all__ = ["BusRAM"]


class BusRAM(wiring.Component):
    def __init__(self, depth, init=()):
        self.depth = depth
        self.init = init
        super().__init__({"bus": In(bus.Signature((depth - 1).bit_length()))})

    def elaborate(self, platform):
        m = Module()
        m.submodules.mem = mem = memory.Memory(shape=16, depth=self.depth, init=self.init)
        w = mem.write_port()
        r = mem.read_port()
        m.d.comb += [
            w.addr.eq(self.bus.addr),
            w.data.eq(self.bus.w_data),
            w.en.eq(self.bus.w_stb),
            r.addr.eq(self.bus.addr),
            r.en.eq(self.bus.r_stb),
            self.bus.r_data.eq(r.data),
        ]
        return m
