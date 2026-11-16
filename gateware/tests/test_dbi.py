# SPDX-License-Identifier: GPL-3.0-or-later
import struct

from amaranth.sim import Simulator

from esdr_fpga.core import Core
from esdr_fpga.dbi import F_NOCMD, OP_DBI
from esdr_fpga.regs import REGS
from spimaster import SPIMaster


def run(bench):
    """bench(ctx, spi) -> list of (rs, byte) the panel saw; checks WR timing on the way."""
    dut = Core()
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    spi = SPIMaster(dut, 40e6)
    seen = []

    async def tb(ctx):
        ctx.set(dut.lcd_mode, 1)
        await ctx.delay(200e-9)
        await bench(ctx, spi)
        await ctx.delay(2e-6)

    async def panel(ctx):
        low = 0
        async for clk, rst, wr, rs, d in ctx.tick().sample(dut.lcd_wr, dut.lcd_rs, dut.lcd_d):
            if wr:
                low += 1
                seen.append((rs, d))
            else:
                low = 0
            assert low <= 1, "WR low for more than one cycle"

    sim.add_testbench(tb)
    sim.add_process(panel)
    sim.run()
    return seen


def dbi(cmd, data=b"", nocmd=False):
    if nocmd:
        return bytes([OP_DBI, F_NOCMD]) + data
    return bytes([OP_DBI, 0, cmd]) + data


def test_passthrough_and_madctl():
    async def bench(ctx, spi):
        await spi.frame(ctx, dbi(0x2A, bytes([0, 0, 1, 0x3F])))
        await spi.frame(ctx, dbi(0x36, b"\x28"))
        await spi.frame(ctx, dbi(0x2C))
        await spi.frame(ctx, dbi(0, bytes(range(10)), nocmd=True))
        assert await spi.read(ctx, REGS["MADCTL_HOST"][0], 1) == [0x28]
        assert await spi.read(ctx, REGS["DBI_BYTES"][0], 1) == [5 + 2 + 1 + 10]

    seen = run(bench)
    assert seen == ([(0, 0x2A)] + [(1, b) for b in (0, 0, 1, 0x3F)]
                    + [(0, 0x36), (1, 0x28), (0, 0x2C)] + [(1, b) for b in range(10)])


def test_bus_idle_without_mode():
    async def bench(ctx, spi):
        ctx.set(spi.dut.lcd_mode, 0)
        await spi.frame(ctx, dbi(0x2C, b"\x01\x02"))
        assert await spi.read(ctx, REGS["DBI_BYTES"][0], 1) == [0]
    assert run(bench) == []
