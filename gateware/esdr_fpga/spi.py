# SPDX-License-Identifier: GPL-3.0-or-later
"""SPI slave, mode 0, MSB first, for the ESP32 on the iCE40's configuration port.

The shift register runs in its own domain on the SPI clock (a global buffer on the pad), reset
while CS is high. Each byte crosses to sync through a toggle, as in Sylvain Munaut's
spi_dev_core.v (mch2022-firmware-ice40, CERN-OHL-P-2.0). MISO lags: the byte presented on
tx_data when the strobe for byte i arrives goes out as byte i + 2.
"""

from amaranth.hdl import ClockDomain, ClockSignal, Module, ResetSignal, Signal
from amaranth.lib import wiring
from amaranth.lib.cdc import FFSynchronizer
from amaranth.lib.wiring import In, Out

__all__ = ["SPISlave"]


class SPISlave(wiring.Component):
    """Pins (sck, copi, cs active high) in; cipo out. In sync: rx_data/rx_stb per byte, start
    and end of frame, tx_data for the byte after next."""

    sck: In(1)
    copi: In(1)
    cs: In(1)
    cipo: Out(1)

    rx_data: Out(8)
    rx_stb: Out(1)
    start: Out(1)
    end: Out(1)
    active: Out(1)
    tx_data: In(8)

    def elaborate(self, platform):
        m = Module()
        m.domains.spi = cd_spi = ClockDomain("spi", async_reset=True, local=True)
        m.domains.spi_n = cd_spi_n = ClockDomain("spi_n", clk_edge="neg", async_reset=True,
                                                 local=True)
        m.d.comb += [
            cd_spi.clk.eq(self.sck),
            cd_spi.rst.eq(~self.cs),
            cd_spi_n.clk.eq(self.sck),
            cd_spi_n.rst.eq(~self.cs),
        ]

        # SPI clock domain: shift in on rising edges.
        bit = Signal(3)
        shreg = Signal(7)
        byte = Signal(8, reset_less=True)
        toggle = Signal(reset_less=True)
        m.d.spi += [
            bit.eq(bit + 1),
            shreg.eq((shreg << 1) | self.copi),
        ]
        with m.If(bit == 7):
            m.d.spi += [byte.eq((shreg << 1) | self.copi), toggle.eq(~toggle)]

        # Shift out on falling edges; the first bit of a byte is set up by the falling edge
        # that ends the previous byte (bit wrapped to 0).
        txreg = Signal(8)
        with m.If(bit == 0):
            m.d.spi_n += txreg.eq(self.tx_data)
        with m.Else():
            m.d.spi_n += txreg.eq(txreg << 1)
        m.d.comb += self.cipo.eq(txreg[7])

        # To sync.
        toggle_s = Signal()
        toggle_d = Signal()
        cs_s = Signal()
        cs_d = Signal()
        m.submodules.toggle_sync = FFSynchronizer(toggle, toggle_s)
        m.submodules.cs_sync = FFSynchronizer(self.cs, cs_s)
        m.d.sync += [
            toggle_d.eq(toggle_s),
            cs_d.eq(cs_s),
            self.rx_stb.eq(toggle_s != toggle_d),
            self.rx_data.eq(byte),
            self.start.eq(cs_s & ~cs_d),
            self.end.eq(~cs_s & cs_d),
        ]
        m.d.comb += self.active.eq(cs_d)
        return m
