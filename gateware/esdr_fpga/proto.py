# SPDX-License-Identifier: GPL-3.0-or-later
"""SPI frames to bus accesses and byte streams. One frame (CS low) is one command.

    0x01 WRITE  a16, w16...         words to a, a+1, ...
    0x02 READ   a16, n16, x16       then 2n bytes of words from a, a+1, ... (x: two pad bytes,
                                    the time to fetch the first word)
    0x00, 0xff  no operation

Little-endian fields. Other opcodes go out on `stream` (opcode, then every payload byte) for the
blocks that own them.
"""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In, Out

from . import bus
from .spi import SPISlave

__all__ = ["Protocol", "OP_WRITE", "OP_READ", "READ_HEADER"]

OP_WRITE = 0x01
OP_READ = 0x02
READ_HEADER = 7


class Protocol(wiring.Component):
    bus: Out(bus.Signature())
    # Bytes of other opcodes: first marks the opcode byte; end pulses when CS rises.
    s_data: Out(8)
    s_first: Out(1)
    s_stb: Out(1)
    s_end: Out(1)

    def __init__(self):
        super().__init__()
        self.spi = SPISlave()

    def elaborate(self, platform):
        m = Module()
        m.submodules.spi = spi = self.spi

        pos = Signal(3)           # header bytes so far, saturating at 7
        op = Signal(8)
        addr = Signal(16)
        count = Signal(8)         # READ: low byte of n until the high byte arrives
        remain = Signal(17)       # READ: data bytes still to set up
        odd = Signal()            # READ/WRITE: the next byte is a high byte
        lo = Signal(8)            # low byte of a word being written
        hi = Signal(8)            # high byte of a word being read
        rd_req = Signal()         # a bus read was issued last cycle
        rd_lo = Signal()          # its data is on the bus now

        byte = spi.rx_data
        m.d.comb += [
            self.s_data.eq(byte),
            self.s_end.eq(spi.end),
        ]
        m.d.sync += [self.bus.w_stb.eq(0), self.bus.r_stb.eq(0), rd_req.eq(0),
                     rd_lo.eq(rd_req)]
        with m.If(rd_lo):
            m.d.sync += [spi.tx_data.eq(self.bus.r_data[:8]), hi.eq(self.bus.r_data[8:])]

        with m.If(spi.start):
            m.d.sync += [pos.eq(0), spi.tx_data.eq(0), remain.eq(0), odd.eq(0)]
        with m.Elif(spi.rx_stb):
            with m.If(pos != 7):
                m.d.sync += pos.eq(pos + 1)
            with m.If(pos == 0):
                m.d.sync += op.eq(byte)
                m.d.comb += [self.s_first.eq(1),
                             self.s_stb.eq((byte > OP_READ) & (byte != 0xff))]
            with m.Else():
                m.d.comb += self.s_stb.eq((op > OP_READ) & (op != 0xff))

                with m.If(pos == 1):
                    m.d.sync += addr[:8].eq(byte)
                with m.Elif(pos == 2):
                    m.d.sync += addr[8:].eq(byte)
                with m.Elif(op == OP_WRITE):
                    m.d.sync += odd.eq(~odd)
                    with m.If(~odd):
                        m.d.sync += lo.eq(byte)
                    with m.Else():
                        m.d.sync += [
                            self.bus.addr.eq(addr),
                            self.bus.w_data.eq((byte << 8) | lo),
                            self.bus.w_stb.eq(1),
                            addr.eq(addr + 1),
                        ]
                with m.Elif(op == OP_READ):
                    with m.If(pos == 3):
                        m.d.sync += count[:8].eq(byte)
                    with m.Elif(pos == 4):
                        # Data byte j is frame byte READ_HEADER + j and is set up on the strobe
                        # two bytes earlier, the first one on the next strobe.
                        m.d.sync += remain.eq(((byte << 8) | count[:8]) << 1)
                    with m.Elif(remain != 0):
                        m.d.sync += [remain.eq(remain - 1), odd.eq(~odd)]
                        with m.If(~odd):
                            m.d.sync += [
                                self.bus.addr.eq(addr),
                                self.bus.r_stb.eq(1),
                                rd_req.eq(1),
                                addr.eq(addr + 1),
                            ]
                        with m.Else():
                            m.d.sync += spi.tx_data.eq(hi)
                    with m.Else():
                        m.d.sync += spi.tx_data.eq(0)
        return m
