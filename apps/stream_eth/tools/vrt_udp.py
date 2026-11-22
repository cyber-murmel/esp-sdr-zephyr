#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fallback of vrt_udp.c for a few MS/s: subscribe ("VRTS", the board's "VRTC" + cookie, "VRTS" + cookie), renew, check the packet counter and
the sample index, write cs8 with zeros in the gaps. No settings: use vrt_udp for those.

Usage: vrt_udp.py <board address> [-p 4992] [-t seconds] [-o file]
"""
import argparse
import socket
import struct
import sys
import time

sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "..", "stream", "tools"))
from vrt_rx import parse  # noqa: E402  (the stream app's packet parser)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("-p", "--port", type=int, default=4992)
    ap.add_argument("-t", "--seconds", type=float, default=0)
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    ai = socket.getaddrinfo(a.host, a.port, type=socket.SOCK_DGRAM)
    ai = sorted(ai, key=lambda x: x[0] != socket.AF_INET6)[0]
    s = socket.socket(ai[0], socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.connect(ai[4])
    s.settimeout(0.25)
    out = open(a.out, "wb") if a.out else None
    rate = 0.0
    last = None
    nxt = None
    pk = lost = board = samples = restarts = 0
    start = renew = time.monotonic()
    cookie = None
    s.send(b"VRTS")
    while not a.seconds or time.monotonic() - start < a.seconds:
        if time.monotonic() - renew >= 0.25:
            s.send(b"VRTS" + cookie if cookie else b"VRTS")
            renew = time.monotonic()
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if d[:3] == b"VRT":
            if d[:4] == b"VRTC" and len(d) == 12:
                cookie = d[4:]
            elif d[:4] == b"VRTB":
                print("vrt_udp.py: another host has the stream", file=sys.stderr)
                return 3
            continue
        try:
            p = parse(d)
        except (ValueError, struct.error):
            continue
        if p["type"] in (4, 5):
            rate = p.get("sample_rate", rate)
            continue
        if p["type"] != 1 or not rate:
            continue
        pk += 1
        t = p["trailer_word"]
        loss = bool(t >> 24 & 1 and t >> 12 & 1)
        idx = p["seconds"] * int(rate) + (p["fraction"] * int(rate) + 500000000000) // 1000000000000
        if last is not None and (p["count"] - last) & 15 != 1:
            lost += 1
        last = p["count"]
        if nxt is not None and idx > nxt + int(rate) * 5:
            # more than 5 s ahead: a restart of the board or the stream, not a gap to fill with zeros
            restarts += 1
            print("vrt_udp.py: the time axis jumped %d samples: a restart" % (idx - nxt), file=sys.stderr)
        elif nxt is not None and idx > nxt:
            if loss:
                board += idx - nxt
            if out:
                out.write(bytes(2 * (idx - nxt)))
        nxt = idx + len(p["body"]) // 2
        samples += len(p["body"]) // 2
        if out:
            out.write(p["body"])
    if cookie:
        s.send(b"VRTX" + cookie)
    print(f"vrt_udp.py: {pk} packets, {samples} samples, counter gaps {lost}, board drops {board} samples, restarts {restarts}",
          file=sys.stderr)
    return 0 if pk and not lost and not board and not restarts else 1


if __name__ == "__main__":
    sys.exit(main())
