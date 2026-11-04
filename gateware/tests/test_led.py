# SPDX-License-Identifier: GPL-3.0-or-later
"""The LED's hard PWM: what its control bus gets. The values come from Lattice TN1288, appendix D,
worked out by hand here, not from the module's formulas."""

import pytest
from amaranth.sim import Simulator

from esdr_fpga.core import Core
from esdr_fpga.link import LINK_REGS, LinkCore
from esdr_fpga.prims import LED_RUN, LedBus, LedFade, led_colour, led_setup
from esdr_fpga.regs import REGS
from spimaster import SPIMaster

CR0, BR, ONR, OFR, BCRR, BCFR, PWRR, PWRG, PWRB = (0b1000, 0b1001, 0b1010, 0b1011, 0b0101, 0b0110,
                                                   0b0001, 0b0010, 0b0011)


def word(addr, data):
    return addr << 8 | data


def test_setup_at_30_mhz():
    # 30 MHz / 64 kHz - 1 = 467.75: 468, 0x1D4. Enabled, 250 Hz, the prescaler's high bits 01.
    # Ramps of 1.024 s: rate 7, both alike, every colour in that time. Holds of 32 ms.
    assert led_setup(30e6) == [word(CR0, 0b11000001), word(BR, 0xD4), word(ONR, 1), word(OFR, 1),
                               word(BCRR, 0b11100111), word(BCFR, 0b10100111)]


def test_prescaler_at_12_mhz():
    # 12 MHz / 64 kHz - 1 = 186.5, rounding to even: 186, no high bits.
    setup = led_setup(12e6)
    assert setup[0] == word(CR0, 0b11000000)
    assert setup[1] == word(BR, 186)


def test_ramp():
    setup = led_setup(30e6, ramp_ms=512)
    assert setup[4] & 0x0F == 3
    assert setup[5] & 0x0F == 3


def test_colour():
    assert led_colour(255, 16, 1) == [word(PWRR, 255), word(PWRG, 16), word(PWRB, 1)]


@pytest.mark.parametrize("kwargs", [
    {"sys_clk_freq": 3.9e6},
    {"sys_clk_freq": 65e6},
    # A rate of 0 means no ramp at all.
    {"ramp_ms": 128},
    {"ramp_ms": 2176},
])
def test_refuses_what_the_block_cannot_do(kwargs):
    with pytest.raises(ValueError):
        led_setup(**kwargs)


def test_fade_writes_once_and_runs_at_the_end():
    fade = LedFade(30e6, colour=(1, 2, 3))
    seen = []

    async def tb(ctx):
        for _ in range(30):
            await ctx.delay(0)
            if ctx.get(fade.stb):
                seen.append(ctx.get(fade.value))
            await ctx.tick()

    sim = Simulator(fade)
    sim.add_clock(1 / 30e6, domain="sync")
    sim.add_testbench(tb)
    sim.run()
    assert seen == led_setup(30e6) + [word(PWRR, 1) | LED_RUN, word(PWRG, 2) | LED_RUN,
                                      word(PWRB, 3) | LED_RUN]


def test_bus_runs_from_the_write_that_says_so():
    dut = LedBus()

    async def tb(ctx):
        was = ctx.get(dut.run)
        assert was == 0
        for value, want in ((word(CR0, 1), 0), (word(PWRR, 1) | LED_RUN, 1), (word(PWRG, 1), 0)):
            ctx.set(dut.value, value)
            await ctx.tick()
            assert ctx.get(dut.run) == was, "only a strobe takes it"
            was = want
            ctx.set(dut.stb, 1)
            await ctx.tick()
            ctx.set(dut.stb, 0)
            await ctx.delay(0)
            assert ctx.get(dut.run) == want

    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    sim.add_testbench(tb)
    sim.run()


@pytest.mark.parametrize("make, addr", [(Core, REGS["LED"][0]), (LinkCore, LINK_REGS["LED"])])
def test_both_bitstreams_pass_the_register_on(make, addr):
    dut = make()
    spi = SPIMaster(dut, 20e6)
    words = [word(CR0, 0xC1), word(PWRB, 24) | LED_RUN]
    seen = []

    async def watch(ctx):
        async for _, _, stb, value in ctx.tick().sample(dut.led_stb, dut.led):
            if stb:
                seen.append(value)

    async def tb(ctx):
        await ctx.delay(200e-9)
        for w in words:
            await spi.write(ctx, addr, [w])
        await ctx.delay(2e-6)

    sim = Simulator(dut)
    sim.add_clock(1 / 30e6, domain="sync")
    sim.add_process(watch)
    sim.add_testbench(tb)
    sim.run()
    assert seen == words
