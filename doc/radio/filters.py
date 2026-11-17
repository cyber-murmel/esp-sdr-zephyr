#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render the receive filter plots doc/radio/filters-*.svg.

filters-rx-codes.svg: the receive low-pass capacitor code against the two-sided bandwidth, per
chip. These are the curves esp_sdr_rx_set_bandwidth() interpolates in (upstream's
modules/lib/esp-sdr/main/common/rx_bandwidth.h, included by lib/esp_sdr/src/esp_sdr_rx.c):
approximate full -3 dB widths from median noise spectra.

filters-attenuation.svg: how far a line drops below its pass-band level by its distance from the
LO, every chip and setting the sources measured in one plot, as a datasheet compares parts: the
ESP32-S31's low-pass settings (points past the measurement floor sit on it with a downward
marker), and the spans and bounds the sources give for the S31's widest setting and for the
ESP32-C6's and ESP32-S3's channel modes.

The data tables come first; finer measurements replace them there. The drawing (plain SVG,
<text> labels, deterministic output) follows, then main().

Run: python3 doc/radio/filters.py writes the SVGs next to this file.
--check: exit 1 if a committed SVG differs from what the script would write.
"""

import sys
from pathlib import Path
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent

# Code to two-sided bandwidth in MHz, (code, MHz), from rx_bandwidth.h.
RX_CODES = [
    ("ESP32-S3", [(0, 69), (4, 51), (8, 45), (16, 33), (24, 25), (32, 21), (48, 16), (60, 13)]),
    ("ESP32-C6", [(0, 54), (4, 48), (8, 39), (12, 33), (16, 28), (24, 23), (32, 20), (40, 17),
                  (48, 15), (60, 12)]),
    ("ESP32-C5, mode 0", [(0, 23), (4, 22), (8, 21), (12, 20), (16, 18), (24, 16), (32, 15),
                          (40, 13), (48, 12), (60, 11)]),
    ("ESP32-C5, mode 1", [(0, 48), (4, 45), (8, 42), (12, 40), (16, 37), (24, 34), (32, 30),
                          (40, 27), (48, 25), (56, 23), (60, 22)]),
    ("ESP32-S31", [(0, 54), (8, 36), (16, 30), (28, 21), (32, 18), (48, 15), (60, 13)]),
    ("ESP32 (7-bit codes)", [(8, 67), (12, 55), (16, 48), (24, 38), (32, 32), (48, 25), (64, 20),
                             (80, 17), (96, 15), (112, 14), (127, 12)]),
]

# Attenuation: dB below a line's pass-band level, by the line's distance from the LO in MHz, every
# chip in one plot. Kinds:
# - "line": measured points; None is past the measurement floor (over FLOOR dB down), and a list
#   ends where it was not measured further.
# - "range": (MHz, least, most) bars where the source gives a span, (MHz, dB) points otherwise;
#   not joined, since nothing is known in between.
# - "band": (from MHz, to MHz, dB): within that many dB of the pass band over the span.
FLOOR = 40
ATTENUATION = [
    # Fixed gain, 80 MS/s, by low-pass setting.
    ("line", "ESP32-S31, 13 MHz", [(6, 2), (7, 10), (8, 19), (9, 23), (10, 31), (12, None)]),
    ("line", "ESP32-S31, 15 MHz", [(6, 0), (7, 1), (8, 5), (9, 14), (10, 19), (12, 31), (14, None)]),
    ("line", "ESP32-S31, 17 MHz", [(6, 0), (7, 1), (8, 1), (9, 0), (10, 7), (12, 20), (14, 26),
                                   (16, 36)]),
    ("line", "ESP32-S31, 21 MHz", [(6, 0), (7, 2), (8, 0), (9, 2), (10, 1), (12, 8), (14, 18),
                                   (16, 28), (20, None)]),
    ("line", "ESP32-S31, AUTO", [(6, 0), (7, 1), (8, 1), (9, 1), (10, 2), (12, 8), (14, 18),
                                 (16, 28), (20, 31)]),
    # The widest setting: "within some 3 dB out to 25 MHz, some 7 dB down at 30" (one burst each).
    ("range", "ESP32-S31, 54 MHz", [(25, 3), (30, 7)]),
    # The WHY2025 survey: "16 to 21 dB down at +-25 MHz and some 35 dB at +-35 MHz".
    ("range", "ESP32-C6, 40 MHz mode", [(25, 16, 21), (35, 35)]),
    # Widest low-pass: an 802.15.4 sender within 7 dB of its level from +-15 out to +-35 MHz.
    ("band", "ESP32-C6, 20 MHz mode", [(15, 35, 7)]),
    # cbw 2, TX and RX end to end: +-18 MHz within +-3 dB.
    ("band", "ESP32-S3, 40 MHz mode (TX+RX)", [(5, 18, 3)]),
]

# Drawing.

COLOURS = ["#1f6fb4", "#d9541e", "#2a9d3c", "#c0392b", "#7e57c2", "#8d6e3f", "#d6338a",
           "#4d4d4d", "#17a2b8"]
MARKERS = ["circle", "square", "diamond", "triangle", "cross", "circle-open", "square",
           "diamond", "circle"]
W, H = 1000, 540
LEFT, RIGHT, TOP, BOTTOM = 70, 280, 50, 60


def fmt(v):
    return f"{v:g}"


class Plot:
    def __init__(self, title, xlabel, ylabel, xrange, yrange, xticks, yticks):
        self.x0, self.x1 = xrange
        self.y0, self.y1 = yrange
        self.out = [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
            f'width="{W}" height="{H}" font-family="sans-serif">',
            f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
            f'<text x="{LEFT}" y="28" font-size="18" font-weight="bold" fill="#000">'
            f'{escape(title)}</text>',
        ]
        for x in xticks:
            px = self.px(x)
            self.out.append(f'<line x1="{px:.1f}" y1="{TOP}" x2="{px:.1f}" y2="{H - BOTTOM}" '
                            'stroke="#dddddd"/>')
            self.out.append(f'<text x="{px:.1f}" y="{H - BOTTOM + 18}" font-size="12" '
                            f'text-anchor="middle" fill="#333">{fmt(x)}</text>')
        for y in yticks:
            py = self.py(y)
            self.out.append(f'<line x1="{LEFT}" y1="{py:.1f}" x2="{W - RIGHT}" y2="{py:.1f}" '
                            'stroke="#dddddd"/>')
            self.out.append(f'<text x="{LEFT - 8}" y="{py + 4:.1f}" font-size="12" '
                            f'text-anchor="end" fill="#333">{fmt(y)}</text>')
        self.out.append(f'<rect x="{LEFT}" y="{TOP}" width="{W - LEFT - RIGHT}" '
                        f'height="{H - TOP - BOTTOM}" fill="none" stroke="#000"/>')
        self.out.append(f'<text x="{(LEFT + W - RIGHT) / 2:.1f}" y="{H - 18}" font-size="13" '
                        f'text-anchor="middle" fill="#000">{escape(xlabel)}</text>')
        cy = (TOP + H - BOTTOM) / 2
        self.out.append(f'<text x="18" y="{cy:.1f}" font-size="13" text-anchor="middle" '
                        f'transform="rotate(-90 18 {cy:.1f})" fill="#000">{escape(ylabel)}</text>')
        self.legend = []

    def px(self, x):
        return LEFT + (x - self.x0) / (self.x1 - self.x0) * (W - LEFT - RIGHT)

    def py(self, y):
        return TOP + (self.y1 - y) / (self.y1 - self.y0) * (H - TOP - BOTTOM)

    def marker(self, kind, x, y, colour):
        if kind == "circle":
            return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="{colour}"/>'
        if kind == "circle-open":
            return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="#fff" stroke="{colour}" stroke-width="2"/>'
        if kind == "square":
            return f'<rect x="{x - 4:.1f}" y="{y - 4:.1f}" width="8" height="8" fill="{colour}"/>'
        if kind == "diamond":
            return (f'<polygon points="{x:.1f},{y - 5:.1f} {x + 5:.1f},{y:.1f} {x:.1f},{y + 5:.1f} '
                    f'{x - 5:.1f},{y:.1f}" fill="{colour}"/>')
        if kind == "triangle":
            return (f'<polygon points="{x:.1f},{y - 5:.1f} {x + 5:.1f},{y + 4:.1f} '
                    f'{x - 5:.1f},{y + 4:.1f}" fill="{colour}"/>')
        if kind == "down":
            return (f'<polygon points="{x - 6:.1f},{y - 5:.1f} {x + 6:.1f},{y - 5:.1f} '
                    f'{x:.1f},{y + 6:.1f}" fill="#fff" stroke="{colour}" stroke-width="2"/>')
        return (f'<path d="M{x - 4:.1f},{y - 4:.1f} L{x + 4:.1f},{y + 4:.1f} M{x - 4:.1f},{y + 4:.1f} '
                f'L{x + 4:.1f},{y - 4:.1f}" stroke="{colour}" stroke-width="2"/>')

    def series(self, i, name, points, floor=None, dashed=False):
        colour, kind = COLOURS[i % len(COLOURS)], MARKERS[i % len(MARKERS)]
        xy = [(self.px(x), self.py(floor if y is None else y), y is None) for x, y in points]
        path = " ".join(f"{x:.1f},{y:.1f}" for x, y, _ in xy)
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        self.out.append(f'<polyline points="{path}" fill="none" stroke="{colour}" '
                        f'stroke-width="2"{dash}/>')
        for x, y, past in xy:
            self.out.append(self.marker("down" if past else kind, x, y, colour))
        self.legend.append((name, colour, kind, dash))

    def ranges(self, i, name, points):
        colour, kind = COLOURS[i % len(COLOURS)], MARKERS[i % len(MARKERS)]
        for p in points:
            x = self.px(p[0])
            if len(p) == 3:
                y0, y1 = self.py(-p[1]), self.py(-p[2])
                self.out.append(f'<line x1="{x:.1f}" y1="{y0:.1f}" x2="{x:.1f}" y2="{y1:.1f}" '
                                f'stroke="{colour}" stroke-width="3"/>')
                for y in (y0, y1):
                    self.out.append(f'<line x1="{x - 6:.1f}" y1="{y:.1f}" x2="{x + 6:.1f}" '
                                    f'y2="{y:.1f}" stroke="{colour}" stroke-width="2"/>')
            else:
                self.out.append(self.marker(kind, x, self.py(-p[1]), colour))
        self.legend.append((name, colour, kind, ' stroke-opacity="0"'))

    def band(self, i, name, spans):
        colour = COLOURS[i % len(COLOURS)]
        for x0, x1, db in spans:
            px0, px1, py0, py1 = self.px(x0), self.px(x1), self.py(0), self.py(-db)
            self.out.append(f'<rect x="{px0:.1f}" y="{py0:.1f}" width="{px1 - px0:.1f}" '
                            f'height="{py1 - py0:.1f}" fill="{colour}" fill-opacity="0.18" '
                            f'stroke="{colour}" stroke-dasharray="4 3"/>')
        self.legend.append((name, colour, "band", ""))

    def hline(self, y, label):
        py = self.py(y)
        self.out.append(f'<line x1="{LEFT}" y1="{py:.1f}" x2="{W - RIGHT}" y2="{py:.1f}" '
                        'stroke="#555" stroke-dasharray="3 3"/>')
        self.out.append(f'<text x="{W - RIGHT - 6}" y="{py - 6:.1f}" font-size="11" '
                        f'text-anchor="end" fill="#555">{escape(label)}</text>')

    def svg(self, note=None):
        lx, ly = W - RIGHT + 18, TOP + 10
        for n, (name, colour, kind, dash) in enumerate(self.legend):
            y = ly + n * 24
            if kind == "band":
                self.out.append(f'<rect x="{lx}" y="{y - 6}" width="28" height="12" fill="{colour}" '
                                f'fill-opacity="0.18" stroke="{colour}" stroke-dasharray="4 3"/>')
            else:
                self.out.append(f'<line x1="{lx}" y1="{y}" x2="{lx + 28}" y2="{y}" '
                                f'stroke="{colour}" stroke-width="2"{dash}/>')
                self.out.append(self.marker(kind, lx + 14, y, colour))
            self.out.append(f'<text x="{lx + 36}" y="{y + 4}" font-size="12" fill="#000">'
                            f'{escape(name)}</text>')
        if note:
            for n, line in enumerate(note):
                self.out.append(f'<text x="{lx}" y="{ly + len(self.legend) * 24 + 16 + n * 16}" '
                                f'font-size="11" fill="#555">{escape(line)}</text>')
        return "\n".join(self.out + ["</svg>", ""])


def rx_codes():
    p = Plot("Receive low-pass: code against bandwidth", "Capacitor code (larger is narrower)",
             "Two-sided bandwidth (MHz)", (0, 128), (0, 70), range(0, 129, 16), range(0, 71, 10))
    for i, (name, points) in enumerate(RX_CODES):
        p.series(i, name, points, dashed=name.startswith("ESP32 ("))
    return p.svg(["Approximate full -3 dB widths", "from median noise spectra", "(rx_bandwidth.h)"])


def attenuation():
    p = Plot("Receive low-pass attenuation", "Distance of the line from the LO (MHz)",
             "Level against the pass band (dB)", (4, 36), (-45, 2), range(5, 36, 5),
             range(-40, 1, 10))
    p.hline(-FLOOR, "measurement floor")
    for i, (kind, name, data) in enumerate(ATTENUATION):
        if kind == "line":
            p.series(i, name, [(x, None if a is None else -a) for x, a in data], -FLOOR)
        elif kind == "range":
            p.ranges(i, name, data)
        else:
            p.band(i, name, data)
    return p.svg(["Open triangles: over 40 dB down.", "Bars: a span the source gives;",
                  "bands: within that many dB.", "S31 settings: fixed gain, 80 MS/s."])


PLOTS = {"filters-rx-codes.svg": rx_codes, "filters-attenuation.svg": attenuation}


def main():
    check = "--check" in sys.argv[1:]
    stale = []
    for name, render in PLOTS.items():
        path = HERE / name
        svg = render()
        if not path.exists() or path.read_text(encoding="utf-8") != svg:
            stale.append(name)
            if not check:
                path.write_text(svg, encoding="utf-8")
    if check and stale:
        sys.exit("out of date, run doc/radio/filters.py: " + ", ".join(stale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
