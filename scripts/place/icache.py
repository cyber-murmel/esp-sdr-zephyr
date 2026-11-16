#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The ESP32-S3 instruction cache's access and miss counters, through Zephyr's devmem shell.

    icache.py <port> [--seconds S] [--window W] [--period P] [--range START END] [--log FILE]
    icache.py --parse FILE...

Live: opens the image's shell (a CDC-ACM port, which does not reset the chip; the image needs
CONFIG_DEVMEM_SHELL), sets the counters' address window once (EXTMEM_IBUS_TO_FLASH_START/END_VADDR,
default 0x42000000 to 0x43ffffff, the flash and PSRAM code), then every P seconds clears them
(EXTMEM_CACHE_ACS_CNT_CLR, the I-cache bit), waits W seconds and reads EXTMEM_IBUS_ACS_CNT and
EXTMEM_IBUS_ACS_MISS_CNT. Both CPUs count. Prints one line per window and a summary; --log keeps
the shell's output, which --parse reads again later.
"""

import argparse
import re
import statistics
import sys
import time

START_REG, END_REG = 0x600C40B4, 0x600C40B8
CLR_REG, CLR_ICACHE = 0x600C40C4, 0x2
ACS_REG, MISS_REG = 0x600C40CC, 0x600C40C8


def parse(text):
    """(accesses, misses) per window from a log of "@@ <t> devmem <reg>" marks and replies."""
    rows, cur, want = [], {}, None
    for line in text.replace("\r", "").split("\n"):
        m = re.match(r"@@ [0-9.]+ devmem (0x[0-9a-fA-F]+)\s*$", line)
        if m:
            want = int(m.group(1), 16)
            continue
        m = re.search(r"Read value (0x[0-9a-fA-F]+)", line)
        if m and want is not None:
            cur[want] = int(m.group(1), 16)
            want = None
            if ACS_REG in cur and MISS_REG in cur:
                rows.append((cur[ACS_REG], cur[MISS_REG]))
                cur = {}
    return rows


def summary(rows, window):
    if not rows:
        return "no windows"
    acs = [r[0] for r in rows]
    mis = sorted(r[1] for r in rows)

    def q(p):
        return mis[min(len(mis) - 1, int(p * len(mis)))]

    return (f"{len(rows)} windows of {window} s: accesses mean {statistics.mean(acs) / 1e6:.2f} M, "
            f"misses mean {statistics.mean(mis) / 1e3:.1f} k, median {q(0.5) / 1e3:.1f} k, "
            f"p90 {q(0.9) / 1e3:.1f} k, max {mis[-1] / 1e3:.1f} k, "
            f"ratio {100.0 * sum(mis) / max(1, sum(acs)):.3f} %")


def live(args):
    import serial

    log = open(args.log, "wb") if args.log else None
    with serial.Serial(args.port, 115200, timeout=0.05) as s:
        t0 = time.monotonic()
        text = bytearray()

        def pump(until):
            while time.monotonic() < until:
                d = s.read(4096)
                if d:
                    text.extend(d)
                    if log:
                        log.write(d)

        def send(cmd):
            mark = f"\n@@ {time.monotonic() - t0:.3f} {cmd}\n".encode()
            text.extend(mark)
            if log:
                log.write(mark)
            s.write((cmd + "\n").encode())
            pump(time.monotonic() + 0.15)

        pump(t0 + 0.5)
        send(f"devmem 0x{START_REG:08X} 32 0x{args.range[0]:08x}")
        send(f"devmem 0x{END_REG:08X} 32 0x{args.range[1]:08x}")
        nxt, seen = time.monotonic(), 0
        while time.monotonic() - t0 < args.seconds:
            if time.monotonic() >= nxt:
                nxt += args.period
                send(f"devmem 0x{CLR_REG:08X} 32 {CLR_ICACHE}")
                pump(time.monotonic() + args.window)
                send(f"devmem 0x{ACS_REG:08X}")
                send(f"devmem 0x{MISS_REG:08X}")
                rows = parse(text.decode(errors="replace"))
                for a, m in rows[seen:]:
                    print(f"{time.monotonic() - t0:7.1f} s: accesses {a:10d}, misses {m:8d}, "
                          f"ratio {100.0 * m / max(1, a):.3f} %", flush=True)
                seen = len(rows)
            pump(min(nxt, t0 + args.seconds))
    if log:
        log.close()
    print(summary(parse(text.decode(errors="replace")), args.window))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("port", nargs="?")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--window", type=float, default=1.0)
    ap.add_argument("--period", type=float, default=5.0)
    ap.add_argument("--range", nargs=2, type=lambda v: int(v, 0), default=[0x42000000, 0x43ffffff])
    ap.add_argument("--log")
    ap.add_argument("--parse", nargs="+", metavar="FILE")
    args = ap.parse_args()
    if args.parse:
        for f in args.parse:
            text = open(f, "rb").read().decode(errors="replace")
            print(f"{f}: {summary(parse(text), args.window)}")
        return 0
    if not args.port:
        ap.error("a port, or --parse")
    live(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
