# SPDX-License-Identifier: GPL-3.0-or-later
"""The link variant: the framed serial link's blocks (include/esp_sdr/esdr_link.h) from the ESP32
to the RP2040, by way of the FPGA, instead of the panel's renderer.

The ESP32's UART is limited by its CPU: it feeds a FIFO by hand and starves it while a burst is
captured. SPI takes a frame by DMA at 5 MB/s, and this holds it in SPRAM and sends it on the UART
pin at a rate the RP2040's PIO can read. Frames are opaque here. An odd byte at the end of an SPI
frame is padded with a zero, which the link reads as an empty frame. The RP2040 pulls `ready` low when
its ring fills up: the sender pauses between bytes. The FIFO cannot overflow when the host's
credit stays below it, as it does; LINK_LOST counts if it did.
"""

from amaranth.hdl import Elaboratable, Instance, Module, Mux, Signal
from amaranth.lib import io, wiring
from amaranth.lib.cdc import FFSynchronizer
from amaranth.lib.wiring import In, Out

from .crg import CRG
from .prims import Spram
from .proto import Protocol
from .regs import ID
from .uart import UARTTx

__all__ = ["CAPS_LINK", "FIFO_WORDS", "LINK_REGS", "LinkCore", "LinkTop", "OP_LINK", "VERSION"]

# SPI frame: this opcode, then the bytes.
OP_LINK = 0x20
FIFO_WORDS = 16384
# Register CAPS: this variant. The renderer's has CAPS_DSP in bit 0.
CAPS_LINK = 1 << 1
VERSION = 1
# Registers, as esp_sdr/esdr_link.h has them.
LINK_REGS = {
    "ID": 0x00,
    "VERSION": 0x01,
    "CAPS": 0x03,
    # Words free in the FIFO.
    "LINK_FREE": 0x20,
    # Bytes sent to the RP2040, low 16 bits.
    "LINK_SENT": 0x21,
    # Words the FIFO had no room for, sticky.
    "LINK_LOST": 0x22,
    # Cycles (sync, over 256) the RP2040 held the sender back, low 16 bits.
    "LINK_HELD": 0x23,
}


