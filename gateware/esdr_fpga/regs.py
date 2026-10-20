# SPDX-License-Identifier: GPL-3.0-or-later
"""Registers. REGS is the single source for the gateware, the C header and the docs."""

from amaranth.hdl import Module, Signal
from amaranth.lib import wiring
from amaranth.lib.wiring import In

from . import bus

__all__ = ["REGS", "ID", "VERSION", "Registers"]

ID = 0xE5DF
VERSION = (0 << 8) | 2

# name: (address, access, width, reset, description). "c": constant, "r": status (driven by
# the gateware), "w": control (write-only: the host keeps its own copy), "rw": control read
# back, "s": strobe (a write pulses it with the value). Reads of "w" and "s" give 0.
REGS = {
    "ID":          (0x00, "c",  16, ID, "0xE5DF"),
    "VERSION":     (0x01, "c",  16, VERSION, "gateware version, major in bits 15:8"),
    "BUILD":       (0x02, "c",  16, 0, "build hash, low 16 bits"),
    "CAPS":        (0x03, "c",  16, 0, "features of this variant"),
    "STATUS":      (0x04, "r",  16, 0, "b0 lcd_mode (FPGA owns the panel bus), b1 DBI FIFO "
                                        "not empty, b2 host HOLD, b3 renderer owns the bus, "
                                        "b7 DBI FIFO overflowed (sticky)"),
    "SCRATCH":     (0x07, "rw", 16, 0, "free for host tests"),
    "IN_FREE":     (0x08, "r",  16, 0, "DSP: free complex samples in the input ring"),
    "FRAMES":      (0x0A, "r",  16, 0, "DSP: FFT frames done"),
    "LINES":       (0x0B, "r",  16, 0, "waterfall lines written"),
    "TE_CNT":      (0x0C, "r",  16, 0, "rising edges of the panel's FMARK (TE)"),
    "DBI_BYTES":   (0x0D, "r",  16, 0, "bytes written to the panel bus, low 16 bits"),
    "OVERRUNS":    (0x0E, "r",  16, 0, "DSP: times the input ring was about to overflow (the "
                                        "frame skipped ahead)"),
    "DROPPED":     (0x0F, "r",  16, 0, "DSP: samples dropped because the input ring was full"),
    "RENDERS":     (0x10, "r",  16, 0, "renders completed"),
    "R_TIME":      (0x11, "r",  16, 0, "last render's duration, 256-cycle units"),
    "CTRL":        (0x18, "w",   3, 0, "b0 renderer on, b1 start renders on TE, b2 a new "
                                        "waterfall line marks the waterfall dirty"),
    "DIRTY":       (0x19, "s",   2, 0, "write 1 to redraw: b0 waterfall, b1 spectrum"),
    "COL_COMMIT":  (0x1A, "s",   1, 0, "write: the spectrum columns written so far become the "
                                        "front bank (marks the spectrum dirty)"),
    "WF_PAGE0":    (0x28, "w",   9, 0, "waterfall: first native row (its column 0)"),
    "WF_COL0":     (0x29, "w",   9, 0, "waterfall: first native column (its row 0)"),
    "WF_W":        (0x2A, "w",   9, 0, "waterfall: columns (line length)"),
    "WF_H":        (0x2B, "w",   9, 0, "waterfall: rows on screen"),
    "WF_RING":     (0x2C, "w",  16, 1, "waterfall: lines in the ring, WF_W * WF_RING <= 65536"),
    "WF_FLAGS":    (0x2D, "w",   1, 0, "waterfall: b0 on"),
    "WF_HEAD":     (0x2E, "r",  16, 0, "waterfall: slot of the newest line"),
    "SP_PAGE0":    (0x30, "w",   9, 0, "spectrum: first native row (its column 0)"),
    "SP_COL0":     (0x31, "w",   9, 0, "spectrum: first native column (its row 0)"),
    "SP_W":        (0x32, "w",   9, 0, "spectrum: columns"),
    "SP_H":        (0x33, "w",   9, 0, "spectrum: rows"),
    "SP_FLAGS":    (0x34, "w",   2, 0, "spectrum: b0 on, b1 centre line"),
    "MADCTL_FPGA": (0x40, "w",   8, 0, "MADCTL the renderer writes with (MV clear)"),
    "MADCTL_HOST": (0x41, "r",   8, 0, "last MADCTL (0x36) the host passed through"),
    "DSP_CTRL":    (0x50, "w",   3, 0, "DSP: b0 on, b1 average, b2 frames overlap by half"),
    "FFT_CFG":     (0x51, "w",  16, 0, "DSP: bits 3:0 log2 N (4..9), bits 15:8 frames per "
                                        "line (0: lines only on EMIT)"),
    "L_OFFSET":    (0x52, "w",  16, 0, "DSP: added to 256 log2 |X|^2 (s16, L units)"),
    "AVG_C_HI":    (0x53, "w",  16, 0, "DSP: average step for a jump of 8 log2 or more "
                                        "(256 log2 alpha)"),
    "AVG_C_LO":    (0x54, "w",  16, 0, "DSP: average step for a drop beyond 8 log2 "
                                        "(256 log2 (1 - alpha))"),
    "COLS":        (0x55, "w",   9, 0, "DSP: columns per line (the regions' width)"),
    "ZOOM_START":  (0x56, "w",  10, 0, "DSP: first bin (fftshifted) of column 0"),
    "ZOOM_STEP":   (0x57, "w",  16, 0, "DSP: bins per column, 8.8"),
    "WF_LO":       (0x59, "w",  16, 0, "DSP: L of waterfall index 0 (s16)"),
    "WF_GAIN":     (0x5A, "w",  16, 0, "DSP: waterfall index per L, 8.8 (below 128)"),
    "TR_TOP":      (0x5B, "w",  16, 0, "DSP: L of the trace's top row (s16)"),
    "TR_GAIN":     (0x5C, "w",  16, 0, "DSP: trace rows per L, 8.8 (below 128)"),
    "AVG_RESET":   (0x5D, "s",   1, 0, "DSP: the next frame restarts the average"),
    "EMIT":        (0x5E, "s",   1, 0, "DSP: a line after the next frame"),
}


