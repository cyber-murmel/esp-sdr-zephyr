# SPDX-License-Identifier: GPL-3.0-or-later
"""ILI9341 8080 8-bit write bus. One byte per two sync cycles: WR low in the first, high in the
second (the panel latches on the rising edge); at 30 MHz a 66.7 ns write cycle, the
ILI9341's minimum is 66 ns. After lcd_phy.v in mch2022-firmware-ice40 (CERN-OHL-P-2.0)."""

from amaranth.hdl import Module, Signal
from amaranth.lib import stream, wiring
from amaranth.lib.cdc import FFSynchronizer
from amaranth.lib.wiring import In, Out

__all__ = ["LCDPhy", "BYTE_RS"]

# Stream payload: bit 8 is RS (1: data, 0: command).
BYTE_RS = 1 << 8


class LCDPhy(wiring.Component):
    """Pins as plain outputs (registered by the caller's FFBuffers) and enables. The bus is
    driven only while `mode` (the ESP32's LCD_MODE, sensed) is high; bytes that arrive while
    it is low are dropped."""

    data: In(stream.Signature(9))
    mode_in: In(1)
    fmark_in: In(1)

    d: Out(8)
    rs: Out(1)
    wr: Out(1)          # active high here: the pin is inverted
    oe: Out(1)
    mode: Out(1)
    te: Out(1)          # one cycle per FMARK rising edge
    count: Out(16)      # bytes written

    def elaborate(self, platform):
        m = Module()
        m.submodules.mode_sync = FFSynchronizer(self.mode_in, self.mode)
        fmark = Signal()
        fmark_d = Signal()
        m.submodules.fmark_sync = FFSynchronizer(self.fmark_in, fmark)
        m.d.sync += [fmark_d.eq(fmark), self.te.eq(fmark & ~fmark_d)]
        m.d.comb += self.oe.eq(self.mode)

        busy = Signal()
        m.d.comb += self.data.ready.eq(~busy)
        m.d.sync += self.wr.eq(0)
        with m.If(busy):
            m.d.sync += busy.eq(0)
        with m.Elif(self.data.valid):
            m.d.sync += [
                self.d.eq(self.data.payload[:8]),
                self.rs.eq(self.data.payload[8]),
                busy.eq(1),
            ]
            with m.If(self.mode):
                m.d.sync += [self.wr.eq(1), self.count.eq(self.count + 1)]
        return m
