# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference models: what the gateware must produce, in plain Python."""

from .render import PAL_BG, PAL_CENTER, PAL_TRACE, SP_CENTER

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


def spectrum_indices(cols, w, h, flags):
    """[y][x] palette indices of the trace region for trace rows `cols` (one per column)."""
    out = [[PAL_BG] * w for _ in range(h)]
    for x in range(w):
        v = cols[x]
        vp = cols[x - 1] if x else v
        lo, hi = min(v, vp), max(v, vp)
        for y in range(h):
            if lo <= y <= hi:
                out[y][x] = PAL_TRACE
            elif flags & SP_CENTER and x == w >> 1:
                out[y][x] = PAL_CENTER
    return out


# DSP chain ---------------------------------------------------------------------------------

import math

TAU = 2 * math.pi
FFT_MAX_LOG2 = 9
FFT_MAX = 1 << FFT_MAX_LOG2
# Quarter wave of sin(TAU * k / 1024), Q15, k = 0..256 (entry 256 is 1.0 as 32767): the
# test source's table.
QUARTER = [min(32767, round(32768 * math.sin(TAU * k / 1024))) for k in range(257)]
# The FFT's twiddles: sin(TAU * k / FFT_MAX), k = 0..FFT_MAX / 4.
TW_QUARTER = [min(32767, round(32768 * math.sin(TAU * k / FFT_MAX)))
              for k in range(FFT_MAX // 4 + 1)]
# log2(1 + m / 256) in 1/256 units, m = 0..255.
LOG2_FRAC = [round(256 * math.log2(1 + m / 256)) for m in range(256)]
WF_MAX = 247


def bitrev(n, bits):
    return int(f"{n:0{bits}b}"[::-1], 2) if bits else 0


def twiddle(t):
    """(cos, sin) of TAU * t / FFT_MAX for t in [0, FFT_MAX / 2), Q15, from the quarter table."""
    q = FFT_MAX // 4
    if t < q:
        return TW_QUARTER[q - t], TW_QUARTER[t]
    return -TW_QUARTER[t - q], TW_QUARTER[2 * q - t]


def fits(v, bits):
    return -(1 << (bits - 1)) <= v < (1 << (bits - 1))


def round_shift(v, sh):
    return (v + (1 << (sh - 1))) >> sh if sh else v


def fft(re, im):
    """Radix-2 DIT FFT in place on lists of s16 whose complex magnitudes stay below 2**15;
    every stage halves (rounded), so they stay there. Returns (re, im, exponent), natural
    order, X = out * 2**exponent."""
    n = len(re)
    bits = n.bit_length() - 1
    re = [re[bitrev(i, bits)] for i in range(n)]
    im = [im[bitrev(i, bits)] for i in range(n)]
    exp = 0
    for s in range(bits):
        sh = 1
        exp += sh
        span = 1 << s
        for b in range(n // 2):
            j = b & (span - 1)
            a = ((b >> s) << (s + 1)) | j
            c = a + span
            wr, ws = twiddle(j << (FFT_MAX_LOG2 - 1 - s))
            # B * W with W = cos - i sin.
            pr = (re[c] * wr + im[c] * ws + (1 << 14)) >> 15
            pi = (im[c] * wr - re[c] * ws + (1 << 14)) >> 15
            ar, ai = re[a], im[a]
            re[a], im[a] = round_shift(ar + pr, sh), round_shift(ai + pi, sh)
            re[c], im[c] = round_shift(ar - pr, sh), round_shift(ai - pi, sh)
    return re, im, exp


def log2_units(p):
    """256 * log2(p) for p >= 1 (0 for p = 0): leading one, then the next 8 bits."""
    if p <= 0:
        return 0
    e = p.bit_length() - 1
    mant = ((p << 8) >> e) & 0xff
    return 256 * e + LOG2_FRAC[mant]


def window_q15(kind, n):
    """Window coefficients (Q15, the first half; the second mirrors it), as esp_sdr_window()
    makes them: 0 rectangular, 1 Hann, 5 Blackman-Harris; periodic (span n)."""
    out = []
    for k in range(n // 2):
        x = TAU * k / n
        if kind == 0:
            w = 1.0
        elif kind == 1:
            w = 0.5 - 0.5 * math.cos(x)
        else:
            w = (0.35875 - 0.48829 * math.cos(x) + 0.14128 * math.cos(2 * x)
                 - 0.01168 * math.cos(3 * x))
        out.append(min(32767, round(32768 * w)))
    return out


def window_coef(win, n, k):
    return win[k] if k < n // 2 else win[n - 1 - k]


def avg_table(alpha):
    """F(d) = 256 * log2((1 - alpha) + alpha * 2**(d / 256)) for d = 8 * (i - 256), and the
    constants beyond the table: C_HI = 256 * log2(alpha), C_LO = 256 * log2(1 - alpha)."""
    tab = []
    for i in range(512):
        d = 8 * (i - 256)
        tab.append(round(256 * math.log2((1 - alpha) + alpha * 2 ** (d / 256))))
    c_hi = round(256 * math.log2(alpha))
    c_lo = round(256 * math.log2(1 - alpha)) if alpha < 1 else -32768
    return tab, c_hi, c_lo


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def average(s, l, tab, c_hi, c_lo):
    """One step of the log-domain exponential average: state s, new value l (L units)."""
    d = l - s
    if d >= 2048:
        return s16(l + c_hi)
    if d < -2048:
        return s16(s + c_lo)
    return s16(s + tab[(d >> 3) + 256])


def windowed(samples, win):
    """(i, q) s16 pairs times the window and a half (so the magnitude stays below 2**15),
    rounded."""
    n = len(samples)
    re = [(i * window_coef(win, n, k) + (1 << 15)) >> 16 for k, (i, q) in enumerate(samples)]
    im = [(q * window_coef(win, n, k) + (1 << 15)) >> 16 for k, (i, q) in enumerate(samples)]
    return re, im


def spectrum_frame(samples, win, l_offset):
    """L per bin, fftshifted, for one frame of (i, q) s16 pairs."""
    n = len(samples)
    xr, xi, exp = fft(*windowed(samples, win))
    exp += 1
    out = [0] * n
    for k in range(n):
        out[k ^ (n >> 1)] = s16(log2_units(xr[k] ** 2 + xi[k] ** 2) + 512 * exp + l_offset)
    return out


def columns(bins, width, start, step):
    """Max over the bins of each column: [floor(c * step), floor((c + 1) * step)) from start,
    at least one bin; step in 8.8."""
    out = []
    for c in range(width):
        b0 = start + ((c * step) >> 8)
        b1 = max(start + (((c + 1) * step) >> 8), b0 + 1)
        out.append(max(bins[b] for b in range(b0, min(b1, len(bins)))) if b0 < len(bins)
                   else -32768)
    return out


def scale(v, lo, gain, top):
    """clamp(((v - lo) * gain) >> 8, 0, top)."""
    return max(0, min(top, ((v - lo) * gain) >> 8))
