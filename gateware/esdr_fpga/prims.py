# SPDX-License-Identifier: GPL-3.0-or-later
"""iCE40 primitives behind small wrappers."""

from amaranth.hdl import Elaboratable, Instance, Module, Signal

__all__ = ["RgbDriver"]


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
