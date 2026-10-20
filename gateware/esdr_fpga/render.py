# SPDX-License-Identifier: GPL-3.0-or-later
"""Renderer: the waterfall and spectrum regions, drawn straight to the panel.

The renderer writes with its own MADCTL (MV clear, so a write runs along a native panel row)
and puts the host's back when it is done or yields. Each native row is its own memory write
(CASET, PASET, RAMWR, pixels), so the renderer can hand the bus to the host between any two
rows: when the host's FIFO holds bytes or the host holds the bus for a memory write that spans
frames. A region is described in the renderer's own address space: native row (page)
PAGE0 + x carries the region's column x, native column COL0 + y its row y; the host picks
MADCTL_FPGA so that this is the region's orientation on the screen.

Every pixel is a palette index; the palette's top entries are the chrome colours.
"""

from amaranth.hdl import Cat, Const, Module, Mux, Signal
from amaranth.lib import memory, stream, wiring
from amaranth.lib.wiring import In, Out

from .dbi import MADCTL

__all__ = ["Renderer", "PAL_BG", "PAL_TRACE", "PAL_FILL", "PAL_GRID", "PAL_CENTER",
           "SP_ON", "SP_FILL", "SP_HGRID", "SP_VGRID", "SP_CENTER"]

CASET = 0x2A
PASET = 0x2B
RAMWR = 0x2C

PAL_BG = 248
PAL_TRACE = 249
PAL_FILL = 250
PAL_GRID = 251
PAL_CENTER = 252

# SP_FLAGS
SP_ON = 1 << 0
SP_FILL = 1 << 1
SP_HGRID = 1 << 2
SP_VGRID = 1 << 3
SP_CENTER = 1 << 4


