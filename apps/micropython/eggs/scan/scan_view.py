# SPDX-License-Identifier: GPL-3.0-or-later
"""The Scan app's plan and numbers, without the radio or the display (tests/host/test_scan_egg.py):
which LOs a sweep tunes and which of each hop's 256 bins go where in the panorama, the tuning
edges, the levels, the marker's text and the saved lines.

Frequencies are integer MHz (the LOs, on a 5 MHz grid) and panorama bins 312.5 kHz apart (16 to
5 MHz); Hz only in text and files (past 2^30 a MicroPython int is a heap object). A hop is an
80 MS/s burst at an LO, its spectrum 256 bins with DC at bin 128 (esdr.scan_hop()). Each panorama
bin comes from one hop of a plan: the nearest LO, unless it lies within the guard around that LO
(its DC, 1/f and the window's main lobe), then the neighbour on its side, never farther than the
usable half span U. Two plans whose LOs differ take turns, so what moves with the LO (its DC
residue, images, LO spurs) lands at other frequencies in the other plan."""

N = 256
BIN_HZ = 312500
GRID = 5
# The smallest gap between two LOs of a plan.
GAP_MIN = 10
EMPTY = -200.0

# Per radio chip (esdr.RADIO); all radio values ints:
#   bw     the receive filter (esdr.rx_bandwidth()), the widest the chip takes
#   cbw    the S3's PHY channel bandwidth mode (esdr.channel_bw()), None elsewhere
#   d      LO step of a plan (MHz, on the grid): the farthest a bin of the core is read from its
#          LO (a guard's bins from the neighbour), so where the filter is still flat
#   u      half span (MHz) read at all, in the flanks beyond the outer LOs
#   edges  (out, in, out) MHz for bisection where tune() refuses an LO out of lock, else None
#   table  (lo_min, lo_max) MHz where it does not refuse (a C6 behind the P4 whose firmware does
#          not report its PLL: radio_state()["locked"] None), a measured chip's range, a grid
#          step or two in; None where the radio is on the chip
#   lines  known lines of the board itself beyond the crystal's multiples (MHz)
#   deadline  ms a hop may take before it counts as failed
#   flat   the filter's roll-off as (offset MHz, dB down) points, added back; none: where the
#          noise at gain 48 is the ADC's (the S31, the C6) a correction would only lift it at
#          the hops' edges
# Measured 2026-11-10 (tmp/mscan/flat.py, the median over the hops of each bin, gain 48): the
# S3 with channel_bw(2) and 69 MHz within 1 dB to +-15.6 MHz, 3 dB at +-20, 6 at +-22 (its
# channel_bw(0) passes +-35 MHz and lets what lies up to 50 MHz away fold in); the S31's and the
# C6's noise flat within 3 dB to +-40 MHz (the ADC's), the S31's filter 3 dB down at 25 MHz
# (doc/radio/filters.md); DC and its skirt +3 dB within +-1.5 MHz on the S31 and the C6.
RADIOS = {
    "esp32s3": {"bw": 69000000, "cbw": 2, "d": 15, "u": 18, "edges": (1700, 2400, 3000),
                "table": None, "lines": (), "deadline": 60, "flat": ()},
    "esp32c6": {"bw": 54000000, "cbw": None, "d": 20, "u": 24, "edges": (1700, 2400, 3000),
                "table": (1795, 2845), "lines": (), "deadline": 300, "flat": ()},
    "esp32s31": {"bw": 54000000, "cbw": None, "d": 20, "u": 24, "edges": (2195, 2400, 2905),
                 "table": None, "lines": (2250, 2500, 2750), "deadline": 200, "flat": ()},
}
# The crystal's harmonics (40 MHz on all these chips; 2400 MHz the radio's own).
XTAL_MHZ = 40


