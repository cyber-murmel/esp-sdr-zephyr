# SPDX-License-Identifier: GPL-3.0-or-later
import random

from amaranth.sim import Simulator

from esdr_fpga.core import SCRATCH_RAM, Core
from esdr_fpga.regs import ID, REGS
from spimaster import SPIMaster


def run(bench, freq=20e6):
    dut = Core()
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    spi = SPIMaster(dut, freq)

    async def tb(ctx):
        await ctx.delay(200e-9)
        await bench(ctx, spi)

    sim.add_testbench(tb)
    sim.run()


def test_id_and_scratch():
    async def bench(ctx, spi):
        assert await spi.read(ctx, REGS["ID"][0], 1) == [ID]
        await spi.write(ctx, REGS["SCRATCH"][0], [0xBEEF])
        assert await spi.read(ctx, REGS["SCRATCH"][0], 1) == [0xBEEF]
    run(bench)


def test_ram_burst():
    rng = random.Random(1)
    words = [rng.randrange(1 << 16) for _ in range(64)]

    async def bench(ctx, spi):
        await spi.write(ctx, SCRATCH_RAM, words)
        assert await spi.read(ctx, SCRATCH_RAM, 64) == words
        assert await spi.read(ctx, SCRATCH_RAM + 10, 3) == words[10:13]
    run(bench)


def test_write_at_40mhz():
    async def bench(ctx, spi):
        await spi.write(ctx, SCRATCH_RAM, [1, 2, 3, 4])
        spi.half = 0.5 / 20e6
        assert await spi.read(ctx, SCRATCH_RAM, 4) == [1, 2, 3, 4]
    run(bench, freq=40e6)
