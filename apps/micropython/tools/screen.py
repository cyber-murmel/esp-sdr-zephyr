#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The board's screen and touch panel from the host, over the console UART, while the REPL or an
app runs (src/console_remote.c, CONFIG_APP_CONSOLE_REMOTE).

    screen.py -p PORT shot out.png [--step N]    the screen as a PNG, every N-th pixel
    screen.py -p PORT tap X Y [--ms 80]
    screen.py -p PORT hold X Y --ms 2000
    screen.py -p PORT drag X1 Y1 X2 Y2 [--ms 400]
    screen.py -p PORT key CODE [--ms 80]         a key by its input code, pressed and released
    screen.py -p PORT count [--ms 10000]         the panel's frames and underruns over that time

Needs pyserial. A flat screen takes a second, a waterfall some 11 s; --step 2 a quarter of that."""

import argparse
import base64
import struct
import sys
import time
import zlib

import serial

START = b"\x1e"
SLOW = 115200
FAST = 921600
# The board's pause before it changes speed.
BAUD_S = 0.1


def open_port(path):
    s = serial.Serial()
    s.port = path
    s.baudrate = SLOW
    s.timeout = 0.2
    # Both asserted is what Linux drives on open: no edge, so the board keeps running.
    s.dtr = True
    s.rts = True
    s.open()
    return s


def command(s, text):
    s.write(START + text.encode() + b"\n")
    s.flush()


def png(path, width, height, rows):
    """RGB565 rows (lists of ints) to an 8-bit RGB PNG."""
    raw = bytearray()
    for r in rows:
        raw.append(0)
        for p in r:
            raw += bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                          (p & 31) * 255 // 31))

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def decode(how, text, width):
    data = base64.b64decode(text)
    words = struct.unpack("<{}H".format(len(data) // 2), data)
    if how == "L":
        return list(words)
    row = []
    for k in range(0, len(words), 2):
        row += [words[k + 1]] * words[k]
    return row[:width] + [0] * (width - len(row))


def shot(s, path, step):
    """The screen into a PNG; False if lines came damaged (those rows repeat the one before)."""
    command(s, "s {}".format(step))
    s.flush()
    time.sleep(BAUD_S * 1.5)
    s.baudrate = FAST
    s.reset_input_buffer()
    width = height = 0
    rows = []
    bad = 0
    t0 = time.monotonic()
    buf = b""
    done = False
    while not done and time.monotonic() - t0 < 300:
        if not width and time.monotonic() - t0 > 5:
            break
        buf += s.read(65536)
        lines = buf.split(b"\n")
        buf = lines.pop()
        for line in lines:
            line = line.strip()
            if line.startswith(START + b"FB"):
                width, height = (int(v) for v in line.split()[1:3])
                rows = [None] * height
            elif line.startswith(START + b"END"):
                done = True
            elif width and line:
                parts = line.split()
                try:
                    y = int(parts[0])
                    how = parts[1].decode()
                    if how == "=":
                        rows[y] = rows[y - 1]
                    elif sum(parts[2]) & 0xFF != int(parts[3], 16):
                        bad += 1
                    else:
                        rows[y] = decode(how, parts[2], width)
                except (ValueError, IndexError):
                    bad += 1
    time.sleep(BAUD_S * 1.5)
    s.baudrate = SLOW
    if not done:
        print("no end of the screen after {:.0f} s".format(time.monotonic() - t0))
    missing = 0
    for y in range(height):
        if rows[y] is None:
            missing += 1
            rows[y] = rows[y - 1] if y and rows[y - 1] else [0] * width
    if not width:
        return False
    png(path, width, height, rows)
    print("{}: {}x{}, {:.1f} s, {} damaged lines, {} rows missing".format(
        path, width, height, time.monotonic() - t0, bad, missing))
    return done and bad == 0 and missing == 0


def counts(s):
    command(s, "c")
    end = time.monotonic() + 3
    buf = b""
    while time.monotonic() < end:
        buf += s.read(1024)
        at = buf.rfind(START + b"COUNT ")
        if at >= 0 and b"\n" in buf[at:]:
            return [int(x) for x in buf[at:].split(b"\n")[0].split()[1:3]]
    raise SystemExit("no answer: an image without CONFIG_APP_CONSOLE_REMOTE?")


def touch(s, x, y, down):
    command(s, "t {} {} {}".format(x, y, 1 if down else 0))


def drag(s, x1, y1, x2, y2, ms):
    steps = max(2, ms // 20)
    for k in range(steps + 1):
        touch(s, x1 + (x2 - x1) * k // steps, y1 + (y2 - y1) * k // steps, True)
        time.sleep(ms / 1000 / steps)
    touch(s, x2, y2, False)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", required=True)
    ap.add_argument("what", choices=("shot", "tap", "hold", "drag", "key", "count"))
    ap.add_argument("args", nargs="*")
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--ms", type=int, default=None)
    a = ap.parse_args()
    s = open_port(a.port)
    if a.what == "shot":
        return 0 if shot(s, a.args[0], a.step) else 1
    if a.what == "count":
        first = counts(s)
        time.sleep((a.ms or 10000) / 1000)
        last = counts(s)
        print("{} frames, {} underruns in {} ms; {} and {} since the start".format(
            last[0] - first[0], last[1] - first[1], a.ms or 10000, last[0], last[1]))
        return 0
    v = [int(x) for x in a.args]
    if a.what in ("tap", "hold"):
        # A finger repeats its position while it lasts.
        ms = a.ms or (80 if a.what == "tap" else 2000)
        end = time.monotonic() + ms / 1000
        while time.monotonic() < end:
            touch(s, v[0], v[1], True)
            time.sleep(0.02)
        touch(s, v[0], v[1], False)
    elif a.what == "drag":
        drag(s, v[0], v[1], v[2], v[3], a.ms or 400)
    elif a.what == "key":
        command(s, "k {} 1".format(v[0]))
        time.sleep((a.ms or 80) / 1000)
        command(s, "k {} 0".format(v[0]))
    time.sleep(0.05)
    return 0


sys.exit(main())
