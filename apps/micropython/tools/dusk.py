#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derives Dusk, the board's colours (modules/dusk.py), in OkLab: three ramps from a bright
orange, a dark purple and a gentle gray, each step named by its OkLab lightness x 100 (orange-72
has L 0.72, so equal numbers are equally light in every hue), and the heat scale of the
waterfalls: purple-24 through orange-72 to near white in straight lines through OkLab, chroma
pulled back to the sRGB edge where the line leaves it, so lightness climbs the whole way.

usage: dusk.py   prints the colours modules/dusk.py holds (tests/host/test_dusk.py compares)
"""

import math

STEPS = (96, 90, 84, 78, 72, 64, 56, 48, 40, 32, 24)

# Per ramp: steps, hue and chroma as (step, value) points joined by straight lines, the anchor
# (the colour the ramp is built around, kept exact). Orange's hue leans yellow in the tints and
# red in the shades; gray is tinted toward the purple.
RAMPS = {
    "orange": ((STEPS, ((24, 38), (72, 50), (96, 68)),
                ((24, 0.070), (32, 0.090), (48, 0.135), (64, 0.168), (72, 0.190), (78, 0.140),
                 (84, 0.098), (90, 0.060), (96, 0.025)), 72)),
    "purple": ((STEPS, ((24, 300), (32, 302), (96, 310)),
                ((24, 0.090), (32, 0.110), (48, 0.140), (56, 0.145), (72, 0.110), (84, 0.065),
                 (96, 0.018)), 32)),
    "gray": (((99,) + STEPS + (18,), ((18, 298), (99, 300)),
              ((18, 0.016), (40, 0.014), (78, 0.011), (99, 0.004)), 78)),
}

# The heat scale: purple-24 at 0, orange-72 at HEAT_SPLIT, near white (L, C, h) at 1.
HEAT_SPLIT = 0.6
HEAT_WHITE = (0.985, 0.012, 70)
HEAT_STOPS = 33


def to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_srgb(c):
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def oklab_to_linear(L, a, b):
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)


def linear_to_oklab(r, g, b):
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l, m, s = (math.copysign(abs(x) ** (1 / 3), x) for x in (l, m, s))
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)


def lch_to_lab(L, C, h):
    return L, C * math.cos(h * math.tau / 360), C * math.sin(h * math.tau / 360)


def lab_to_lch(L, a, b):
    return L, math.hypot(a, b), math.atan2(b, a) * 360 / math.tau % 360


def in_gamut(L, C, h):
    return all(-1e-6 <= x <= 1 + 1e-6 for x in oklab_to_linear(*lch_to_lab(L, C, h)))


def max_chroma(L, h):
    lo, hi = 0.0, 0.4
    for _ in range(40):
        mid = (lo + hi) / 2
        if in_gamut(L, mid, h):
            lo = mid
        else:
            hi = mid
    return lo


def lch_to_rgb(L, C, h):
    """0xRRGGBB."""
    out = 0
    for x in oklab_to_linear(*lch_to_lab(L, C, h)):
        out = out << 8 | round(255 * min(1, max(0, to_srgb(min(1, max(0, x))))))
    return out


def rgb_to_lab(c):
    return linear_to_oklab(*(to_linear((c >> s & 0xFF) / 255) for s in (16, 8, 0)))


def _along(points, x):
    if x <= points[0][0]:
        return points[0][1]
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return points[-1][1]


def ramps():
    """{"orange-72": 0xRRGGBB, ...}: chroma follows the ramp's curve up to 95 % of the sRGB edge
    (the anchors keep theirs)."""
    out = {}
    for name, (steps, hue, chroma, anchor) in RAMPS.items():
        for step in steps:
            L, h, c = step / 100, _along(hue, step), _along(chroma, step)
            edge = max_chroma(L, h)
            out["{}-{}".format(name, step)] = lch_to_rgb(L, min(c, edge if step == anchor
                                                                   else 0.95 * edge), h)
    return out


def heat(t, colours=None):
    """The heat scale at t (0..1) as (L, C, h)."""
    colours = colours or ramps()
    dark, mid = rgb_to_lab(colours["purple-24"]), rgb_to_lab(colours["orange-72"])
    white = lch_to_lab(*HEAT_WHITE)
    if t <= HEAT_SPLIT:
        a, b, u = dark, mid, t / HEAT_SPLIT
    else:
        a, b, u = mid, white, (t - HEAT_SPLIT) / (1 - HEAT_SPLIT)
    L, C, h = lab_to_lch(*(p + (q - p) * u for p, q in zip(a, b)))
    return L, min(C, max_chroma(L, h)), h


def heat_stops(colours=None):
    colours = colours or ramps()
    return tuple(lch_to_rgb(*heat(k / (HEAT_STOPS - 1), colours)) for k in range(HEAT_STOPS))


def main():
    colours = ramps()
    for name, c in colours.items():
        print("{} 0x{:06X}".format(name, c))
    print("HEAT = (" + ", ".join("0x{:06X}".format(c) for c in heat_stops(colours)) + ")")


if __name__ == "__main__":
    main()