class Geometry:
    """The panorama of LOs lo_min to lo_max (MHz): bin j is lo_min + (j - ub) bins."""

    def __init__(self, lo_min, lo_max, u, d):
        self.lo_min = lo_min
        self.lo_max = lo_max
        self.d = d
        self.ub = u * 16 // 5
        self.db = d * 16 // 5
        self.gb = max(6, self.ub - self.db)
        self.n = (lo_max - lo_min) * 16 // 5 + 2 * self.ub + 1

    def lo_bin(self, lo):
        return (lo - self.lo_min) * 16 // 5 + self.ub

    def f16(self, j):
        """Bin j in 1/16 MHz (a small int)."""
        return self.lo_min * 16 + (j - self.ub) * 5

    def hz(self, j):
        return self.lo_min * 1000000 + (j - self.ub) * BIN_HZ

    def mhz_text(self, j):
        f = self.f16(j)
        return "{}.{:04d}".format(f // 16, (f % 16) * 625)

    def core(self):
        """The bins within D/2 of an LO's range (the flanks beyond come from one hop only)."""
        half = self.db // 2
        return self.ub - half, self.n - self.ub + half


def _los(lo_min, lo_max, first, d):
    """A plan's LOs: lo_min, then first, first + d, ... below lo_max, then lo_max; gaps between
    GAP_MIN and d."""
    los = [lo_min]
    x = first
    while x < lo_max:
        if x - los[-1] >= GAP_MIN:
            los.append(x)
        x += d
    if lo_max != los[-1]:
        if len(los) > 1 and lo_max - los[-1] < GAP_MIN:
            los[-1] = lo_max - GAP_MIN
            if los[-1] - los[-2] < GAP_MIN:
                los.pop()
        los.append(lo_max)
    return los


def plan(geo, parity):
    """Plan A (parity 0) or B: [(lo MHz, [(src0, dst0, n), ...]), ...] in order of LO. Every bin of
    the panorama has one owner, at an offset between gb and ub bins from its LO."""
    d = geo.d
    if geo.lo_max - geo.lo_min < 2 * GAP_MIN:
        # Too narrow for two plans (never the whole range of a radio): its ends in both.
        los = [geo.lo_min] if geo.lo_max == geo.lo_min else [geo.lo_min, geo.lo_max]
    else:
        los = _los(geo.lo_min, geo.lo_max, geo.lo_min + (d if parity == 0 else GAP_MIN), d)
    centres = [geo.lo_bin(lo) for lo in los]
    segs = [[] for _ in los]
    k = 0
    last = len(centres) - 1
    for j in range(geo.n):
        # The nearest LO (the lower on a tie).
        while k < last and centres[k + 1] - j < j - centres[k]:
            k += 1
        owner = k
        x = j - centres[k]
        if -geo.gb < x < geo.gb:
            side = k + 1 if x >= 0 else k - 1
            if side < 0 or side > last:
                side = k - 1 if x >= 0 else k + 1
            if 0 <= side <= last:
                owner = side
        x = j - centres[owner]
        src = N // 2 + x
        runs = segs[owner]
        if runs and runs[-1][0] + runs[-1][2] == src and runs[-1][1] + runs[-1][2] == j:
            runs[-1][2] += 1
        else:
            runs.append([src, j, 1])
    return [(lo, [tuple(r) for r in runs]) for lo, runs in zip(los, segs)]


def owners(geo, hops):
    """Per bin: (hop index, offset bins), or None where no hop reads it (for the tests)."""
    out = [None] * geo.n
    for i, (lo, runs) in enumerate(hops):
        for src, dst, n in runs:
            for t in range(n):
                if out[dst + t] is not None:
                    raise ValueError("bin {} twice".format(dst + t))
                out[dst + t] = (i, src + t - N // 2)
    return out


def edges(try_tune, hint):
    """The LO range (MHz) where try_tune(mhz) holds, by bisection on the grid from hint (out, in,
    out); an LO refused twice counts as refused (the S31 refuses one in a hundred at random).
    Each edge then moves a grid step in (the window moves a few codes between calibrations)."""
    out_lo, mid, out_hi = hint

    def ok(mhz):
        return try_tune(mhz) or try_tune(mhz)

    if not ok(mid):
        raise OSError("no lock at {} MHz".format(mid))

    def search(good, bad):
        while abs(bad - good) > GRID:
            m = (good + bad) // 2 // GRID * GRID
            if m == good or m == bad:
                break
            if ok(m):
                good = m
            else:
                bad = m
        return good

    lo = search(mid, out_lo) + GRID
    hi = search(mid, out_hi) - GRID
    return min(lo, mid), max(hi, mid)


def correction(flat, offsets=N):
    """The roll-off correction (dB to add) per bin of a hop's spectrum, from (offset MHz, dB down)
    points, linear between them (0 inside the first)."""
    if not flat:
        return None
    pts = sorted(flat)
    out = []
    for s in range(offsets):
        f = abs(s - N // 2) * 5 / 16
        down = pts[0][1] if f <= pts[0][0] else pts[-1][1]
        for (f0, d0), (f1, d1) in zip(pts, pts[1:]):
            if f0 <= f <= f1:
                down = d0 + (d1 - d0) * (f - f0) / (f1 - f0) if f1 > f0 else d1
                break
        out.append(down)
    return out


def _floor5(v):
    return int(v // 5) * 5


def levels(floors, peaks, cur=None):
    """The plot's (bottom, top) dB from a sweep's hop floors and peaks: 10 dB under the median
    floor, 3 over the strongest peak, 40 apart at least, on 5 dB steps; cur stays unless one moves
    5 dB or more."""
    fl = sorted(f for f in floors if f > EMPTY)
    pk = [p for p in peaks if p > EMPTY]
    if not fl or not pk:
        return cur if cur is not None else (-100, -20)
    bottom = _floor5(fl[len(fl) // 2] - 10)
    top = -_floor5(-(max(pk) + 3))
    top = max(top, bottom + 40)
    if cur is not None and abs(cur[0] - bottom) < 5 and abs(cur[1] - top) < 5:
        return cur
    return bottom, top


def known(geo, j, lines=()):
    """A known line within a bin of j: a crystal harmonic or one of lines, as text, else ''."""
    f = geo.f16(j)
    for mhz in lines:
        if abs(f - mhz * 16) <= 5:
            return "{}M".format(mhz)
    near = (f + XTAL_MHZ * 8) // (XTAL_MHZ * 16) * XTAL_MHZ * 16
    if abs(f - near) <= 5:
        return "x{}".format(XTAL_MHZ)
    return ""


def marker_text(geo, j, avg, peak, floor, lines=()):
    def db(v):
        return "--" if v <= EMPTY else "{:.0f}".format(v)

    text = "{}M a{} p{} f{}".format(geo.mhz_text(j), db(avg[j]), db(peak[j]), db(floor[j]))
    flag = known(geo, j, lines)
    if not (geo.ub <= j < geo.n - geo.ub):
        flag = (flag + " flank").strip()
    return (text + " " + flag).strip()


def strongest(values, a, b):
    """The bin of the largest value in [a, b), or a."""
    best, at = EMPTY - 1, a
    for j in range(a, b):
        if values[j] > best:
            best, at = values[j], j
    return at


def next_peak(values, a, b, taken, sep=4):
    """The strongest bin in [a, b) at least sep bins from each in taken, or None."""
    best, at = EMPTY, None
    for j in range(a, b):
        v = values[j]
        if v <= best:
            continue
        for t in taken:
            if -sep < j - t < sep:
                break
        else:
            best, at = v, j
    return at


def clock(t_s):
    """Uptime as a 1970 date and time (no RTC on the boards): '1970-01-DD, HH:MM:SS'."""
    t = int(t_s)
    day, t = divmod(t, 86400)
    h, t = divmod(t, 3600)
    m, s = divmod(t, 60)
    return "1970-01-{:02d}, {:02d}:{:02d}:{:02d}".format(day + 1, h, m, s)


# Text of the 256 q8 steps (0 empty: the floor of the scale).
Q8_TEXT = ["-127.50"] + ["{:.2f}".format((q - 255) / 2) for q in range(1, 256)]


def rtl_line(t_s, low_hz, high_hz, samples, row):
    """An rtl_power line of a history row (q8 bytes): date, time, Hz low, Hz high, Hz step,
    samples, dB..."""
    return "{}, {}, {}, {:.2f}, {}, {}".format(clock(t_s), low_hz, high_hz, BIN_HZ, samples,
                                               ", ".join(Q8_TEXT[q] for q in row))