class Renderer(wiring.Component):
    out: Out(stream.Signature(9))   # (rs << 8) | byte, to the panel bus
    own: Out(1)                     # the panel bus is ours

    host_busy: In(1)
    te: In(1)
    enable: In(1)
    te_sync: In(1)
    madctl_fpga: In(8)
    madctl_host: In(8)
    dirty_set: In(2)                # b0 waterfall, b1 spectrum

    wf_on: In(1)
    wf_page0: In(9)
    wf_col0: In(9)
    wf_w: In(9)
    wf_h: In(9)
    wf_ring: In(16)
    wf_head: In(16)

    sp_page0: In(9)
    sp_col0: In(9)
    sp_w: In(9)
    sp_h: In(9)
    sp_flags: In(8)
    sp_grid_y0: In(9)
    sp_grid_dy: In(9)
    sp_grid_dx: In(9)

    # Waterfall ring read port (Waterfall.r_*).
    wf_addr: Out(16)
    wf_req: Out(1)
    wf_ack: In(1)
    wf_data: In(8)

    # Host writes: palette, spectrum columns (the back bank), and the bank swap.
    pal_addr: In(8)
    pal_data: In(16)
    pal_we: In(1)
    col_addr: In(9)
    col_data: In(8)
    col_we: In(1)
    col_commit: In(1)

    renders: Out(16)
    busy_cycles: Out(24)            # duration of the last render

    def elaborate(self, platform):
        m = Module()

        # Memories.
        m.submodules.pal = pal = memory.Memory(shape=16, depth=256, init=[])
        pal_w = pal.write_port()
        pal_r = pal.read_port()
        m.d.comb += [pal_w.addr.eq(self.pal_addr), pal_w.data.eq(self.pal_data),
                     pal_w.en.eq(self.pal_we)]

        m.submodules.cols = cols = memory.Memory(shape=8, depth=1024, init=[])
        col_w = cols.write_port()
        col_r = cols.read_port()
        front = Signal()
        m.d.comb += [col_w.addr.eq(Cat(self.col_addr, ~front)), col_w.data.eq(self.col_data),
                     col_w.en.eq(self.col_we)]

        dirty = Signal(2)
        m.d.sync += dirty.eq(dirty | self.dirty_set)
        with m.If(self.col_commit):
            m.d.sync += [front.eq(~front), dirty[1].eq(1)]

        # Item stream from the generator to the output stage: bit 9 pixel (byte is a palette
        # index), bit 8 rs.
        item = Signal(10)
        item_valid = Signal()
        item_take = Signal()
        with m.If(item_take):
            m.d.sync += item_valid.eq(0)

        def emit(value):
            """Queue an item when the slot is free; returns the condition to advance on."""
            ok = ~item_valid | item_take
            with m.If(ok):
                m.d.sync += [item.eq(value), item_valid.eq(1)]
            return ok

        # Output stage: commands as they are, pixels through the palette as two bytes.
        o_state = Signal(2)     # 0 idle/command, 1 palette read, 2 high byte, 3 low byte
        rgb = Signal(16)
        m.d.comb += pal_r.addr.eq(item[:8])
        with m.Switch(o_state):
            with m.Case(0):
                with m.If(item_valid & ~item[9]):
                    m.d.comb += [self.out.payload.eq(item[:9]), self.out.valid.eq(1)]
                    m.d.comb += item_take.eq(self.out.ready)
                with m.Elif(item_valid & item[9]):
                    m.d.sync += o_state.eq(1)
            with m.Case(1):
                m.d.sync += [rgb.eq(pal_r.data), o_state.eq(2)]
            with m.Case(2):
                m.d.comb += [self.out.payload.eq((1 << 8) | rgb[8:]), self.out.valid.eq(1)]
                with m.If(self.out.ready):
                    m.d.sync += o_state.eq(3)
            with m.Case(3):
                m.d.comb += [self.out.payload.eq((1 << 8) | rgb[:8]), self.out.valid.eq(1)]
                with m.If(self.out.ready):
                    m.d.comb += item_take.eq(1)
                    m.d.sync += o_state.eq(0)
        drained = ~item_valid & (o_state == 0)

        # Generator.
        todo = Signal(2)
        region = Signal()       # 0 waterfall, 1 spectrum
        x = Signal(9)
        y = Signal(9)
        k = Signal(4)           # byte within a command sequence
        head = Signal(16)
        base = Signal(16)       # x * ring
        slot = Signal(16)
        v = Signal(8)
        vp = Signal(8)
        gy = Signal(9)
        gx = Signal(9)
        timer = Signal(24)

        page0 = Mux(region, self.sp_page0, self.wf_page0)
        col0 = Mux(region, self.sp_col0, self.wf_col0)
        width = Mux(region, self.sp_w, self.wf_w)
        height = Mux(region, self.sp_h, self.wf_h)
        page = Signal(9)
        col1 = Signal(9)
        m.d.comb += [page.eq(page0 + x), col1.eq(col0 + height - 1)]

        def command_bytes(seq):
            """Emit a list of (rs, value) items, k counting; returns the 'done' condition."""
            done = Signal(name=f"seq{len(seq)}_done")
            with m.Switch(k):
                for i, (rs, val) in enumerate(seq):
                    with m.Case(i):
                        byte = Const(val, 8) if isinstance(val, int) else val
                        if len(byte) < 8:
                            byte = Cat(byte, Const(0, 8 - len(byte)))
                        with m.If(emit(Cat(byte, Const(rs, 1), Const(0, 1)))):
                            if i == len(seq) - 1:
                                m.d.sync += k.eq(0)
                                m.d.comb += done.eq(1)
                            else:
                                m.d.sync += k.eq(k + 1)
            return done

        m.d.sync += timer.eq(timer + self.own)

        with m.FSM():
            with m.State("IDLE"):
                go = (self.enable & (dirty != 0) & ~self.host_busy & drained
                      & (~self.te_sync | self.te))
                with m.If(go):
                    m.d.sync += [
                        todo.eq(dirty & Cat(self.wf_on, self.sp_flags[0])),
                        dirty.eq(self.dirty_set),
                        head.eq(self.wf_head),
                        self.own.eq(1),
                        timer.eq(0),
                    ]
                    m.next = "ACQUIRE"

            with m.State("ACQUIRE"):
                with m.If(command_bytes([(0, MADCTL), (1, self.madctl_fpga)])):
                    m.next = "NEXT_REGION"

            with m.State("NEXT_REGION"):
                m.d.sync += [x.eq(0), base.eq(0), vp.eq(0)]
                with m.If(todo[0]):
                    m.d.sync += [region.eq(0), todo[0].eq(0)]
                    m.next = "ROW"
                with m.Elif(todo[1]):
                    m.d.sync += [region.eq(1), todo[1].eq(0), gx.eq(0)]
                    m.next = "ROW"
                with m.Else():
                    m.next = "RELEASE"

            with m.State("ROW"):
                # Yield between rows when the host wants the bus.
                with m.If(self.host_busy):
                    m.next = "YIELD"
                with m.Else():
                    with m.If(command_bytes([
                        (0, CASET), (1, col0[8:]), (1, col0[:8]), (1, col1[8:]), (1, col1[:8]),
                        (0, PASET), (1, page[8:]), (1, page[:8]), (1, page[8:]), (1, page[:8]),
                        (0, RAMWR),
                    ])):
                        m.d.sync += [
                            y.eq(0),
                            slot.eq(head),
                            gy.eq(self.sp_grid_y0),
                        ]
                        with m.If(region):
                            m.next = "SP_COL"
                        with m.Else():
                            m.next = "WF_READ"

            # Waterfall: column x, rows y = 0.. from the newest line down.
            with m.State("WF_READ"):
                m.d.comb += [self.wf_addr.eq(base + slot), self.wf_req.eq(1)]
                with m.If(self.wf_ack):
                    m.next = "WF_DATA"
            with m.State("WF_DATA"):
                with m.If(emit(Cat(self.wf_data, Const(1, 1), Const(1, 1)))):
                    m.d.sync += slot.eq(Mux(slot == 0, self.wf_ring - 1, slot - 1))
                    m.next = "PIXEL_NEXT"
                with m.Else():
                    m.next = "WF_HOLD"
            with m.State("WF_HOLD"):
                # The item slot was full: read again (the data is gone after a cycle).
                m.next = "WF_READ"

            # Spectrum: column value, then the pixels of the column.
            with m.State("SP_COL"):
                m.d.comb += col_r.addr.eq(Cat(x, front))
                m.next = "SP_COL_DATA"
            with m.State("SP_COL_DATA"):
                m.d.comb += col_r.addr.eq(Cat(x, front))
                m.d.sync += [v.eq(col_r.data), vp.eq(Mux(x == 0, col_r.data, v))]
                m.next = "SP_PIX"
            with m.State("SP_PIX"):
                lo = Mux(vp < v, vp, v)
                hi = Mux(vp < v, v, vp)
                f = self.sp_flags
                pix = Signal(8)
                with m.If((y >= lo) & (y <= hi)):
                    m.d.comb += pix.eq(PAL_TRACE)
                with m.Elif(f[1] & (y > v)):
                    m.d.comb += pix.eq(PAL_FILL)
                with m.Elif(f[4] & (x == (width >> 1))):
                    m.d.comb += pix.eq(PAL_CENTER)
                with m.Elif((f[2] & (gy == 0)) | (f[3] & (gx == 0))):
                    m.d.comb += pix.eq(PAL_GRID)
                with m.Else():
                    m.d.comb += pix.eq(PAL_BG)
                with m.If(emit(Cat(pix, Const(1, 1), Const(1, 1)))):
                    m.d.sync += gy.eq(Mux(gy == 0, self.sp_grid_dy - 1, gy - 1))
                    m.next = "PIXEL_NEXT"

            with m.State("PIXEL_NEXT"):
                with m.If(y + 1 == height):
                    m.d.sync += [
                        x.eq(x + 1),
                        base.eq(base + self.wf_ring),
                        gx.eq(Mux(gx == 0, self.sp_grid_dx - 1, gx - 1)),
                    ]
                    with m.If(x + 1 == width):
                        m.next = "NEXT_REGION"
                    with m.Else():
                        m.next = "ROW"
                with m.Else():
                    m.d.sync += y.eq(y + 1)
                    with m.If(region):
                        m.next = "SP_PIX"
                    with m.Else():
                        m.next = "WF_READ"

            with m.State("YIELD"):
                with m.If(command_bytes([(0, MADCTL), (1, self.madctl_host)])):
                    m.next = "YIELD_DRAIN"
            with m.State("YIELD_DRAIN"):
                with m.If(drained):
                    m.d.sync += self.own.eq(0)
                    m.next = "YIELD_WAIT"
            with m.State("YIELD_WAIT"):
                with m.If(~self.host_busy):
                    m.d.sync += self.own.eq(1)
                    m.next = "REACQUIRE"
            with m.State("REACQUIRE"):
                with m.If(command_bytes([(0, MADCTL), (1, self.madctl_fpga)])):
                    m.next = "ROW"

            with m.State("RELEASE"):
                with m.If(command_bytes([(0, MADCTL), (1, self.madctl_host)])):
                    m.next = "RELEASE_DRAIN"
            with m.State("RELEASE_DRAIN"):
                with m.If(drained):
                    m.d.sync += [
                        self.own.eq(0),
                        self.renders.eq(self.renders + 1),
                        self.busy_cycles.eq(timer),
                    ]
                    m.next = "IDLE"
        return m
