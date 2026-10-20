# SPDX-License-Identifier: GPL-3.0-or-later
"""iCE40 primitives behind small wrappers."""

from amaranth.hdl import Cat, ClockSignal, Elaboratable, Instance, Module, Signal

__all__ = ["RgbDriver", "Spram"]


class RgbDriver(Elaboratable):
    """SB_RGBA_DRV at its lowest current (2 mA half-current mode); r, g, b are PWM inputs."""

    def __init__(self):
        self.r = Signal()
        self.g = Signal()
        self.b = Signal()

    def elaborate(self, platform):
        m = Module()
        if platform is None:
            return m
        pins = platform.request("rgb", 0, dir={"r": "-", "g": "-", "b": "-"})
        m.submodules.drv = Instance(
            "SB_RGBA_DRV",
            p_CURRENT_MODE="0b1",
            p_RGB0_CURRENT="0b000001",
            p_RGB1_CURRENT="0b000001",
            p_RGB2_CURRENT="0b000001",
            i_CURREN=1, i_RGBLEDEN=1,
            i_RGB0PWM=self.g, i_RGB1PWM=self.r, i_RGB2PWM=self.b,
            io_RGB0=pins.g.io, io_RGB1=pins.r.io, io_RGB2=pins.b.io,
        )
        return m


class Spram(Elaboratable):
    """SB_SPRAM256KA banks as one 16-bit RAM: `banks` x 16K words, one access per cycle, read
    data the cycle after. mask: one bit per byte (the primitive's nibble mask pairs)."""

    def __init__(self, banks=1):
        self.banks = banks
        self.addr = Signal(14 + max(banks - 1, 0).bit_length())
        self.w_data = Signal(16)
        self.mask = Signal(2, init=0b11)
        self.we = Signal()
        self.r_data = Signal(16)

    def elaborate(self, platform):
        m = Module()
        if platform is None:
            # Simulation model.
            from amaranth.lib import memory
            m.submodules.mem = mem = memory.Memory(shape=16, depth=16384 * self.banks, init=[])
            w = mem.write_port(granularity=8)
            r = mem.read_port()
            m.d.comb += [
                w.addr.eq(self.addr),
                w.data.eq(self.w_data),
                w.en.eq(self.mask.replicate(1) & self.we.replicate(2)),
                r.addr.eq(self.addr),
                self.r_data.eq(r.data),
            ]
            return m

        sel = Signal(range(self.banks))
        if self.banks > 1:
            m.d.sync += sel.eq(self.addr[14:])
        outs = []
        for i in range(self.banks):
            out = Signal(16, name=f"out{i}")
            cs = (self.addr[14:] == i) if self.banks > 1 else 1
            m.submodules[f"spram{i}"] = Instance(
                "SB_SPRAM256KA",
                i_ADDRESS=self.addr[:14],
                i_DATAIN=self.w_data,
                i_MASKWREN=Cat(self.mask[0], self.mask[0], self.mask[1], self.mask[1]),
                i_WREN=self.we,
                i_CHIPSELECT=cs,
                i_CLOCK=ClockSignal("sync"),
                i_STANDBY=0, i_SLEEP=0, i_POWEROFF=1,
                o_DATAOUT=out,
            )
            outs.append(out)
        with m.Switch(sel):
            for i, out in enumerate(outs):
                with m.Case(i):
                    m.d.comb += self.r_data.eq(out)
        return m
