# SPDX-License-Identifier: GPL-3.0-or-later
"""The link variant: SPI frames into the FIFO, bytes out of the UART pin at their rate."""

from amaranth.sim import Simulator

from esdr_fpga.link import CAPS_LINK, FIFO_WORDS, LINK_REGS, OP_LINK, LinkCore
from esdr_fpga.regs import ID
from spimaster import SPIMaster

CLK = 30e6


def run(bench, divisor=2):
    dut = LinkCore(divisor)
    sim = Simulator(dut)
    sim.add_clock(1 / CLK, domain="sync")
    spi = SPIMaster(dut, 20e6)

    async def tb(ctx):
        ctx.set(dut.ready, 1)
        await ctx.delay(200e-9)
        await bench(ctx, spi, dut)

    sim.add_testbench(tb)
    sim.run()


async def send(ctx, spi, dut, *frames):
    """The frames in the FIFO, the UART held meanwhile (SPI is slower here than on the badge)."""
    ctx.set(dut.ready, 0)
    for f in frames:
        await spi.frame(ctx, f)
    ctx.set(dut.ready, 1)


async def capture(ctx, dut, n, divisor=2, timeout_cycles=200000):
    """@p n bytes from the UART pin, sampled at each bit's middle."""
    out = bytearray()
    cycles = 0
    while len(out) < n and cycles < timeout_cycles:
        await ctx.tick()
        cycles += 1
        if ctx.get(dut.tx) == 0:
            # The start bit: to the middle of bit 0, then a bit at a time.
            for _ in range(divisor + divisor // 2):
                await ctx.tick()
                cycles += 1
            byte = 0
            for b in range(8):
                byte |= ctx.get(dut.tx) << b
                for _ in range(divisor):
                    await ctx.tick()
                    cycles += 1
            assert ctx.get(dut.tx) == 1, "stop bit"
            out.append(byte)
    return bytes(out)


def test_id_and_caps():
    async def bench(ctx, spi, dut):
        assert await spi.read(ctx, LINK_REGS["ID"], 1) == [ID]
        assert await spi.read(ctx, LINK_REGS["CAPS"], 1) == [CAPS_LINK]
        assert await spi.read(ctx, LINK_REGS["LINK_FREE"], 1) == [FIFO_WORDS - 1]
    run(bench)


def test_bytes_come_out_in_order():
    data = bytes(range(1, 41))

    async def bench(ctx, spi, dut):
        await send(ctx, spi, dut, bytes([OP_LINK]) + data)
        got = await capture(ctx, dut, len(data))
        assert got == data
    run(bench)


def test_at_the_slower_divisor_too():
    data = bytes(range(1, 21))

    async def bench(ctx, spi, dut):
        await send(ctx, spi, dut, bytes([OP_LINK]) + data)
        assert await capture(ctx, dut, len(data), divisor=3) == data
    run(bench, divisor=3)


def test_an_odd_frame_ends_with_a_zero():
    async def bench(ctx, spi, dut):
        await send(ctx, spi, dut, bytes([OP_LINK, 1, 2, 3]))
        assert await capture(ctx, dut, 4) == bytes([1, 2, 3, 0])
    run(bench)


def test_frames_follow_each_other():
    async def bench(ctx, spi, dut):
        await send(ctx, spi, dut, bytes([OP_LINK, 1, 2]), bytes([OP_LINK, 3, 4]),
                   bytes([OP_LINK, 5, 6]))
        assert await capture(ctx, dut, 6) == bytes(range(1, 7))
    run(bench)


def test_other_opcodes_stay_out():
    async def bench(ctx, spi, dut):
        ctx.set(dut.ready, 0)
        await spi.frame(ctx, bytes([0x21, 9, 9, 9, 9]))
        await spi.write(ctx, 0x30, [0x1234])
        await send(ctx, spi, dut, bytes([OP_LINK, 7, 8]))
        assert await capture(ctx, dut, 2) == bytes([7, 8])
    run(bench)


def test_not_ready_holds_the_sender():
    async def bench(ctx, spi, dut):
        ctx.set(dut.ready, 0)
        await spi.frame(ctx, bytes([OP_LINK, 0x11, 0x22, 0x33, 0x44]))
        for _ in range(500):
            await ctx.tick()
            assert ctx.get(dut.tx) == 1, "sent while not ready"
        assert await spi.read(ctx, LINK_REGS["LINK_SENT"], 1) == [0]
        assert (await spi.read(ctx, LINK_REGS["LINK_HELD"], 1))[0] > 0
        ctx.set(dut.ready, 1)
        assert await capture(ctx, dut, 4) == bytes([0x11, 0x22, 0x33, 0x44])
        for _ in range(100):
            await ctx.tick()
        assert await spi.read(ctx, LINK_REGS["LINK_SENT"], 1) == [4]
    run(bench)


def test_free_follows_the_fill():
    async def bench(ctx, spi, dut):
        ctx.set(dut.ready, 0)
        await spi.frame(ctx, bytes([OP_LINK]) + bytes(100))
        free = (await spi.read(ctx, LINK_REGS["LINK_FREE"], 1))[0]
        # One word may have moved to the output register.
        assert FIFO_WORDS - 1 - 50 <= free <= FIFO_WORDS - 1 - 49
        assert await spi.read(ctx, LINK_REGS["LINK_LOST"], 1) == [0]
    run(bench)
