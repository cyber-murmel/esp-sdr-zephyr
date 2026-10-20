# SPDX-License-Identifier: GPL-3.0-or-later
"""Top level on the MCH2022 badge: clocks, pins, the core."""

from amaranth.hdl import Elaboratable, Instance, Module, Signal
from amaranth.lib import io

from .blinky import Blinky
from .core import Core
from .crg import CRG

__all__ = ["Top"]


class Top(Elaboratable):
    def __init__(self, sys_clk_freq=30e6, build=0, miso_test=False):
        self.sys_clk_freq = sys_clk_freq
        self.build = build
        # Bring-up: MISO held high while selected.
        self.miso_test = miso_test

    def elaborate(self, platform):
        m = Module()
        m.submodules.crg = CRG(self.sys_clk_freq)
        m.submodules.core = core = Core(build=self.build)

        # Heartbeat on the LED and the FPGA UART (no CRG of its own here).
        m.submodules.heartbeat = Blinky(self.sys_clk_freq, crg=False,
                                        probes=[core.dbg_frames, core.dbg_last])

        spi = platform.request("spi", 0, dir={"clk": "-", "copi": "-", "cipo": "-", "cs": "-"})
        m.submodules.sck_buf = sck_buf = io.Buffer("i", spi.clk)
        m.submodules.copi_buf = copi_buf = io.Buffer("i", spi.copi)
        m.submodules.cs_buf = cs_buf = io.Buffer("i", spi.cs)
        m.submodules.cipo_buf = cipo_buf = io.Buffer("o", spi.cipo)

        # The PLL output takes the global buffer at X19/Y0, so the SPI clock's is pinned
        # (as in spi_dev_core.v).
        sck = Signal()
        m.submodules.sck_gb = Instance(
            "SB_GB",
            a_BEL="X13/Y0/gb",
            i_USER_SIGNAL_TO_GLOBAL_BUFFER=sck_buf.i,
            o_GLOBAL_BUFFER_OUTPUT=sck,
        )
        platform.add_clock_constraint(sck, 50e6)

        m.d.comb += [
            core.sck.eq(sck),
            core.copi.eq(copi_buf.i),
            core.cs.eq(cs_buf.i),
            cipo_buf.o.eq(1 if self.miso_test else core.cipo),
            cipo_buf.oe.eq(cs_buf.i),
        ]
        return m
