#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bench check of the gapless receiver (ESDR_MODE_GAPLESS on the WHY2025 badge's C6, through
apps/osmosdr_bridge on its P4): a second board's MicroPython (the Communicator badge, an S3)
loops a carrier through its DAC engine, every window is kept (a fixed threshold below the
carrier), the staging ring then drops windows in bursts, and the carrier's phase across all
those gaps must hold: one sample off moves it by 2 pi f / fs. Also fails on ring errors
(restarts) and on segments out of order.

  gapless_carrier.py --tx-port /dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_MicroPython_<sn>-if00
                     [--esdr-usb esdr_usb] [--seconds 20] [--offset-mhz 20] [--gain 40]
                     [--tx-step 0]

Puts a carrier on air (2.4 GHz ISM band by default, the weakest transmit step: 23 raw units
with the badges on one desk, 435 at step 6). Fails without a carrier well over the noise.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gapless  # noqa: E402

# The transmitter's tone: 100 kHz over its loop rate, exactly one cycle in 800 samples.
TX_RATE = 80_000_000
TX_TONE_HZ = 100_000


def repl(port, stmts, wait=2.0):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.write(b"\r\x03\x03\r")
    time.sleep(0.4)
    s.read(65536)
    out = b""
    for stmt in stmts:
        s.write(stmt.encode() + b"\r")
        end = time.time() + wait
        while time.time() < end:
            out += s.read(4096)
    s.close()
    return out.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tx-port", required=True)
    ap.add_argument("--esdr-usb", default="esdr_usb")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--center", type=float, default=2440e6)
    ap.add_argument("--offset-mhz", type=float, default=20)
    ap.add_argument("--gain", type=int, default=40, help="receive gain index")
    ap.add_argument("--tx-step", type=int, default=0, help="transmit power step (0 weakest)")
    a = ap.parse_args()

    tx_hz = int(a.center + a.offset_mhz * 1e6)
    out = repl(a.tx_port, ["import esdr", "esdr.init()",
                           f"esdr.tx_loop({TX_RATE}, {tx_hz}, {a.tx_step}, gen=(102, "
                           f"{TX_TONE_HZ / TX_RATE}, "
                           "0.5, 0.0, 0.0))", "print('TX', esdr.tx_busy())"], wait=3.0)
    if "TX True" not in out:
        sys.exit(f"the transmitter did not start:\n{out}")
    try:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "blocks.bin")
            r = subprocess.run([a.esdr_usb, "-t", str(a.seconds), "-f", str(a.center), "-g",
                                str(a.gain), "-L", "1", "-o", path, "gapless"],
                               capture_output=True, text=True)
            log = r.stdout + r.stderr
            print("\n".join(l for l in log.splitlines() if l.startswith(("total", "device"))))
            m = re.search(r"ring errors (\d+)", log)
            errors = int(m.group(1)) if m else -1
            segs = list(gapless.segments(path))
    finally:
        repl(a.tx_port, ["esdr.tx_stop()"])

    import numpy as np
    idx = np.array([s[0] for s in segs], dtype=np.int64)
    end = idx + np.array([len(s[7]) for s in segs], dtype=np.int64)
    backwards = int(np.sum(idx[1:] < end[:-1]))

    class Args:
        min_pairs = 700
        plot = None

    phase_ok = gapless.tone_check(segs, Args)
    print(f"ring errors {errors}, segments out of order {backwards}")
    if errors != 0 or backwards != 0 or not phase_ok:
        sys.exit("FAIL")
    print("PASS")


if __name__ == "__main__":
    main()
