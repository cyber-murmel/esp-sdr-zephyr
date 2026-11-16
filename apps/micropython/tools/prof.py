#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where CPU 0 spends its time, from the sampling profiler (CONFIG_APP_PROF, cmod/esdr_prof.c).

    prof.py -p PORT --elf zephyr.elf [--app NAME] [--seconds 20] [--top 40] [--save FILE]
            [--rom-ld DIR] [--callers 8]
    prof.py --elf zephyr.elf --load FILE [--top 40]

Over the REPL on the console UART: esdr.prof_start(), then the app NAME as the launcher starts
it (a soft reset; the samples are kept) or, without --app, whatever runs; after --seconds Ctrl-C
ends the app, esdr.prof_stop() and prof_read() bring the samples back. They are counted by the
function each PC falls in (nm on the image, and the ROM's linker scripts in --rom-ld for ROM code)
and printed with their share; for the --callers top ones, by the function their return address
falls in, which for a leaf such as memcpy() is its caller. --save keeps the raw samples for
another look with --load. Needs pyserial and the toolchain's nm (NM, or the one next to the
compiler in CMakeCache.txt beside the image)."""

import argparse
import base64
import bisect
import collections
import os
import re
import struct
import subprocess
import sys
import time

import serial


def open_port(path):
    s = serial.Serial()
    s.port = path
    s.baudrate = 115200
    s.timeout = 0.05
    # Both asserted is what Linux drives on open: no edge, so the board keeps running.
    s.dtr = True
    s.rts = True
    s.open()
    return s


def read_until(s, pattern, seconds):
    end = time.monotonic() + seconds
    buf = b""
    while time.monotonic() < end:
        data = s.read(4096)
        if data:
            buf += data
            if pattern in buf:
                return buf
    return buf


def repl(s, line, seconds=5):
    s.write(line.encode() + b"\r")
    return read_until(s, b"\n>>> ", seconds).decode("utf-8", "replace")


def prompt(s):
    s.write(b"\x03\x03")
    time.sleep(0.5)
    s.reset_input_buffer()
    s.write(b"\r")
    if b">>> " not in read_until(s, b">>> ", 3):
        sys.exit("prof.py: no REPL prompt")


def record(port, app, seconds):
    s = open_port(port)
    prompt(s)
    repl(s, "import esdr")
    repl(s, "esdr.prof_start()")
    if app:
        repl(s, "import launcher")
        s.write("launcher.run({!r})\r".format(app).encode())
    print("prof.py: sampling for {} s".format(seconds), file=sys.stderr)
    time.sleep(seconds)
    prompt(s)
    out = repl(s, "import esdr; print('PROF', esdr.prof_stop())")
    m = re.search(r"PROF \((\d+), (\d+)\)", out)
    if not m:
        sys.exit("prof.py: no prof_stop() answer: " + out[-300:])
    n, hz = int(m.group(1)), int(m.group(2))
    repl(s, "import binascii")
    words = []
    for first in range(0, n, 1024):
        out = repl(s, "print('B', binascii.b2a_base64(esdr.prof_read({}, 1024)))".format(first),
                   10)
        m = re.search(r"B b'([A-Za-z0-9+/=]*)\\n'", out)
        if not m:
            sys.exit("prof.py: bad read at {}: {}".format(first, out[-200:]))
        data = base64.b64decode(m.group(1))
        words += struct.unpack("<{}I".format(len(data) // 4), data)
    return list(zip(words[0::2], words[1::2])), hz


def find_nm(elf):
    if os.environ.get("NM"):
        return os.environ["NM"]
    cache = os.path.join(os.path.dirname(os.path.abspath(elf)), "..", "CMakeCache.txt")
    if os.path.exists(cache):
        for line in open(cache):
            if line.startswith("CMAKE_NM:"):
                return line.split("=", 1)[1].strip()
    return "nm"


def symbols(elf, rom_ld):
    out = subprocess.run([find_nm(elf), "-S", "--defined-only", elf], check=True,
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in "tTwW":
            syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    # ROM functions: their addresses only, each taken to end where the next begins.
    rom = []
    if rom_ld and os.path.isdir(rom_ld):
        for name in sorted(os.listdir(rom_ld)):
            if name.endswith(".ld"):
                text = open(os.path.join(rom_ld, name)).read()
                for m in re.finditer(r"^\s*(?:PROVIDE\s*\(\s*)?(\w+)\s*=\s*(0x[0-9a-fA-F]+)", text,
                                     re.M):
                    rom.append((int(m.group(2), 16), m.group(1)))
    rom.sort()
    for k, (a, n) in enumerate(rom):
        end = rom[k + 1][0] if k + 1 < len(rom) else a + 4
        if end > a:
            syms.append((a, end - a, "rom:" + n))
    syms.sort()
    return syms


def report(pairs, hz, elf, top, rom_ld, callers):
    syms = symbols(elf, rom_ld)
    starts = [a for a, _, _ in syms]

    def name(pc):
        k = bisect.bisect_right(starts, pc) - 1
        if k >= 0 and pc < syms[k][0] + max(syms[k][1], 1):
            return syms[k][2]
        return "?{:08x}".format(pc & ~0xfff)

    count = collections.Counter()
    by = collections.defaultdict(collections.Counter)
    for pc, ra in pairs:
        n = name(pc)
        count[n] += 1
        by[n][name(ra) if ra else "-"] += 1
    total = max(len(pairs), 1)
    print("{} samples, {:.1f} s at {} Hz".format(len(pairs), len(pairs) / hz, hz))
    for n, c in count.most_common(top):
        print("{:6.2f} %  {:6d}  {}".format(100.0 * c / total, c, n))
    for n, c in count.most_common(callers):
        print("\n{} ({:.1f} %), by return address:".format(n, 100.0 * c / total))
        for r, rc in by[n].most_common(6):
            print("  {:6.2f} %  {}".format(100.0 * rc / total, r))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--app")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--save")
    ap.add_argument("--load")
    ap.add_argument("--rom-ld", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "..", "..", "..", "modules", "hal", "espressif", "components", "esp_rom",
                    "esp32s31", "ld"))
    ap.add_argument("--callers", type=int, default=8)
    a = ap.parse_args()
    if a.load:
        raw = open(a.load, "rb").read()
        hz = struct.unpack("<I", raw[:4])[0]
        words = struct.unpack("<{}I".format(len(raw) // 4 - 1), raw[4:])
        pairs = list(zip(words[0::2], words[1::2]))
    else:
        if not a.port:
            sys.exit("prof.py: -p PORT or --load FILE")
        pairs, hz = record(a.port, a.app, a.seconds)
        if a.save:
            flat = [w for p in pairs for w in p]
            with open(a.save, "wb") as f:
                f.write(struct.pack("<I", hz) + struct.pack("<{}I".format(len(flat)), *flat))
    report(pairs, hz, a.elf, a.top, a.rom_ld, a.callers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
