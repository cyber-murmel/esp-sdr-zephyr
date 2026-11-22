#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A fake S31 board and a stand-in for vrt_udp, for tests without hardware.

  fake_vrt.py serve [--port P] [--rate R] [--tone HZ] [--drop 50-52,200] [--packets N] [--pps N]
      plays the board side of the cookie handshake (VRTS, VRTC, VRTS + cookie, VRTK; VRTB, VRTT, VRTX), then sends VRT context and signal data packets
      (cs8, a complex tone) to the subscriber. Packets listed by --drop are numbered
      but not sent, which is how a real loss looks to the receiver.
  fake_vrt.py vrt_udp HOST:PORT [-r rate] [-f freq] [-g gain|auto] [-t seconds] [-o file|-]
      the command line of the C receiver: writes raw cs8 with gaps zero filled, status on stderr.
"""
import argparse
import hashlib
import socket
import struct
import sys
import time

import numpy as np

STREAM_ID = 0x5D5D0001
SPP = 720                      # samples per packet: 1440 byte payload, LAN MTU 1492 (20 byte header, 4 byte trailer)
Q20 = 1 << 20


def data_packet(count, sec, ps, payload, lost=False):
    """Type 1 (data with stream ID), trailer, TSI=UTC, TSF=real time; trailer bit 24/12 = sample loss."""
    words = 1 + 1 + 1 + 2 + len(payload) // 4 + 1
    hdr = 1 << 28 | 1 << 26 | 1 << 22 | 3 << 20 | (count & 15) << 16 | words
    trailer = (1 << 24 | 1 << 12) if lost else 0
    return struct.pack(">IIIQ", hdr, STREAM_ID, sec, ps) + payload + struct.pack(">I", trailer)


def context_packet(count, freq, rate, gain=-1, sec=0, ps=0, changed=True, epoch=0):
    """The board's context packet, field for field as ctl_build_context() builds it with libvrt: type 4,
    nd0, TSI other, TSF real time, CIF0 (change, bandwidth, rf frequency, gain, sample rate, state and event with the settings epoch in
    its bits 7..0, payload format,
    CIF1 enable), CIF1 (v49 spec compliance), the fields in that order, the spec word last (4: VITA 49.2).
    Frequencies in Q44.20 Hz, gain Q9.7, payload format link efficient complex cartesian signed, 8 bit."""
    q = lambda hz: list(struct.unpack(">II", struct.pack(">q", round(hz * Q20))))
    cif0 = changed << 31 | 1 << 29 | 1 << 27 | 1 << 23 | 1 << 21 | 1 << 16 | 1 << 15 | 1 << 1
    words = [STREAM_ID, sec, ps >> 32, ps & 0xFFFFFFFF, cif0, 1 << 3] + q(rate) + q(freq) + \
        [round(gain * 128) & 0xFFFF] + q(rate) + [epoch & 0xFF, 0xA00001C7, 0, 4]
    hdr = 4 << 28 | 1 << 25 | 3 << 22 | 2 << 20 | (count & 15) << 16 | (1 + len(words))
    return struct.pack(f">{1 + len(words)}I", hdr, *words)


def decode_command(pkt):
    """(freq, gain, rate) of a command packet, None where absent (CIF0 bits 27, 23, 21)."""
    w = struct.unpack(f">{len(pkt) // 4}I", pkt)
    cif0, i, out = w[4], 5, []
    for bit, n in ((27, 2), (23, 1), (21, 2)):
        if cif0 >> bit & 1:
            raw = w[i] if n == 1 else struct.unpack(">q", struct.pack(">II", w[i], w[i + 1]))[0]
            out.append(((raw - (raw >> 15 & 1) * 65536) / 128 if n == 1 else raw / Q20))
            i += n
        else:
            out.append(None)
    return tuple(out)


RATES = (40e6, 20e6, 16e6, 10e6, 8e6, 5e6, 4e6, 3.2e6, 2.5e6, 2e6, 1.6e6, 1.25e6, 1e6, 0.8e6, 0.64e6)
FREQ_RANGE, GAIN_MAX = (2.2e9, 2.9e9), 71           # src/radio.c; 71 is what the S31 reports at boot
OUT_OF_RANGE = 0x10000000


def ack_packet(message_id, executed, errors=None):
    """The execution acknowledge of ctl.c send_ack(): CAM execution (bit 19), executed (bit 10) and, when
    the command asked for error words and some setting is refused, errors (bit 16), EIF0 and one response
    word per refused field (CIF0 order: rf frequency 27, gain 23, sample rate 21, payload format 15)."""
    errors = errors or {}
    bits = {"freq": 27, "gain": 23, "rate": 21}
    eif0 = sum(1 << bits[k] for k in errors)
    words = [STREAM_ID, 1 << 19 | executed << 10 | bool(errors) << 16, message_id]
    if errors:
        words += [eif0] + [OUT_OF_RANGE for k in sorted(errors, key=lambda k: -bits[k])]
    return struct.pack(f">{1 + len(words)}I", 6 << 28 | 1 << 26 | (1 + len(words)), *words)


def answer_command(s, d, addr, ctx):
    """handle_command() of src/ctl.c for the subscriber: refuse a frequency out of range or not whole, a
    gain not -1 or a whole index in range, a rate not in the list; execute only when nothing is refused
    (permit_errors, CAM bit 25, executes the rest); acknowledge with executed and the error words."""
    cam, mid = struct.unpack(">II", d[8:16])
    freq, gain, rate = decode_command(d)
    bad = {}
    if freq is not None and not (FREQ_RANGE[0] <= freq <= FREQ_RANGE[1] and freq == int(freq)):
        bad["freq"] = freq
    if gain is not None and not (gain == -1 or (gain == int(gain) and 0 <= gain <= GAIN_MAX)):
        bad["gain"] = gain
    if rate is not None and rate not in RATES:
        bad["rate"] = rate
    executed = not bad or bool(cam >> 25 & 1)
    if executed:
        for key, v in (("freq", freq), ("gain", gain), ("rate", rate)):
            if v is not None and key not in bad:
                ctx[key] = v
    if cam >> 19 & 1:                                        # execution acknowledge wanted
        s.sendto(ack_packet(mid, executed, bad if cam >> 16 & 1 else None), addr)
    if executed:
        s.sendto(context_packet(0, **ctx), addr)
    print("fake_vrt: command", decode_command(d), "refused" if bad else "executed", sorted(bad), file=sys.stderr, flush=True)


def tone_block(n0, n, tone, rate, amp=0.5):
    """cs8 payload of samples n0..n0+n of exp(j 2 pi tone t), amplitude amp of full scale (128)."""
    ph = 2 * np.pi * tone / rate * (np.arange(n0, n0 + n) % round(rate / np.gcd(round(rate), round(tone))))
    iq = np.empty(2 * n, np.int8)
    iq[0::2] = np.round(amp * 128 * np.cos(ph))
    iq[1::2] = np.round(amp * 128 * np.sin(ph))
    return iq.tobytes()


def parse_map(spec):
    """"100:3,200:1" to {100: 3, 200: 1}"""
    return {int(k): int(v) for k, _, v in (p.partition(":") for p in spec.split(",") if p)}


def parse_drop(spec):
    out = set()
    for part in filter(None, spec.split(",")):
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def cookie_of(addr):
    return hashlib.blake2b(repr(addr).encode(), digest_size=8).digest()


def serve(a):
    """The board side: VRTS -> VRTC + cookie; VRTS + cookie subscribes (VRTK) or renews or says VRTB;
    VRTT + cookie takes over; VRTX + cookie stops. --other-host: someone else holds the stream."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
    s.bind(("127.0.0.1", a.port))
    sub = ("127.0.0.1", 1) if a.other_host else None
    ctx = {"freq": a.freq, "rate": a.rate, "gain": -1}

    def control(d, addr):
        nonlocal sub
        if d[:1] and d[0] >> 4 == 6 and addr == sub:         # a command from the subscriber
            answer_command(s, d, addr, ctx)
            return
        verb, ck = d[:4], d[4:]
        if verb == b"VRTS" and ck != cookie_of(addr):            # no cookie or a stale one
            s.sendto(b"VRTC" + cookie_of(addr), addr)
        elif ck != cookie_of(addr):
            pass
        elif verb == b"VRTX" and sub == addr:
            sub = None
        elif verb == b"VRTT" or (verb == b"VRTS" and sub is None):
            sub = addr
            s.sendto(b"VRTK", addr)
        elif verb == b"VRTS" and sub != addr:
            s.sendto(b"VRTB", addr)

    while sub is None or sub[1] == 1:                  # until a host holds the stream (not the pretend one)
        control(*s.recvfrom(64))
    dest = sub
    pps = a.pps or a.rate / SPP
    period = round(a.rate / np.gcd(round(a.rate), round(a.tone)))
    cache, shift, offset, late = {}, 0, 0, []
    s.sendto(context_packet(0, **ctx), dest)
    s.setblocking(False)
    t0 = time.perf_counter()
    for i in range(a.packets):
        try:                                       # renewals are read; VRTX + cookie stops the stream
            while True:
                control(*s.recvfrom(64))
                if sub is None:
                    return
        except BlockingIOError:
            pass
        due = t0 + i / pps
        while time.perf_counter() < due:
            time.sleep(max(0, min(0.0005, due - time.perf_counter() - 0.0002)))
        if i == a.evict:                           # another host takes the stream: VRTB to our renewals
            sub = ("127.0.0.1", 1)
        shift += a.jump.get(i, 0) * SPP
        offset += a.restart.get(i, 0) * round(a.rate)
        if i in a.drop or sub != dest:
            continue
        m = i * SPP + shift                        # the tone's sample index: jumps skip samples, restarts do not
        if m % period not in cache:
            cache[m % period] = tone_block(m, SPP, a.tone, a.rate)
        sec, ps = divmod((m + offset + 100 * round(a.rate)) * 10**12 // round(a.rate), 10**12)
        out = [data_packet(i, sec, ps, cache[m % period])]
        if i == a.swap:                            # arrives after the next one
            late = out
            continue
        s.sendto(out[0], dest)
        if late:
            s.sendto(late[0], dest)
            late = []
        if i % 1000 == 999:
            s.sendto(context_packet(i // 1000, **ctx, changed=False), dest)
    time.sleep(0.2)
    s.sendto(b"", dest)        # end marker the stand-in understands: an empty datagram


def vrt_udp(a):
    """Like the C tool: VRTS, cookie, VRTS + cookie renewed every second, exit 3 on VRTB, VRTX + cookie."""
    host, _, port = a.addr.rpartition(":")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.connect((host, int(port or 4992)))
    s.settimeout(0.25)
    print("vrt_udp: IPv4", file=sys.stderr)
    out = sys.stdout.buffer if a.o == "-" else open(a.o, "wb")
    last, got, gaps, cookie = None, 0, 0, None
    t_end, renew, idle = time.time() + a.t if a.t else None, 0, time.time()
    while (t_end is None or time.time() < t_end) and time.time() - idle < 2:
        if time.time() - renew >= 1:
            s.send(b"VRTS" + (cookie or b""))
            renew = time.time()
        try:
            p = s.recv(65536)
        except (socket.timeout, ConnectionRefusedError):
            continue
        idle = time.time()
        if not p:
            break
        if p[:3] == b"VRT":
            if p[:4] == b"VRTC":
                cookie = p[4:]
                s.send(b"VRTS" + cookie)
            elif p[:4] == b"VRTB":
                print("vrt_udp: another host has the stream", file=sys.stderr)
                sys.exit(3)
            continue
        if p[0] >> 4 != 1:
            continue
        cnt, body = p[1] & 15, p[20:-4]
        if last is not None and cnt != (last + 1) & 15:
            miss = (cnt - last - 1) & 15
            gaps += miss
            out.write(bytes(miss * len(body)))
        out.write(body)
        last, got = cnt, got + 1
    if cookie:
        s.send(b"VRTX" + cookie)
    out.flush()
    print(f"vrt_udp: {got} data packets, {gaps} lost", file=sys.stderr)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("serve")
    p.add_argument("--port", type=int, default=4992)
    p.add_argument("--rate", type=float, default=1e6)
    p.add_argument("--freq", type=float, default=2.44e9)
    p.add_argument("--tone", type=float, default=125e3)
    p.add_argument("--drop", type=parse_drop, default=set())
    p.add_argument("--packets", type=int, default=1000)
    p.add_argument("--pps", type=float, default=0)
    p.add_argument("--jump", type=parse_map, default={}, help="PKT:N the board drops N packets of samples, counter goes on")
    p.add_argument("--restart", type=parse_map, default={}, help="PKT:S the time index jumps by S seconds (a restart)")
    p.add_argument("--swap", type=int, default=-1, help="PKT: it arrives after the next packet")
    p.add_argument("--evict", type=int, default=-1, help="PKT: another host takes the stream here")
    p.add_argument("--other-host", action="store_true", help="another host holds the stream: VRTB until VRTT")
    p.set_defaults(fn=serve)
    p = sub.add_parser("vrt_udp")
    p.add_argument("addr")
    p.add_argument("-r", type=float)
    p.add_argument("-f", type=float)
    p.add_argument("-g")
    p.add_argument("-t", type=float, default=0)
    p.add_argument("-o", default="-")
    p.set_defaults(fn=vrt_udp)
    a = ap.parse_args()
    a.fn(a)
