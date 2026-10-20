# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-level SPI master for simulation, behaving like the ESP32's half-duplex driver."""

import struct

from esdr_fpga.proto import OP_READ, OP_WRITE

__all__ = ["SPIMaster"]


class SPIMaster:
    def __init__(self, dut, freq=20e6):
        self.dut = dut
        self.half = 0.5 / freq

    async def frame(self, ctx, tx, rx_len=0, gap=100e-9):
        """CS low, clock out tx then rx_len zero bytes while sampling MISO, CS high."""
        d = self.dut
        ctx.set(d.cs, 1)
        await ctx.delay(gap)
        rx = bytearray()
        for byte in bytes(tx) + bytes(rx_len):
            got = 0
            for b in range(8):
                ctx.set(d.copi, (byte >> (7 - b)) & 1)
                await ctx.delay(self.half)
                ctx.set(d.sck, 1)
                got = (got << 1) | ctx.get(d.cipo)
                await ctx.delay(self.half)
                ctx.set(d.sck, 0)
            rx.append(got)
        await ctx.delay(gap)
        ctx.set(d.cs, 0)
        await ctx.delay(gap)
        return bytes(rx)

    async def write(self, ctx, addr, words):
        await self.frame(ctx, struct.pack(f"<BH{len(words)}H", OP_WRITE, addr, *words))

    async def read(self, ctx, addr, n):
        """As esdr_fpga_reg_read(): a 7-byte header, then 2n bytes in."""
        head = struct.pack("<BHHH", OP_READ, addr, n, 0)
        rx = await self.frame(ctx, head, 2 * n)
        return list(struct.unpack(f"<{n}H", rx[-2 * n:]))
