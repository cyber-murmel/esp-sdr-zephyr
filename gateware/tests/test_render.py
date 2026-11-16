# SPDX-License-Identifier: GPL-3.0-or-later
import struct

from amaranth.sim import Simulator

from esdr_fpga.core import COLUMNS, PALETTE, Core
from esdr_fpga.dbi import F_HOLD, F_NOCMD, OP_DBI
from esdr_fpga.model import Panel, spectrum_indices, waterfall_indices
from esdr_fpga.regs import REGS
from esdr_fpga.render import SP_CENTER, SP_ON
from esdr_fpga.waterfall import OP_WFLINE
from spimaster import SPIMaster

PAL = [(i * 0x0123 + 0x4000) & 0xffff for i in range(256)]


def reg(name):
    return REGS[name][0]


def run(bench, hold_timeout=1 << 20):
    dut = Core(hold_timeout=hold_timeout)
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    spi = SPIMaster(dut, 40e6)
    panel = Panel()

    async def tb(ctx):
        ctx.set(dut.lcd_mode, 1)
        await ctx.delay(200e-9)
        await spi.write(ctx, PALETTE, PAL)
        await bench(ctx, spi, panel)

    async def bus(ctx):
        async for clk, rst, wr, rs, d in ctx.tick().sample(dut.lcd_wr, dut.lcd_rs, dut.lcd_d):
            if wr:
                panel.byte(rs, d)

    sim.add_testbench(tb)
    sim.add_process(bus)
    sim.run()
    return panel


async def wait_renders(ctx, spi, n):
    for _ in range(2000):
        if (await spi.read(ctx, reg("RENDERS"), 1))[0] >= n:
            return
        await ctx.delay(5e-6)
    raise AssertionError("no render")


def region(panel, page0, col0, w, h):
    """[y][x] native pixels of a region drawn with MADCTL_FPGA = 0."""
    return [[panel.fb[page0 + x][col0 + y] for x in range(w)] for y in range(h)]


WF = dict(page0=10, col0=20, w=16, h=8, ring=12)


async def setup_wf(ctx, spi):
    await spi.write(ctx, reg("WF_PAGE0"), [WF["page0"], WF["col0"], WF["w"], WF["h"],
                                          WF["ring"], 1])
    await spi.write(ctx, reg("MADCTL_FPGA"), [0])
    await spi.write(ctx, reg("CTRL"), [1])


def test_waterfall():
    lines = [[(n * 7 + x) % 200 for x in range(WF["w"])] for n in range(10)]

    async def bench(ctx, spi, panel):
        await spi.frame(ctx, bytes([OP_DBI, 0, 0x36, 0x28]))
        await setup_wf(ctx, spi)
        for line in lines:
            await spi.frame(ctx, bytes([OP_WFLINE] + line))
        assert await spi.read(ctx, reg("LINES"), 1) == [10]
        await spi.write(ctx, reg("DIRTY"), [1])
        await wait_renders(ctx, spi, 1)

    panel = run(bench)
    want = waterfall_indices(lines, WF["ring"], WF["w"], WF["h"])
    got = region(panel, WF["page0"], WF["col0"], WF["w"], WF["h"])
    assert got == [[PAL[i] for i in row] for row in want]
    assert panel.madctl == 0x28


def test_waterfall_wraps():
    lines = [[(n * 3 + x) % 240 for x in range(WF["w"])] for n in range(30)]

    async def bench(ctx, spi, panel):
        await setup_wf(ctx, spi)
        for line in lines:
            await spi.frame(ctx, bytes([OP_WFLINE] + line))
        await spi.write(ctx, reg("DIRTY"), [1])
        await wait_renders(ctx, spi, 1)

    panel = run(bench)
    want = waterfall_indices(lines, WF["ring"], WF["w"], WF["h"])
    got = region(panel, WF["page0"], WF["col0"], WF["w"], WF["h"])
    assert got == [[PAL[i] for i in row] for row in want]


def test_spectrum():
    w, h = 24, 20
    cols = [(x * 5) % h for x in range(w)]
    flags = SP_ON | SP_CENTER

    async def bench(ctx, spi, panel):
        await spi.write(ctx, reg("SP_PAGE0"), [3, 4, w, h, flags])
        await spi.write(ctx, reg("MADCTL_FPGA"), [0])
        await spi.write(ctx, COLUMNS, cols)
        await spi.write(ctx, reg("CTRL"), [1])
        await spi.write(ctx, reg("COL_COMMIT"), [1])
        await wait_renders(ctx, spi, 1)

    panel = run(bench)
    want = spectrum_indices(cols, w, h, flags)
    got = region(panel, 3, 4, w, h)
    assert got == [[PAL[i] for i in row] for row in want]


