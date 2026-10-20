# SPDX-License-Identifier: GPL-3.0-or-later
"""Registers. REGS is the single source for the gateware, the C header and the docs."""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In

from . import bus

__all__ = ["REGS", "ID", "VERSION", "Registers"]

ID = 0xE5DF
VERSION = (0 << 8) | 2

# name: (address, access, reset, description). "c": constant, "r": status (driven by the
# gateware), "rw": control (written by the host, read back).
REGS = {
    "ID":          (0x00, "c",  ID, "0xE5DF"),
    "VERSION":     (0x01, "c",  VERSION, "gateware version, major in bits 15:8"),
    "BUILD":       (0x02, "c",  0, "build hash, low 16 bits"),
    "CAPS":        (0x03, "c",  0, "features of this variant"),
    "STATUS":      (0x04, "r",  0, "b0 lcd_mode (FPGA owns the panel bus), b1 DBI FIFO "
                                  "not empty"),
    "SCRATCH":     (0x07, "rw", 0, "free for host tests"),
    "TE_CNT":      (0x0C, "r",  0, "rising edges of the panel's FMARK (TE)"),
    "DBI_BYTES":   (0x0D, "r",  0, "bytes written to the panel bus, low 16 bits"),
    "MADCTL_HOST": (0x41, "r",  0, "last MADCTL (0x36) the host passed through"),
}


class Registers(wiring.Component):
    """Window 0x0000..0x00ff. `status[name]` are inputs for "r" registers, `control[name]`
    outputs for "rw" ones; `consts` overrides constant values (BUILD, CAPS)."""

    bus: In(bus.Signature(8))

    def __init__(self, consts=None):
        self.consts = {name: v[2] for name, v in REGS.items() if v[1] == "c"}
        self.consts.update(consts or {})
        self.status = {name: Signal(16, name=name.lower())
                       for name, v in REGS.items() if v[1] == "r"}
        self.control = {name: Signal(16, name=name.lower(), init=v[2])
                        for name, v in REGS.items() if v[1] == "rw"}
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        with m.If(self.bus.w_stb):
            with m.Switch(self.bus.addr):
                for name, sig in self.control.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += sig.eq(self.bus.w_data)
        with m.If(self.bus.r_stb):
            with m.Switch(self.bus.addr):
                for name, value in self.consts.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += self.bus.r_data.eq(value & 0xffff)
                for name, sig in {**self.status, **self.control}.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += self.bus.r_data.eq(sig)
                with m.Default():
                    m.d.sync += self.bus.r_data.eq(0)
        return m