class Registers(wiring.Component):
    """Window 0x0000..0x00ff. `status[name]` are inputs for "r" registers, `control[name]`
    outputs for "rw" ones; `consts` overrides constant values (BUILD, CAPS)."""

    bus: In(bus.Signature(8))

    def __init__(self, consts=None):
        self.consts = {name: v[3] for name, v in REGS.items() if v[1] == "c"}
        self.consts.update(consts or {})
        self.status = {name: Signal(v[2], name=name.lower())
                       for name, v in REGS.items() if v[1] == "r"}
        self.control = {name: Signal(v[2], name=name.lower(), init=v[3])
                        for name, v in REGS.items() if v[1] in ("w", "rw")}
        self.strobe = {name: (Signal(v[2], name=name.lower()), Signal(name=name.lower() + "_stb"))
                       for name, v in REGS.items() if v[1] == "s"}
        super().__init__()

    def elaborate(self, platform):
        m = Module()
        for value, stb in self.strobe.values():
            m.d.sync += [stb.eq(0), value.eq(self.bus.w_data)]
        with m.If(self.bus.w_stb):
            with m.Switch(self.bus.addr):
                for name, sig in self.control.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += sig.eq(self.bus.w_data)
                for name, (value, stb) in self.strobe.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += stb.eq(1)
        with m.If(self.bus.r_stb):
            with m.Switch(self.bus.addr):
                for name, value in self.consts.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += self.bus.r_data.eq(value & 0xffff)
                readable = {n: sig for n, sig in self.control.items() if REGS[n][1] == "rw"}
                for name, sig in {**self.status, **readable}.items():
                    with m.Case(REGS[name][0]):
                        m.d.sync += self.bus.r_data.eq(sig)
                with m.Default():
                    m.d.sync += self.bus.r_data.eq(0)
        return m


