# SPDX-License-Identifier: GPL-3.0-or-later
"""iCE40 primitives behind small wrappers."""

from amaranth.hdl import Cat, ClockSignal, Elaboratable, Instance, Module, Signal

__all__ = ["LED_RUN", "LedBus", "LedFade", "RgbDriver", "Spram", "led_colour", "led_setup"]

# Register LED's word: bits 11:8 a register of SB_LEDDA_IP, the UP5K's hard PWM (Lattice TN1288,
# appendix D), 7:0 its value. LED_RUN, with any write, sets whether the block runs (LEDDEXE).
LED_RUN = 1 << 12

_LEDD_CR0 = 0b1000
_LEDD_BR = 0b1001
_LEDD_ONR = 0b1010
_LEDD_OFR = 0b1011
_LEDD_BCRR = 0b0101
_LEDD_BCFR = 0b0110
_LEDD_PWRR = 0b0001
_LEDD_PWRG = 0b0010
_LEDD_PWRB = 0b0011

# CR0: on, 250 Hz flick rate; its low two bits are the prescaler's high ones.
_CR0_EN = 0x80
_CR0_FR250 = 0x40
# BCRR, BCFR: breathe on, the first for both ramps, every colour taking the same time.
_BC_EN = 0x80
_BC_BOTH = 0x40
_BC_MODULATE = 0x20
# Each hold, in the block's steps of 32 ms: the shortest, so that it fades without a plateau.
_HOLD = 1


def _word(addr, data):
    return addr << 8 | data


def led_setup(sys_clk_freq=30e6, ramp_ms=1024):
    """The words that make the block breathe, the colour aside: ramps of ramp_ms, in steps of
    128 ms. Its timing comes from a 64 kHz tick, from clocks of 4 to 64 MHz."""
    if not 4e6 <= sys_clk_freq <= 64e6:
        raise ValueError(f"no LED driver at {sys_clk_freq} Hz")
    rate = round(ramp_ms / 128) - 1
    if not 1 <= rate <= 15:
        raise ValueError(f"no ramp of {ramp_ms} ms")
    prescale = round(sys_clk_freq / 64e3 - 1)
    breathe = _BC_EN | _BC_MODULATE | rate
    return [
        _word(_LEDD_CR0, _CR0_EN | _CR0_FR250 | prescale >> 8),
        _word(_LEDD_BR, prescale & 0xFF),
        _word(_LEDD_ONR, _HOLD),
        _word(_LEDD_OFR, _HOLD),
        _word(_LEDD_BCRR, breathe | _BC_BOTH),
        _word(_LEDD_BCFR, breathe),
    ]


def led_colour(r, g, b):
    """The words that set each colour's peak, in 256ths of full."""
    return [_word(_LEDD_PWRR, r), _word(_LEDD_PWRG, g), _word(_LEDD_PWRB, b)]


class RgbDriver(Elaboratable):
    """SB_RGBA_DRV at its lowest current (2 mA half-current mode); r, g, b are PWM inputs."""

    def __init__(self):
        self.r = Signal()
        self.g = Signal()
        self.b = Signal()

    def elaborate(self, platform):
        m = Module()
        if platform is None:
            return m
        pins = platform.request("rgb", 0, dir={"r": "-", "g": "-", "b": "-"})
        m.submodules.drv = Instance(
            "SB_RGBA_DRV",
            p_CURRENT_MODE="0b1",
            p_RGB0_CURRENT="0b000001",
            p_RGB1_CURRENT="0b000001",
            p_RGB2_CURRENT="0b000001",
            i_CURREN=1, i_RGBLEDEN=1,
            i_RGB0PWM=self.g, i_RGB1PWM=self.r, i_RGB2PWM=self.b,
            io_RGB0=pins.g.io, io_RGB1=pins.r.io, io_RGB2=pins.b.io,
        )
        return m


