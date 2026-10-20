# SPDX-License-Identifier: GPL-3.0-or-later
"""Registers. The table here is the single source for the gateware, the C header and the docs."""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

from . import bus

__all__ = ["REGS", "ID", "VERSION", "Registers"]

ID = 0xE5DF
VERSION = (0 << 8) | 1

# name: (address, access, description)
REGS = {
    "ID":      (0x00, "r",  "0xE5DF"),
    "VERSION": (0x01, "r",  "gateware version, major in bits 15:8"),
    "BUILD":   (0x02, "r",  "build hash, low 16 bits"),
    "CAPS":    (0x03, "r",  "features of this variant"),
    "SCRATCH": (0x07, "rw", "free for host tests"),
}


class Registers(wiring.Component):
    """Fixed and scratch registers, window 0x0000..0x00ff."""

    bus: In(bus.Signature(8))

    def __init__(self, build=0, caps=0):
        self.build = build
        self.caps = caps
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        scratch = Signal(16)
        consts = {
            REGS["ID"][0]: ID,
            REGS["VERSION"][0]: VERSION,
            REGS["BUILD"][0]: self.build & 0xffff,
            REGS["CAPS"][0]: self.caps,
        }
        with m.If(self.bus.w_stb & (self.bus.addr == REGS["SCRATCH"][0])):
            m.d.sync += scratch.eq(self.bus.w_data)
        with m.If(self.bus.r_stb):
            with m.Switch(self.bus.addr):
                for a, v in consts.items():
                    with m.Case(a):
                        m.d.sync += self.bus.r_data.eq(v)
                with m.Case(REGS["SCRATCH"][0]):
                    m.d.sync += self.bus.r_data.eq(scratch)
                with m.Default():
                    m.d.sync += self.bus.r_data.eq(0)
        return m
