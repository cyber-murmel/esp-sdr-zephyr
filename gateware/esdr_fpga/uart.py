# SPDX-License-Identifier: GPL-3.0-or-later
"""8N1 UART transmitter."""

from amaranth.hdl import Elaboratable, Module, Signal
from amaranth.lib import stream, wiring
from amaranth.lib.wiring import In, Out

__all__ = ["UARTTx"]


class UARTTx(wiring.Component):
    def __init__(self, divisor):
        self.divisor = divisor
        super().__init__({
            "data": In(stream.Signature(8)),
            "tx": Out(1, init=1),
        })

    def elaborate(self, platform):
        m = Module()
        shreg = Signal(10)
        bits = Signal(range(11))
        timer = Signal(range(self.divisor))

        m.d.comb += self.data.ready.eq(bits == 0)
        with m.If(self.data.valid & self.data.ready):
            m.d.sync += [
                shreg.eq((1 << 9) | (self.data.payload << 1)),
                bits.eq(10),
                timer.eq(0),
            ]
        with m.Elif(bits != 0):
            with m.If(timer == 0):
                m.d.sync += [
                    self.tx.eq(shreg[0]),
                    shreg.eq(shreg >> 1),
                    timer.eq(self.divisor - 1),
                ]
            with m.Else():
                m.d.sync += timer.eq(timer - 1)
                with m.If(timer == 1):
                    m.d.sync += bits.eq(bits - 1)
        return m