class LinkCore(wiring.Component):
    sck: In(1)
    copi: In(1)
    cs: In(1)
    cipo: Out(1)
    # From the RP2040: it has room.
    ready: In(1)
    # To the RP2040.
    tx: Out(1, init=1)

    def __init__(self, divisor=2):
        self.divisor = divisor
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        m.submodules.proto = proto = Protocol()
        m.submodules.uart = uart = UARTTx(self.divisor)
        m.submodules.ram = ram = Spram(1)

        m.d.comb += [
            proto.spi.sck.eq(self.sck),
            proto.spi.copi.eq(self.copi),
            proto.spi.cs.eq(self.cs),
            self.cipo.eq(proto.spi.cipo),
            self.tx.eq(uart.tx),
        ]

        ready = Signal()
        m.submodules.ready_sync = FFSynchronizer(self.ready, ready, init=1)

        head = Signal(14)
        tail = Signal(14)
        used = Signal(15)
        lost = Signal(16)
        sent = Signal(16)
        held = Signal(24)

        # From SPI: bytes to words.
        is_link = Signal()
        have_lo = Signal()
        lo = Signal(8)
        w_pending = Signal()
        w_word = Signal(16)

        with m.If(proto.s_stb):
            with m.If(proto.s_first):
                m.d.sync += [is_link.eq(proto.s_data == OP_LINK), have_lo.eq(0)]
            with m.Elif(is_link):
                with m.If(have_lo):
                    m.d.sync += [w_word.eq((proto.s_data << 8) | lo), w_pending.eq(1),
                                 have_lo.eq(0)]
                with m.Else():
                    m.d.sync += [lo.eq(proto.s_data), have_lo.eq(1)]
        # The end of the SPI frame: an odd byte goes with a zero, an empty frame on the link.
        with m.If(proto.s_end):
            m.d.sync += is_link.eq(0)
            with m.If(is_link & have_lo):
                m.d.sync += [w_word.eq(lo), w_pending.eq(1), have_lo.eq(0)]

        # One access to the RAM a cycle: a word coming in goes first, it has nowhere to wait.
        out_word = Signal(16)
        out_bytes = Signal(2)
        r_wait = Signal()
        do_write = w_pending
        do_read = Signal()

        m.d.comb += [
            do_read.eq(~do_write & ~r_wait & (used != 0) & (out_bytes == 0)),
            ram.addr.eq(Mux(do_write, head, tail)),
            ram.w_data.eq(w_word),
            ram.we.eq(do_write & (used != FIFO_WORDS)),
        ]
        m.d.sync += r_wait.eq(do_read)
        with m.If(do_write):
            m.d.sync += w_pending.eq(0)
            with m.If(used != FIFO_WORDS):
                m.d.sync += head.eq(head + 1)
            with m.Else():
                m.d.sync += lost.eq(lost + 1)
        with m.If(do_read):
            m.d.sync += tail.eq(tail + 1)
        with m.If(do_write & (used != FIFO_WORDS) & ~do_read):
            m.d.sync += used.eq(used + 1)
        with m.Elif(do_read & ~(do_write & (used != FIFO_WORDS))):
            m.d.sync += used.eq(used - 1)
        with m.If(r_wait):
            m.d.sync += [out_word.eq(ram.r_data), out_bytes.eq(2)]

        # To the RP2040, a byte at a time, while it has room.
        m.d.comb += [
            uart.data.payload.eq(out_word[:8]),
            uart.data.valid.eq((out_bytes != 0) & ready),
        ]
        with m.If(uart.data.valid & uart.data.ready):
            m.d.sync += [
                out_word.eq(out_word >> 8),
                out_bytes.eq(out_bytes - 1),
                sent.eq(sent + 1),
            ]
        with m.If((out_bytes != 0) & ~ready):
            m.d.sync += held.eq(held + 1)

        # Registers: the bus's read data comes the cycle after.
        with m.If(proto.bus.r_stb):
            with m.Switch(proto.bus.addr):
                with m.Case(LINK_REGS["ID"]):
                    m.d.sync += proto.bus.r_data.eq(ID)
                with m.Case(LINK_REGS["VERSION"]):
                    m.d.sync += proto.bus.r_data.eq(VERSION)
                with m.Case(LINK_REGS["CAPS"]):
                    m.d.sync += proto.bus.r_data.eq(CAPS_LINK)
                with m.Case(LINK_REGS["LINK_FREE"]):
                    m.d.sync += proto.bus.r_data.eq(FIFO_WORDS - 1 - used[:14])
                with m.Case(LINK_REGS["LINK_SENT"]):
                    m.d.sync += proto.bus.r_data.eq(sent)
                with m.Case(LINK_REGS["LINK_LOST"]):
                    m.d.sync += proto.bus.r_data.eq(lost)
                with m.Case(LINK_REGS["LINK_HELD"]):
                    m.d.sync += proto.bus.r_data.eq(held[8:])
                with m.Default():
                    m.d.sync += proto.bus.r_data.eq(0)
        return m


class LinkTop(Elaboratable):
    """On the badge: clocks and pins. The panel's stay inputs, the ESP32 keeps the display."""

    def __init__(self, sys_clk_freq=30e6, divisor=2):
        self.sys_clk_freq = sys_clk_freq
        self.divisor = divisor

    def elaborate(self, platform):
        m = Module()
        m.submodules.crg = CRG(self.sys_clk_freq)
        m.submodules.core = core = LinkCore(self.divisor)

        spi = platform.request("spi", 0, dir={"clk": "-", "copi": "-", "cipo": "-", "cs": "-"})
        m.submodules.sck_buf = sck_buf = io.Buffer("i", spi.clk)
        m.submodules.copi_buf = copi_buf = io.Buffer("i", spi.copi)
        m.submodules.cs_buf = cs_buf = io.Buffer("i", spi.cs)
        m.submodules.cipo_buf = cipo_buf = io.Buffer("o", spi.cipo)

        # As in Top: the PLL output takes the global buffer at X19/Y0.
        sck = Signal()
        m.submodules.sck_gb = Instance(
            "SB_GB",
            a_BEL="X13/Y0/gb",
            i_USER_SIGNAL_TO_GLOBAL_BUFFER=sck_buf.i,
            o_GLOBAL_BUFFER_OUTPUT=sck,
        )
        platform.add_clock_constraint(sck, 50e6)

        uart = platform.request("uart", 0, dir={"tx": "-", "rx": "-"})
        m.submodules.tx_buf = tx_buf = io.FFBuffer("o", uart.tx)
        m.submodules.rx_buf = rx_buf = io.Buffer("i", uart.rx)

        m.d.comb += [
            core.sck.eq(sck),
            core.copi.eq(copi_buf.i),
            core.cs.eq(cs_buf.i),
            cipo_buf.o.eq(core.cipo),
            cipo_buf.oe.eq(cs_buf.i),
            core.ready.eq(rx_buf.i),
            tx_buf.o.eq(core.tx),
            tx_buf.oe.eq(1),
        ]
        return m
