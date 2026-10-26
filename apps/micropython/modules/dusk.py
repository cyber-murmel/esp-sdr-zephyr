# SPDX-License-Identifier: GPL-3.0-or-later
"""Dusk, the board's colours: OkLab ramps of a bright orange, a dark purple and a gentle gray,
each step named by its OkLab lightness x 100 (ORANGE_72 has L 0.72: equal numbers are equally
light in every hue), and the heat scale of the waterfalls, dark purple through orange to near
white with lightness climbing the whole way. tools/dusk.py derives every value here.

Colours are 0xRRGGBB (lv.color_hex); rgb565() turns one into the framebuffers' native RGB565."""

# The ramps' steps the screens use.
GRAY_18 = 0x121018
GRAY_24 = 0x201E26
GRAY_32 = 0x34313A
GRAY_64 = 0x8D8B93
GRAY_90 = 0xDEDDE2
GRAY_96 = 0xF2F1F5
PURPLE_56 = 0x895BB8
PURPLE_78 = 0xC7A9E3
ORANGE_40 = 0x772D0A
ORANGE_72 = 0xFE7802
ORANGE_78 = 0xFC9D5A

# Roles on the dark screen. Text pairs reach WCAG AA: TEXT 14:1 and MUTED 5.6:1 on BG,
# ON_ACCENT 7.1:1 on ACCENT, white 4.9:1 on PRIMARY (LVGL's buttons), TEXT 6:1 on WARN.
BG = GRAY_18
SURFACE = GRAY_24
LINE = GRAY_32
TEXT = GRAY_90
MUTED = GRAY_64
PRIMARY = PURPLE_56
ACCENT = ORANGE_72
ON_ACCENT = GRAY_18
WARN = ORANGE_40
ERROR = ORANGE_78
# Plot traces in order: the first, a second (Q, peaks), a floor, a marker.
TRACES = (ORANGE_72, PURPLE_78, GRAY_64, GRAY_96)

# The heat scale at 33 even steps: purple-24, orange-72 at 0.6, near white at 1, in straight
# lines through OkLab. Between neighbours sRGB interpolation stays within 0.004 of the OkLab
# path, far below what an eye can tell.
HEAT = (0x271143, 0x301844, 0x391E46, 0x432447, 0x4D2A48, 0x572F48, 0x623548, 0x6C3A48,
        0x773F48, 0x834447, 0x8E4946, 0x9A4E44, 0xA55442, 0xB1593F, 0xBD5E3C, 0xC96337,
        0xD66831, 0xE26D2A, 0xEF721F, 0xFB7709, 0xFF812A, 0xFF8D46, 0xFF995C, 0xFFA36E,
        0xFFAE7F, 0xFFB88F, 0xFFC29E, 0xFFCBAD, 0xFFD5BC, 0xFFDECA, 0xFFE7D8, 0xFFF0E5,
        0xFFF9F3)


def rgb565(c):
    return (c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F)


def heat(n=128, gamma=1.0):
    """The heat scale in n RGB565 entries, coldest first (esdr.fb_wf_row's palette). Entry k
    shows the scale at (k / (n - 1)) ** gamma: above 1 the weaker values stay dark longer and
    the strong ones stand out, below 1 the reverse."""
    import array

    pal = array.array("H", [0] * n)
    last = len(HEAT) - 1
    for k in range(n):
        f = (k / (n - 1)) ** gamma * last
        i = min(int(f), last - 1)
        t = f - i
        a, b = HEAT[i], HEAT[i + 1]
        c = 0
        for s in (16, 8, 0):
            p = a >> s & 0xFF
            c = c << 8 | int(p + ((b >> s & 0xFF) - p) * t + 0.5)
        pal[k] = rgb565(c)
    return pal


def theme(lv):
    """LVGL's default theme in these colours, dark, on the default display: buttons, sliders,
    check boxes and focus outlines in PRIMARY, the secondary colour ACCENT."""
    disp = lv.display_get_default()
    if disp:
        disp.set_theme(lv.theme_default_init(disp, lv.color_hex(PRIMARY), lv.color_hex(ACCENT),
                                             True, lv.font_get_default()))
