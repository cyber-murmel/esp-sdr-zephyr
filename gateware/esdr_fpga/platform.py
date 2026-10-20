# SPDX-License-Identifier: GPL-3.0-or-later
"""The MCH2022 badge's iCE40UP5K-SG48, pins from badgeteam's mch2022-prod.pcf."""

from amaranth.build import Attrs, Clock, Pins, PinsN, Resource, Subsignal
from amaranth.vendor import SiliconBluePlatform

__all__ = ["MCH2022Platform"]


class MCH2022Platform(SiliconBluePlatform):
    device = "iCE40UP5K"
    package = "SG48"
    default_clk = "clk12"
    resources = [
        # Oscillator X2 on IOT_46b_G0, the PLL pad.
        Resource("clk12", 0, Pins("35", dir="i"), Clock(12e6), Attrs(IO_STANDARD="SB_LVCMOS")),

        # To the RP2040's uart1, USB CDC interface 2 on the host.
        Resource("uart", 0,
                 Subsignal("tx", Pins("9", dir="o")),
                 Subsignal("rx", Pins("6", dir="i")),
                 Attrs(IO_STANDARD="SB_LVCMOS", PULLUP=1)),

        # ESP32 IO39.
        Resource("irq", 0, PinsN("10", dir="o"), Attrs(IO_STANDARD="SB_LVCMOS", PULLUP=1)),

        # ESP32 SPI3 (the configuration port): CLK IO18, MOSI IO23, MISO IO35, CS IO27.
        Resource("spi", 0,
                 Subsignal("clk", Pins("15", dir="i")),
                 Subsignal("copi", Pins("17", dir="i")),
                 Subsignal("cipo", Pins("14", dir="oe")),
                 Subsignal("cs", PinsN("16", dir="i"), Attrs(PULLUP=1)),
                 Attrs(IO_STANDARD="SB_LVCMOS")),

        # ESP-PSRAM64H, QSPI.
        Resource("psram", 0,
                 Subsignal("io", Pins("21 13 12 20", dir="io")),
                 Subsignal("clk", Pins("19", dir="o")),
                 Subsignal("cs", PinsN("18", dir="o"), Attrs(PULLUP=1)),
                 Attrs(IO_STANDARD="SB_LVCMOS")),

        # ILI9341, 8080 8-bit. mode is driven by the ESP32 (IO26) and only sensed here;
        # cs, wr and rst meet the ESP32's IO32, IO33 and IO25 through 47 ohm.
        Resource("lcd", 0,
                 Subsignal("d", Pins("26 27 31 32 34 37 38 42", dir="oe")),
                 Subsignal("rs", Pins("11", dir="oe")),
                 Subsignal("wr", PinsN("23", dir="oe")),
                 Subsignal("cs", PinsN("28", dir="oe")),
                 Subsignal("mode", Pins("43", dir="i")),
                 Subsignal("rst", PinsN("36", dir="i")),
                 Subsignal("fmark", Pins("25", dir="i")),
                 Attrs(IO_STANDARD="SB_LVCMOS")),

        Resource("pmod", 0, Pins("47 48 4 2 44 45 3 46", dir="io"),
                 Attrs(IO_STANDARD="SB_LVCMOS")),

        # Open-drain current sinks of SB_RGBA_DRV: RGB0 green, RGB1 red, RGB2 blue.
        Resource("rgb", 0,
                 Subsignal("g", Pins("39", dir="o")),
                 Subsignal("r", Pins("40", dir="o")),
                 Subsignal("b", Pins("41", dir="o"))),
    ]
    connectors = []