def header():
    """include/esp_sdr/esdr_fpga_regs.h: opcodes, memory windows and registers."""
    from .core import AVG_LUT, COLUMNS, COLV, PALETTE, SCRATCH_RAM, WINDOW
    from .dsp import FMT_IQ8, FMT_IQ10P, FMT_IQ16, FMT_WORD32, F_RESYNC, OP_SAMPLES
    from .dbi import F_HOLD, F_NOCMD, OP_DBI
    from .proto import OP_READ, OP_WRITE
    from .render import PAL_BG, PAL_CENTER, PAL_TRACE, SP_CENTER, SP_ON
    from .waterfall import OP_WFLINE

    def define(name, value, comment=None):
        line = f"#define {name:<28} 0x{value:04X}U"
        return line + (f" /* {comment} */" if comment else "")

    out = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "",
        "/* Generated by gateware/esdr_fpga/regs.py (python -m esdr_fpga.regs): do not edit. */",
        "",
        "#ifndef ESDR_FPGA_REGS_H_",
        "#define ESDR_FPGA_REGS_H_",
        "",
        "/* Opcodes: the first byte of an SPI frame. */",
        define("ESDR_FPGA_OP_WRITE", OP_WRITE),
        define("ESDR_FPGA_OP_READ", OP_READ),
        define("ESDR_FPGA_OP_SAMPLES", OP_SAMPLES),
        define("ESDR_FPGA_OP_DBI", OP_DBI),
        define("ESDR_FPGA_OP_WFLINE", OP_WFLINE),
        define("ESDR_FPGA_DBI_HOLD", F_HOLD),
        define("ESDR_FPGA_DBI_NOCMD", F_NOCMD),
        define("ESDR_FPGA_FMT_WORD32", FMT_WORD32),
        define("ESDR_FPGA_FMT_IQ10P", FMT_IQ10P),
        define("ESDR_FPGA_FMT_IQ8", FMT_IQ8),
        define("ESDR_FPGA_FMT_IQ16", FMT_IQ16),
        define("ESDR_FPGA_SAMPLES_RESYNC", F_RESYNC),
        "",
        "/* Memory windows (16-bit word addresses). */",
        define("ESDR_FPGA_SCRATCH_RAM", SCRATCH_RAM),
        define("ESDR_FPGA_PALETTE", PALETTE),
        define("ESDR_FPGA_COLUMNS", COLUMNS),
        define("ESDR_FPGA_WINDOW", WINDOW),
        define("ESDR_FPGA_AVG_LUT", AVG_LUT),
        define("ESDR_FPGA_COLV", COLV),
        "",
        "/* Palette entries of the renderer's chrome. */",
        define("ESDR_FPGA_PAL_BG", PAL_BG),
        define("ESDR_FPGA_PAL_TRACE", PAL_TRACE),
        define("ESDR_FPGA_PAL_CENTER", PAL_CENTER),
        "",
        "/* SP_FLAGS */",
        define("ESDR_FPGA_SP_ON", SP_ON),
        define("ESDR_FPGA_SP_CENTER", SP_CENTER),
        "",
        "/* Registers. */",
        define("ESDR_FPGA_ID", ID),
    ]
    import textwrap
    for name, (addr, access, width, reset, desc) in REGS.items():
        text = textwrap.wrap(f"{access}, {width} bits: {desc}", 92)
        out.append("/* " + "\n * ".join(text) + " */")
        out.append(define(f"ESDR_FPGA_REG_{name}", addr))
    out += ["", "#endif /* ESDR_FPGA_REGS_H_ */", ""]
    return "\n".join(out)


if __name__ == "__main__":
    print(header(), end="")
