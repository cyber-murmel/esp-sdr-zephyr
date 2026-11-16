# SPDX-License-Identifier: GPL-3.0-or-later
"""Engineering notation: 2.4G <-> 2.4e9."""

_SUFFIX = (("T", 1e12), ("G", 1e9), ("M", 1e6), ("k", 1e3), ("", 1.0),
           ("m", 1e-3), ("u", 1e-6), ("n", 1e-9), ("p", 1e-12))


def num_to_str(n, precision=6):
    if n == 0:
        return "0"
    for s, f in _SUFFIX:
        if abs(n) >= f:
            break
    return ("{:." + str(precision) + "g}{}").format(n / f, s)


def str_to_num(s):
    s = s.strip()
    for suffix, f in _SUFFIX:
        if suffix and s.endswith(suffix):
            return float(s[:-1]) * f
    return float(s)
