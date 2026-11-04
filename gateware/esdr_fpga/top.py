# SPDX-License-Identifier: GPL-3.0-or-later
"""Top level on the MCH2022 badge: clocks, pins, the core."""

from amaranth.hdl import Elaboratable, Instance, Module, Signal
from amaranth.lib import io

from .blinky import Blinky
from .core import Core
from .crg import CRG
from .prims import LedBus

__all__ = ["Top"]


class Top(Elaboratable):
    def __init__(self, sys_clk_freq=30e6, build=0, miso_test=False, debug=False):
        self.sys_clk_freq = sys_clk_freq
        self.build = build
        # A line of probes per second on the UART to the RP2040 (USB interface 2).
        self.debug = debug
        # Bring-up: MISO held high while selected.
        self.miso_test = miso_test

    def elaborate(self, platform):
        m = Module()
        m.submodules.crg = CRG(self.sys_clk_freq)
        m.submodules.core = core = Core(build=self.build, debug=self.debug)

        # The LED, which the host sets up over its register.
        m.submodules.led = led = LedBus()
        m.d.comb += [led.stb.eq(core.led_stb), led.value.eq(core.led)]

        # A heartbeat with probes on the FPGA UART (no CRG of its own here).
        if self.debug:
            m.submodules.heartbeat = Blinky(self.sys_clk_freq, crg=False, led=False,
                                            probes=[core.dbg_frames, core.dbg_last,
                                                    core.dbg_len])

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

        # Panel: registered outputs, enabled while the ESP32 has handed the bus over; CS low
        # whenever enabled.
        lcd = platform.request("lcd", 0, dir={"d": "-", "rs": "-", "wr": "-", "cs": "-",
                                              "mode": "-", "rst": "-", "fmark": "-"})
        m.submodules.lcd_d = lcd_d = io.FFBuffer("o", lcd.d)
        m.submodules.lcd_rs = lcd_rs = io.FFBuffer("o", lcd.rs)
        m.submodules.lcd_wr = lcd_wr = io.FFBuffer("o", lcd.wr)
        m.submodules.lcd_cs = lcd_cs = io.FFBuffer("o", lcd.cs)
        m.submodules.lcd_mode = lcd_mode = io.Buffer("i", lcd.mode)
        m.submodules.lcd_fmark = lcd_fmark = io.Buffer("i", lcd.fmark)
        m.d.comb += [
            core.lcd_mode.eq(lcd_mode.i),
            core.lcd_fmark.eq(lcd_fmark.i),
            lcd_d.o.eq(core.lcd_d),
            lcd_rs.o.eq(core.lcd_rs),
            lcd_wr.o.eq(core.lcd_wr),
            lcd_cs.o.eq(1),
        ]
        for buf in (lcd_d, lcd_rs, lcd_wr, lcd_cs):
            m.d.comb += buf.oe.eq(core.lcd_oe)
        return m
