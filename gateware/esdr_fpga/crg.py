# SPDX-License-Identifier: GPL-3.0-or-later
"""Clock and reset: 12 MHz pad -> SB_PLL40_PAD -> sync, held in reset after lock."""

from amaranth.hdl import ClockDomain, ClockSignal, Elaboratable, Instance, Module, Signal

__all__ = ["CRG", "PLL_SETTINGS"]

# icepll -i 12 -o <f>: (DIVR, DIVF, DIVQ, FILTER_RANGE).
PLL_SETTINGS = {
    30e6: (0, 79, 5, 1),
    24e6: (0, 63, 5, 1),
}


class CRG(Elaboratable):
    """sync at sys_clk_freq. Reset holds 2**reset_bits cycles after lock (the EBRs read zero for
    about 3 us after configuration)."""

    def __init__(self, sys_clk_freq=30e6, reset_bits=10):
        self.sys_clk_freq = sys_clk_freq
        self.reset_bits = reset_bits
        self.locked = Signal()

    def elaborate(self, platform):
        m = Module()
        m.domains.sync = cd_sync = ClockDomain("sync")
        m.domains.por = cd_por = ClockDomain("por", reset_less=True)

        if platform is None:
            # Simulation drives sync directly.
            return m

        pad = platform.request("clk12", dir="-")
        divr, divf, divq, filt = PLL_SETTINGS[self.sys_clk_freq]
        m.submodules.pll = Instance(
            "SB_PLL40_PAD",
            p_FEEDBACK_PATH="SIMPLE",
            p_DIVR=divr, p_DIVF=divf, p_DIVQ=divq, p_FILTER_RANGE=filt,
            io_PACKAGEPIN=pad.io,
            i_RESETB=1, i_BYPASS=0,
            o_PLLOUTGLOBAL=cd_sync.clk,
            o_LOCK=self.locked,
        )
        platform.add_clock_constraint(cd_sync.clk, self.sys_clk_freq)

        # Reset counter in sync itself: restarts whenever lock drops.
        count = Signal(self.reset_bits + 1)
        m.d.comb += cd_por.clk.eq(ClockSignal("sync"))
        with m.If(~self.locked):
            m.d.por += count.eq(0)
        with m.Elif(~count[-1]):
            m.d.por += count.eq(count + 1)
        m.d.comb += cd_sync.rst.eq(~count[-1])
        return m
