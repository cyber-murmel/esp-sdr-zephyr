# SPDX-License-Identifier: GPL-3.0-or-later
"""The bench's 8x8 RF switch, which the ESP32-C5 drives: two sides of eight ports, the selected
port of one side joined to the selected port of the other, the rest terminated in 50 ohm. Side
A on GPIO 0, 1, 2, side B on GPIO 8, 9, 10: bits A, B, C of the port number minus one (port n
is 0b{C}{B}{A} = n - 1). Side A port 4 is the C5 itself; side B port 1 the S3 with the
display, 2 the C6, 5 a 50 ohm load, 6 open, 7 short.

    select(a=4, b=1)   either side or both; returns path()
    path()             (a, b) as the pins are set now

At the board's first start (soft resets keep the pins) boot() selects A4-B1, the C5 to the S3,
or the path /FLASH:/rfswitch holds ("4 1")."""

from machine import Pin

PINS = {"a": (0, 1, 2), "b": (8, 9, 10)}
DEFAULT = (4, 1)
# The C5's one GPIO controller (its devicetree node).
GPIO = "gpio@60091000"

_pins = {}


def _pin(n):
    p = _pins.get(n)
    if p is None:
        # Output with the input buffer on (value() reads the level back); the level stays.
        p = _pins[n] = Pin((GPIO, n), Pin.OUT)
    return p


def select(a=None, b=None):
    """Side A to port a, side B to port b (1 to 8); a side given as None stays."""
    for side, port in (("a", a), ("b", b)):
        if port is None:
            continue
        port = int(port)
        if not 1 <= port <= 8:
            raise ValueError("ports 1 .. 8")
        for bit, n in enumerate(PINS[side]):
            _pin(n).value(((port - 1) >> bit) & 1)
    return path()


def path():
    """(a, b): the ports the pins select now."""
    out = []
    for side in ("a", "b"):
        v = 0
        for bit, n in enumerate(PINS[side]):
            v |= (_pin(n).value() & 1) << bit
        out.append(v + 1)
    return tuple(out)


def boot():
    """The path at the board's first start: /FLASH:/rfswitch ("a b"), else A4-B1."""
    a, b = DEFAULT
    try:
        with open("/FLASH:/rfswitch") as f:
            a, b = (int(x) for x in f.read().split()[:2])
    except (OSError, ValueError):
        pass
    return select(a, b)
