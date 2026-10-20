# SPDX-License-Identifier: GPL-3.0-or-later
import math
import random
import struct

from amaranth.sim import Simulator

from esdr_fpga import model
from esdr_fpga.dsp import DSP, FMT_IQ8, FMT_IQ10P, FMT_IQ16, FMT_WORD32, OP_SAMPLES

N_BITS = 6
N = 1 << N_BITS
COLS = 16
L_OFFSET = -4000


def tone(n, f, amp, rng, noise=200):
    return [(round(amp * math.cos(model.TAU * f * k / n)) + rng.randint(-noise, noise),
             round(amp * math.sin(model.TAU * f * k / n)) + rng.randint(-noise, noise))
            for k in range(n)]


def pack(samples, fmt):
    out = bytearray([OP_SAMPLES, fmt])
    if fmt == FMT_IQ16:
        for i, q in samples:
            out += struct.pack("<hh", i, q)
    elif fmt == FMT_IQ8:
        for i, q in samples:
            out += struct.pack("<bb", i >> 8, q >> 8)
    else:
        words = [((i >> 6) & 0x3ff) << 10 | ((q >> 6) & 0x3ff) for i, q in samples]
        if fmt == FMT_WORD32:
            for w in words:
                out += struct.pack("<I", w)
        else:
            bits = 0
            for j, w in enumerate(words):
                bits |= w << (20 * j)
            out += bits.to_bytes((20 * len(words) + 7) // 8, "little")
    return bytes(out)


def quantise(samples, fmt):
    """What the gateware stores for these samples in this format."""
    if fmt == FMT_IQ16:
        return samples
    if fmt == FMT_IQ8:
        return [((i >> 8) << 8, (q >> 8) << 8) for i, q in samples]
    return [((i >> 6) << 6, (q >> 6) << 6) for i, q in samples]


def run(frames, fmt=FMT_IQ16, alpha=1.0, step=(N // COLS) << 8, start=0,
        wf_lo=-2000, wf_gain=64, tr_top=6000, tr_gain=8, tr_rows=40):
    dut = DSP()
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    win = model.window_q15(5, N)
    tab, c_hi, c_lo = model.avg_table(alpha)
    out = {"line": [], "rows": {}}

    async def stream(ctx, data):
        for i, byte in enumerate(data):
            ctx.set(dut.s_data, byte)
            ctx.set(dut.s_first, i == 0)
            ctx.set(dut.s_stb, 1)
            await ctx.tick()
            ctx.set(dut.s_stb, 0)
            ctx.set(dut.s_first, 0)
            await ctx.tick().repeat(5)
        ctx.set(dut.s_end, 1)
        await ctx.tick()
        ctx.set(dut.s_end, 0)

    async def tb(ctx):
        for a, v in enumerate(win):
            ctx.set(dut.win_addr, a)
            ctx.set(dut.win_data, v)
            ctx.set(dut.win_we, 1)
            await ctx.tick()
        ctx.set(dut.win_we, 0)
        for a, v in enumerate(tab):
            ctx.set(dut.lut_addr, a)
            ctx.set(dut.lut_data, v & 0xffff)
            ctx.set(dut.lut_we, 1)
            await ctx.tick()
        ctx.set(dut.lut_we, 0)
        for sig, v in ((dut.log2n, N_BITS), (dut.fpl, len(frames)), (dut.l_offset, L_OFFSET),
                       (dut.c_hi, c_hi), (dut.c_lo, max(c_lo, -32768)), (dut.cols, COLS),
                       (dut.zoom_start, start), (dut.zoom_step, step),
                       (dut.wf_lo, wf_lo), (dut.wf_gain, wf_gain), (dut.tr_top, tr_top),
                       (dut.tr_gain, tr_gain), (dut.tr_rows, tr_rows),
                       (dut.avg_on, alpha < 1), (dut.on, 1)):
            ctx.set(sig, v)
        for f in frames:
            await stream(ctx, pack(f, fmt))
        for _ in range(100000):
            await ctx.tick()
            if ctx.get(dut.l_end):
                break
        else:
            raise AssertionError("no line")
        cols = []
        for c in range(COLS):
            ctx.set(dut.colv_addr, c)
            await ctx.tick()
            cols.append(ctx.get(dut.colv_data))
        out["colv"] = [v - 0x10000 if v & 0x8000 else v for v in cols]
        out["frames"] = ctx.get(dut.frames)

    async def watch(ctx):
        async for clk, rst, stb, first, data, we, ca, cd in ctx.tick().sample(
                dut.l_stb, dut.l_first, dut.l_data, dut.col_we, dut.col_addr, dut.col_data):
            if stb and not first:
                out["line"].append(data)
            if we:
                out["rows"][ca] = cd

    sim.add_testbench(tb)
    sim.add_process(watch)
    sim.run()

    # The model.
    s = None
    for f in frames:
        l = model.spectrum_frame(quantise(f, fmt), win, L_OFFSET)
        s = l if s is None else [model.average(a, b, tab, c_hi, c_lo) for a, b in zip(s, l)]
    want_cols = model.columns(s, COLS, start, step)
    want_line = [model.scale(v, wf_lo, wf_gain, model.WF_MAX) for v in want_cols]
    want_rows = [model.scale(tr_top - v + 0, 0, tr_gain, tr_rows - 1) for v in want_cols]
    return out, want_cols, want_line, want_rows


def check(frames, **kw):
    out, cols, line, rows = run(frames, **kw)
    assert out["colv"] == cols
    assert out["line"] == line
    assert [out["rows"][c] for c in range(COLS)] == rows
    return out


def test_one_frame_iq16():
    rng = random.Random(6)
    check([tone(N, 9.3, 12000, rng)])


def test_formats():
    rng = random.Random(7)
    for fmt in (FMT_WORD32, FMT_IQ10P, FMT_IQ8):
        check([tone(N, -13.0, 9000, rng)], fmt=fmt)


def test_average_three_frames():
    rng = random.Random(8)
    check([tone(N, 5, 8000, rng), tone(N, 5, 2000, rng), tone(N, 20, 8000, rng)], alpha=0.3)


def test_zoom_in():
    rng = random.Random(9)
    check([tone(N, 3.5, 10000, rng)], start=24, step=1 << 7)



def test_full_ring_drops():
    """Samples faster than the DSP reads them: the reader skips ahead between frames, the
    full ring refuses samples during one, and nothing overwrites the frame being read."""
    from esdr_fpga.dsp import RING
    dut = DSP()
    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")

    async def tb(ctx):
        for sig, v in ((dut.log2n, 9), (dut.cols, 16), (dut.zoom_step, 32 << 8),
                       (dut.tr_rows, 1), (dut.on, 1)):
            ctx.set(sig, v)
        n = RING + 6000
        ctx.set(dut.s_stb, 1)
        ctx.set(dut.s_first, 1)
        ctx.set(dut.s_data, OP_SAMPLES)
        await ctx.tick()
        ctx.set(dut.s_first, 0)
        ctx.set(dut.s_data, FMT_IQ8)
        await ctx.tick()
        ctx.set(dut.s_data, 0)
        for _ in range(2 * n):              # a byte every 6 cycles: SPI at 40 MHz
            ctx.set(dut.s_stb, 1)
            await ctx.tick()
            ctx.set(dut.s_stb, 0)
            await ctx.tick().repeat(5)
        await ctx.tick().repeat(4)
        dropped = ctx.get(dut.dropped)
        frames = ctx.get(dut.frames)
        overruns = ctx.get(dut.overruns)
        # The reader skipped ahead or the writer refused: never both counting a sample.
        assert dropped + overruns > 0
        waiting = RING - 1 - ctx.get(dut.in_free)
        assert frames * 512 + waiting + dropped <= n

    sim.add_testbench(tb)
    sim.run()
