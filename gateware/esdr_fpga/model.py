# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference models: what the gateware must produce, in plain Python."""

from .render import (PAL_BG, PAL_CENTER, PAL_FILL, PAL_GRID, PAL_TRACE, SP_CENTER, SP_FILL,
                     SP_HGRID, SP_VGRID)

__all__ = ["Panel", "waterfall_indices", "spectrum_indices"]


class Panel:
    """ILI9341 memory writes: MADCTL (MY, MX, MV), CASET, PASET, RAMWR, 0x3C; 240 x 320
    native pixels, RGB565 from two bytes, high first."""

    W, H = 240, 320

    def __init__(self):
        self.fb = [[None] * self.W for _ in range(self.H)]
        self.madctl = 0
        self.cmd = None
        self.args = []
        self.c0 = self.c1 = self.p0 = self.p1 = 0
        self.c = self.p = 0
        self.hi = None

    def byte(self, rs, b):
        if not rs:
            self.cmd = b
            self.args = []
            self.hi = None
            if b == 0x2C:
                self.c, self.p = self.c0, self.p0
            return
        if self.cmd in (0x2A, 0x2B, 0x36):
            self.args.append(b)
            if self.cmd == 0x36:
                self.madctl = b
            elif len(self.args) == 4:
                lo, hi = (self.args[0] << 8) | self.args[1], (self.args[2] << 8) | self.args[3]
                if self.cmd == 0x2A:
                    self.c0, self.c1 = lo, hi
                else:
                    self.p0, self.p1 = lo, hi
        elif self.cmd in (0x2C, 0x3C):
            if self.hi is None:
                self.hi = b
                return
            self.put(self.c, self.p, (self.hi << 8) | b)
            self.hi = None
            self.c += 1
            if self.c > self.c1:
                self.c = self.c0
                self.p += 1

    def put(self, c, p, v):
        mv, mx, my = self.madctl & 0x20, self.madctl & 0x40, self.madctl & 0x80
        pc, pr = (p, c) if mv else (c, p)
        if mx:
            pc = self.W - 1 - pc
        if my:
            pr = self.H - 1 - pr
        if 0 <= pc < self.W and 0 <= pr < self.H:
            self.fb[pr][pc] = v


def waterfall_indices(lines, ring, w, h, initial=0):
    """[y][x] palette indices: y = 0 the newest of `lines` (each a list of w indices); rows
    without a line show the ring's initial contents."""
    out = []
    for y in range(h):
        n = len(lines) - 1 - y
        if n >= 0 and y < ring:
            out.append(list(lines[n]))
        else:
            out.append([initial] * w)
    return out


def spectrum_indices(cols, w, h, flags, grid_y0=0, grid_dy=1, grid_dx=1):
    """[y][x] palette indices of the trace region for trace rows `cols` (one per column)."""
    out = [[PAL_BG] * w for _ in range(h)]
    for x in range(w):
        v = cols[x]
        vp = cols[x - 1] if x else v
        lo, hi = min(v, vp), max(v, vp)
        gy = grid_y0
        for y in range(h):
            if lo <= y <= hi:
                pix = PAL_TRACE
            elif flags & SP_FILL and y > v:
                pix = PAL_FILL
            elif flags & SP_CENTER and x == w >> 1:
                pix = PAL_CENTER
            elif (flags & SP_HGRID and gy == 0) or (flags & SP_VGRID and x % grid_dx == 0):
                pix = PAL_GRID
            else:
                pix = PAL_BG
            out[y][x] = pix
            gy = grid_dy - 1 if gy == 0 else gy - 1
    return out
