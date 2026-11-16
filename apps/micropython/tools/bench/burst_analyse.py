#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads burst_capture.py's output (hex lines with checksums, int16 I, Q) from a file or
stdin: the envelope per 100 samples, where the burst lies, its instantaneous frequency, and
what the Data link egg's GFSK receiver (modules/gnuradio/digital/_fsk.py) decodes from it.

usage: burst_analyse.py [FILE] [--rate HZ]
"""

import argparse
import array
import binascii
import importlib.util
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def load(lines):
    data, bad = b"", 0
    for line in lines:
        if not line.startswith("H"):
            continue
        _, h, c = line.split()
        b = binascii.unhexlify(h)
        bad += (sum(b) & 0xFFFF) != int(c, 16)
        data += b
    return data, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file", nargs="?")
    ap.add_argument("--rate", type=float, default=250e3)
    args = ap.parse_args()
    with (open(args.file) if args.file else sys.stdin) as f:
        data, bad = load(f)
    print("bytes", len(data), "bad lines", bad)
    x = np.frombuffer(data, dtype=np.int16).astype(float)
    z = x[0::2] + 1j * x[1::2]
    env = np.abs(z)[: len(z) // 100 * 100].reshape(-1, 100).mean(1)
    print("envelope /100:", " ".join("%d" % v for v in env))
    on = np.where(env > 3 * np.median(env))[0]
    if len(on) == 0:
        print("no burst")
        return
    a, b = on.min() * 100, on.max() * 100 + 100
    print("burst samples", a, b, b - a)
    seg = z[a:b] - np.mean(z[a:b])
    f = np.angle(seg[1:] * np.conj(seg[:-1])) * args.rate / 1e3 / (2 * np.pi)
    print("inst freq kHz /10:", " ".join("%d" % v for v in f[::10][:150]))
    spec = importlib.util.spec_from_file_location(
        "_fsk", os.path.join(HERE, "..", "..", "modules", "gnuradio", "digital", "_fsk.py"))
    fsk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fsk)
    code, code_len = fsk.code_bits("01" * 16 + "11100100001011000110011001011101")
    rx = fsk.Receiver(10, code, code_len, 8)
    iq = array.array("h", np.round(x).astype(np.int16).tobytes())
    print("decoded", [(fr.errors, bytes(fr.data).hex()) for fr in rx.process(iq, len(z), 0)])


if __name__ == "__main__":
    main()
