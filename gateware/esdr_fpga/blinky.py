# SPDX-License-Identifier: GPL-3.0-or-later
"""Bring-up image: PLL, a breathing LED and one UART line per second; the UART line alone is the
debug variant's heartbeat."""

from amaranth.hdl import Const, Elaboratable, Module, Mux, Signal
from amaranth.lib import io

from .crg import CRG
from .prims import LedBus, LedFade
from .uart import UARTTx

__all__ = ["Blinky"]


class Blinky(Elaboratable):
    def __init__(self, sys_clk_freq=30e6, baud=115200, crg=True, probes=(), led=True):
        self.sys_clk_freq = sys_clk_freq
        self.baud = baud
        self.crg = crg
        self.led = led
        # 16-bit signals printed in hex after the seconds.
        self.probes = list(probes)

    def elaborate(self, platform):
        m = Module()
        if self.crg:
            m.submodules.crg = CRG(self.sys_clk_freq)
        if self.led:
            m.submodules.led = led = LedBus()
            m.submodules.fade = fade = LedFade(self.sys_clk_freq)
            m.d.comb += [led.stb.eq(fade.stb), led.value.eq(fade.value)]
        self.report(m, platform)
        return m

    def report(self, m, platform):
        m.submodules.tx = tx = UARTTx(round(self.sys_clk_freq / self.baud))

        # Seconds counter and a 5-digit decimal count of them.
        tick = Signal(range(int(self.sys_clk_freq)))
        second = Signal()
        m.d.sync += second.eq(0)
        with m.If(tick == int(self.sys_clk_freq) - 1):
            m.d.sync += [tick.eq(0), second.eq(1)]
        with m.Else():
            m.d.sync += tick.eq(tick + 1)

        digits = [Signal(4, name=f"d{i}") for i in range(5)]
        with m.If(second):
            carry = Const(1)
            for i, d in enumerate(digits):
                with m.If(carry & (d == 9)):
                    m.d.sync += d.eq(0)
                with m.Elif(carry):
                    m.d.sync += d.eq(d + 1)
                carry = carry & (d == 9)

        # "esdr-fpga 00000[ xxxx...]\r\n", sent once per second.
        prefix = b"esdr-fpga "
        nprobe = len(self.probes)
        length = len(prefix) + len(digits) + 5 * nprobe + 2
        latched = [Signal(16, name=f"probe{i}") for i in range(nprobe)]
        with m.If(second):
            m.d.sync += [l.eq(p) for l, p in zip(latched, self.probes)]
        index = Signal(range(length + 1), init=length)
        char = Signal(8)
        with m.Switch(index):
            for i, c in enumerate(prefix):
                with m.Case(i):
                    m.d.comb += char.eq(c)
            for i, d in enumerate(reversed(digits)):
                with m.Case(len(prefix) + i):
                    m.d.comb += char.eq(ord("0") + d)
            for i, l in enumerate(latched):
                base = len(prefix) + len(digits) + 5 * i
                with m.Case(base):
                    m.d.comb += char.eq(ord(" "))
                for n in range(4):
                    with m.Case(base + 1 + n):
                        nib = l[4 * (3 - n):4 * (4 - n)]
                        m.d.comb += char.eq(Mux(nib < 10, ord("0") + nib, ord("a") - 10 + nib))
            with m.Case(length - 2):
                m.d.comb += char.eq(ord("\r"))
            with m.Case(length - 1):
                m.d.comb += char.eq(ord("\n"))
        m.d.comb += [
            tx.data.payload.eq(char),
            tx.data.valid.eq(index < length),
        ]
        with m.If(second):
            m.d.sync += index.eq(0)
        with m.Elif(tx.data.valid & tx.data.ready):
            m.d.sync += index.eq(index + 1)

        if platform is not None:
            uart = platform.request("uart", 0, dir={"tx": "-", "rx": "-"})
            m.submodules.uart_tx = uart_tx = io.Buffer("o", uart.tx)
            m.d.comb += uart_tx.o.eq(tx.tx)