def test_host_preempts():
    """Host frames during a render: both land, the host's MADCTL is in force afterwards."""
    lines = [[(n + x) % 100 for x in range(WF["w"])] for n in range(WF["h"])]

    async def bench(ctx, spi, panel):
        await spi.frame(ctx, bytes([OP_DBI, 0, 0x36, 0x00]))
        await setup_wf(ctx, spi)
        for line in lines:
            await spi.frame(ctx, bytes([OP_WFLINE] + line))
        await spi.write(ctx, reg("DIRTY"), [1])
        await ctx.delay(3e-6)
        # A host memory write in three frames, held across them.
        await spi.frame(ctx, bytes([OP_DBI, F_HOLD, 0x2A, 0, 200, 0, 201]))
        await spi.frame(ctx, bytes([OP_DBI, F_HOLD, 0x2B, 1, 0x00, 1, 0x00]))
        await spi.frame(ctx, bytes([OP_DBI, F_HOLD, 0x2C]))
        await spi.frame(ctx, bytes([OP_DBI, F_NOCMD, 0x12, 0x34, 0x56, 0x78]))
        await wait_renders(ctx, spi, 1)
        assert (await spi.read(ctx, reg("STATUS"), 1))[0] & 0x80 == 0

    panel = run(bench)
    want = waterfall_indices(lines, WF["ring"], WF["w"], WF["h"])
    got = region(panel, WF["page0"], WF["col0"], WF["w"], WF["h"])
    assert got == [[PAL[i] for i in row] for row in want]
    assert panel.fb[256][200] == 0x1234 and panel.fb[256][201] == 0x5678
    assert panel.madctl == 0x00


def test_region_shrinks_during_render():
    """The host turns the waterfall off mid-render (fpga.stop()): the render still ends and the
    renderer hands the bus back."""
    lines = [[x % 100 for x in range(WF["w"])] for _ in range(WF["h"])]

    async def bench(ctx, spi, panel):
        await setup_wf(ctx, spi)
        for line in lines:
            await spi.frame(ctx, bytes([OP_WFLINE] + line))
        await spi.write(ctx, reg("DIRTY"), [1])
        await ctx.delay(4e-6)
        await spi.write(ctx, reg("WF_PAGE0"), [0, 0, 0, 0])
        await wait_renders(ctx, spi, 1)
        assert (await spi.read(ctx, reg("STATUS"), 1))[0] & 0x08 == 0

    run(bench)


def stress(seed):
    """Host memory writes (held across frames), waterfall lines, redraws, regions turned on
    and off and the renderer switched, at random, with TE edges: the renderer always gives the
    bus back, and never splits a command."""
    import random

    rng = random.Random(seed)
    dut = Core(hold_timeout=1 << 12)
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    spi = SPIMaster(dut, 40e6)
    log = []
    res = {}

    async def te(ctx):
        while True:
            ctx.set(dut.lcd_fmark, 1)
            await ctx.delay(1e-6)
            ctx.set(dut.lcd_fmark, 0)
            await ctx.delay(rng.uniform(20e-6, 60e-6))

    async def bus(ctx):
        async for clk, rst, wr, rs, d in ctx.tick().sample(dut.lcd_wr, dut.lcd_rs, dut.lcd_d):
            if wr:
                log.append((rs, d))

    async def tb(ctx):
        ctx.set(dut.lcd_mode, 1)
        await ctx.delay(200e-9)
        for _ in range(60):
            a = rng.random()
            if a < 0.3:
                w, h = rng.randint(2, 6), rng.randint(2, 6)
                await spi.frame(ctx, bytes([OP_DBI, F_HOLD, 0x2A, 0, 10, 0, 10 + w - 1]))
                await spi.frame(ctx, bytes([OP_DBI, F_HOLD, 0x2B, 0, 20, 0, 20 + h - 1]))
                await spi.frame(ctx, bytes([OP_DBI, F_HOLD, 0x2C]))
                n, k = 2 * w * h, rng.randint(1, 3)
                for c in range(k):
                    part = n // k if c < k - 1 else n - (k - 1) * (n // k)
                    flags = F_NOCMD | (F_HOLD if c < k - 1 else 0)
                    await spi.frame(ctx, bytes([OP_DBI, flags]) + bytes(part))
            elif a < 0.5:
                await spi.frame(ctx, bytes([OP_WFLINE] + [rng.randrange(200) for _ in range(8)]))
            elif a < 0.6:
                await spi.write(ctx, reg("DIRTY"), [3])
            elif a < 0.7:
                w, h = rng.choice([0, 8]), rng.choice([0, 6])
                await spi.write(ctx, reg("WF_PAGE0"), [5, 5, w, h, 20, 1 if w else 0])
                await spi.write(ctx, reg("SP_PAGE0"), [40, 5, w, h, 3 if w else 0])
            elif a < 0.8:
                await spi.write(ctx, reg("CTRL"), [rng.choice([0, 1, 3, 7, 5])])
            else:
                await ctx.delay(rng.uniform(1e-6, 30e-6))
        await spi.write(ctx, reg("CTRL"), [0])
        for _ in range(200):
            await ctx.delay(10e-6)
            res["status"] = (await spi.read(ctx, reg("STATUS"), 1))[0]
            if not res["status"] & 0x08:
                break

    sim.add_testbench(tb)
    sim.add_process(te)
    sim.add_process(bus)
    sim.run()
    assert not res["status"] & 0x08, "the renderer kept the bus"
    # Every CASET and PASET has its four parameter bytes.
    for i, (rs, d) in enumerate(log):
        if rs == 0 and d in (0x2A, 0x2B):
            assert all(r == 1 for r, _ in log[i + 1:i + 5]), f"split command at {i}"


def test_stress_1():
    stress(1)


def test_stress_3():
    stress(3)
