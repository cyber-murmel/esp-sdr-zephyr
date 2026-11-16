# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/modules/dusk.py, the board's colours, against tools/dusk.py, which derives
them in OkLab: the values match, the text pairs the module names reach WCAG AA and the heat
scale only ever gets lighter."""

import re

import pytest

from conftest import load

tool = load("apps/micropython/tools/dusk.py", "dusk_tool")
dusk = load("apps/micropython/modules/dusk.py")


def luminance(c):
    r, g, b = (tool.to_linear((c >> s & 0xFF) / 255) for s in (16, 8, 0))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


def test_steps_are_the_tools():
    ramps = tool.ramps()
    steps = {n: v for n, v in vars(dusk).items() if re.fullmatch(r"(GRAY|PURPLE|ORANGE)_\d+", n)}
    assert steps
    for name, value in steps.items():
        hue, step = name.lower().split("_")
        assert value == ramps["{}-{}".format(hue, step)], name


def test_heat_is_the_tools():
    assert dusk.HEAT == tool.heat_stops()


@pytest.mark.parametrize("fg, bg, least", [
    ("TEXT", "BG", 13.5), ("TEXT", "SURFACE", 12), ("MUTED", "BG", 5.5), ("MUTED", "SURFACE", 4.5),
    ("ON_ACCENT", "ACCENT", 7), ("TEXT", "WARN", 5.5), ("ERROR", "BG", 7),
])
def test_text_contrast(fg, bg, least):
    assert contrast(getattr(dusk, fg), getattr(dusk, bg)) >= least


def test_white_on_primary():
    # LVGL's default theme writes white on its primary colour (buttons).
    assert contrast(0xFFFFFF, dusk.PRIMARY) >= 4.5


def test_rgb565():
    assert dusk.rgb565(0xFFFFFF) == 0xFFFF
    assert dusk.rgb565(0xFF0000) == 0xF800
    assert dusk.rgb565(0x00FF00) == 0x07E0
    assert dusk.rgb565(0x0000FF) == 0x001F
    assert dusk.rgb565(0x121018) == (0x12 >> 3) << 11 | (0x10 >> 2) << 5 | 0x18 >> 3


def test_heat_table():
    pal = dusk.heat()
    assert len(pal) == 128 and pal.typecode == "H"
    assert pal[0] == dusk.rgb565(dusk.HEAT[0]) and pal[-1] == dusk.rgb565(dusk.HEAT[-1])
    assert len(dusk.heat(64)) == 64


def lightness565(c):
    r, g, b = (c >> 11) << 3, (c >> 5 & 0x3F) << 2, (c & 0x1F) << 3
    return tool.rgb_to_lab(r << 16 | g << 8 | b)[0]


def test_heat_gamma():
    # The ends stay; above 1 the middle of the table is darker, below 1 lighter, and the
    # lightness still only climbs along it.
    linear, steep, soft = dusk.heat(), dusk.heat(gamma=2.0), dusk.heat(gamma=0.5)
    for pal in (steep, soft):
        assert pal[0] == linear[0] and pal[-1] == linear[-1]
        lightness = [lightness565(c) for c in pal]
        assert all(a <= b + 0.01 for a, b in zip(lightness, lightness[1:]))
    mid = len(linear) // 2
    assert lightness565(steep[mid]) < lightness565(linear[mid]) < lightness565(soft[mid])


def test_heat_gets_lighter():
    # Rank reads from lightness alone (grayscale, colour blindness): OkLab L rises at every stop.
    lightness = [tool.rgb_to_lab(c)[0] for c in dusk.HEAT]
    assert all(a < b for a, b in zip(lightness, lightness[1:]))
    assert lightness[0] < 0.25 and lightness[-1] > 0.97
