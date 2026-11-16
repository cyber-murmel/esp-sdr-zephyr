# SPDX-License-Identifier: GPL-3.0-or-later
import random

from amaranth.sim import Simulator

from esdr_fpga import model
from esdr_fpga.fft import FFT


def run_fft(re, im):
    n = len(re)
    dut = FFT()
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    out = {}

    async def tb(ctx):
        ctx.set(dut.log2n, n.bit_length() - 1)
        for k in range(n):
            ctx.set(dut.ld_addr, k)
            ctx.set(dut.ld_re, re[k])
            ctx.set(dut.ld_im, im[k])
            ctx.set(dut.ld_we, 1)
            await ctx.tick()
        ctx.set(dut.ld_we, 0)
        ctx.set(dut.start, 1)
        await ctx.tick()
        ctx.set(dut.start, 0)
        cycles = 0
        while not ctx.get(dut.done):
            await ctx.tick()
            cycles += 1
        out["cycles"] = cycles
        out["exp"] = ctx.get(dut.exp)
        xr, xi = [], []
        for k in range(n):
            ctx.set(dut.rd_addr, k)
            await ctx.tick()
            xr.append(ctx.get(dut.rd_re))
            xi.append(ctx.get(dut.rd_im))
        out["re"], out["im"] = xr, xi

    sim.add_testbench(tb)
    sim.run()
    return out


def check(re, im):
    want_re, want_im, want_exp = model.fft(list(re), list(im))
    got = run_fft(re, im)
    assert got["exp"] == want_exp
    assert got["re"] == want_re
    assert got["im"] == want_im
    return got


def test_fft_16_random():
    rng = random.Random(4)
    check([rng.randint(-23000, 23000) for _ in range(16)],
          [rng.randint(-23000, 23000) for _ in range(16)])


def test_fft_512_random():
    rng = random.Random(11)
    check([rng.randint(-16000, 16000) for _ in range(512)],
          [rng.randint(-16000, 16000) for _ in range(512)])


def test_fft_64_small():
    rng = random.Random(5)
    check([rng.randint(-300, 300) for _ in range(64)],
          [rng.randint(-300, 300) for _ in range(64)])


def test_fft_256_tone():
    import math
    n = 256
    re = [round(9000 * math.cos(model.TAU * 17.3 * k / n)) for k in range(n)]
    im = [round(9000 * math.sin(model.TAU * 17.3 * k / n)) for k in range(n)]
    got = check(re, im)
    # 8 stages of N/2 butterflies at 2 cycles, plus the drains.
    assert got["cycles"] < 8 * (n + 20)