class LedBus(Elaboratable):
    """The LED: SB_LEDDA_IP into RgbDriver, written a word at a time (LED_RUN and led_setup())."""

    def __init__(self):
        self.stb = Signal()
        self.value = Signal(13)
        self.run = Signal()

    def elaborate(self, platform):
        m = Module()
        with m.If(self.stb):
            m.d.sync += self.run.eq(self.value[12])
        m.submodules.rgb = rgb = RgbDriver()
        if platform is None:
            return m
        m.submodules.ledda = Instance(
            "SB_LEDDA_IP",
            i_LEDDCLK=ClockSignal("sync"),
            i_LEDDCS=self.stb, i_LEDDDEN=self.stb,
            i_LEDDADDR3=self.value[11], i_LEDDADDR2=self.value[10],
            i_LEDDADDR1=self.value[9], i_LEDDADDR0=self.value[8],
            i_LEDDDAT7=self.value[7], i_LEDDDAT6=self.value[6],
            i_LEDDDAT5=self.value[5], i_LEDDDAT4=self.value[4],
            i_LEDDDAT3=self.value[3], i_LEDDDAT2=self.value[2],
            i_LEDDDAT1=self.value[1], i_LEDDDAT0=self.value[0],
            i_LEDDEXE=self.run,
            o_PWMOUT0=rgb.r, o_PWMOUT1=rgb.g, o_PWMOUT2=rgb.b,
        )
        return m


class LedFade(Elaboratable):
    """A breathing LED with no host to set it up: writes a LedBus once after reset."""

    def __init__(self, sys_clk_freq=30e6, colour=(0, 32, 0)):
        self.words = led_setup(sys_clk_freq) + [w | LED_RUN for w in led_colour(*colour)]
        self.stb = Signal()
        self.value = Signal(13)

    def elaborate(self, platform):
        m = Module()
        step = Signal(range(len(self.words) + 1))
        m.d.comb += self.stb.eq(step != len(self.words))
        with m.If(self.stb):
            m.d.sync += step.eq(step + 1)
        with m.Switch(step):
            for i, word in enumerate(self.words):
                with m.Case(i):
                    m.d.comb += self.value.eq(word)
        return m


class Spram(Elaboratable):
    """SB_SPRAM256KA banks as one 16-bit RAM: `banks` x 16K words, one access per cycle, read
    data the cycle after. mask: one bit per byte (the primitive's nibble mask pairs)."""

    def __init__(self, banks=1):
        self.banks = banks
        self.addr = Signal(14 + max(banks - 1, 0).bit_length())
        self.w_data = Signal(16)
        self.mask = Signal(2, init=0b11)
        self.we = Signal()
        self.r_data = Signal(16)

    def elaborate(self, platform):
        m = Module()
        if platform is None:
            # Simulation model.
            from amaranth.lib import memory
            m.submodules.mem = mem = memory.Memory(shape=16, depth=16384 * self.banks, init=[])
            w = mem.write_port(granularity=8)
            r = mem.read_port()
            m.d.comb += [
                w.addr.eq(self.addr),
                w.data.eq(self.w_data),
                w.en.eq(self.mask.replicate(1) & self.we.replicate(2)),
                r.addr.eq(self.addr),
                self.r_data.eq(r.data),
            ]
            return m

        sel = Signal(range(self.banks))
        if self.banks > 1:
            m.d.sync += sel.eq(self.addr[14:])
        outs = []
        for i in range(self.banks):
            out = Signal(16, name=f"out{i}")
            cs = (self.addr[14:] == i) if self.banks > 1 else 1
            m.submodules[f"spram{i}"] = Instance(
                "SB_SPRAM256KA",
                i_ADDRESS=self.addr[:14],
                i_DATAIN=self.w_data,
                i_MASKWREN=Cat(self.mask[0], self.mask[0], self.mask[1], self.mask[1]),
                i_WREN=self.we,
                i_CHIPSELECT=cs,
                i_CLOCK=ClockSignal("sync"),
                i_STANDBY=0, i_SLEEP=0, i_POWEROFF=1,
                o_DATAOUT=out,
            )
            outs.append(out)
        with m.Switch(sel):
            for i, out in enumerate(outs):
                with m.Case(i):
                    m.d.comb += self.r_data.eq(out)
        return m
