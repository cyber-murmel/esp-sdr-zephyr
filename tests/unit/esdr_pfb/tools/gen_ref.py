#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference outputs for tests/unit/esdr_pfb, from the channelizer's definition in numpy.

    y_k[m] = sum_n h[n] x[m D - n] exp(-j tau k (m D - n) / M),  D = M / 2,

with x the pair of absolute index i, 0 where none was received, evaluated directly (no polyphase
split, no FFT) over a gapped int8 stream for every supported M, T = 8. Each segment has its own
shift and DC: a sample is (item << shift) + (2^shift - 1) / 2 - dc, I and Q apart. The C6 packs
raw >> shift (a floor), so an item stands for the middle of its step; that it takes the DC off
without the floor's bias is checked against a model of the C6's quantizer. Frame m exists when
pair m D was received; it is complete when its whole window was. The prototype is designed as
esdr_pfb.c does (Kaiser-windowed sinc, cutoff fs / (2 M), beta for 60 dB, DC gain 1) and its
response is checked: -6 dB at fs / (2 M), at least 60 dB down from fs / M on for T = 8.

Writes src/pfb_ref.h next to this directory (or the path given). Needs numpy:
    nix-shell -p "python3.withPackages(ps: [ps.numpy])" --run "python3 tools/gen_ref.py"
"""

import argparse
import math
import pathlib

import numpy as np

TAU = 2.0 * math.pi
CHANNELS = (16, 20, 32, 40, 64, 80)
TAPS = 8
STOP_DB = 60.0
# First pair's absolute index: not a multiple of any D or M, beyond 32 bits.
I0 = (1 << 33) + 12345
SEED = 1
# Segments: (offset from I0, pairs, shift, DC of I, DC of Q). Gaps of 100 (zeros for every M),
# 700 (a new run for every M), 300 (a new run for M 16, 20 and 32 only, zeros for the others),
# none, 3. The DC (raw units, int8 as esdr_seg_hdr carries it) changes at every segment.
SEGS = ((0, 700, 0, 37, -21), (800, 500, 2, -80, 64), (2000, 600, 1, 5, 3),
        (2900, 500, 0, -128, 127), (3400, 400, 2, 80, -6), (3800, 37, 1, -1, 90),
        (3840, 1000, 0, 19, -45))
# Full frames kept per M: all M outputs of these frames (indices into the frame list).
FULL = 4


def kaiser_beta(db):
    if db > 50.0:
        return 0.1102 * (db - 8.7)
    if db >= 21.0:
        return 0.5842 * (db - 21.0) ** 0.4 + 0.07886 * (db - 21.0)
    return 0.0


def design(m, t, stop_db=STOP_DB):
    l = m * t
    off = np.arange(l) - (l - 1) / 2.0
    h = np.sinc(off / m) * np.kaiser(l, kaiser_beta(stop_db))
    return h / h.sum()


def response_db(h, f):
    """|H(f)| in dB; h is symmetric, so H is real about its centre."""
    off = np.arange(len(h)) - (len(h) - 1) / 2.0
    return 20.0 * np.log10(np.abs(np.cos(TAU * np.outer(f, off)) @ h))


def stop_band_db(h, m):
    """Highest response from fs / M to fs / 2, on a grid of 1 / (8 L)."""
    f = np.arange(1.0 / m, 0.5, 1.0 / (8.0 * len(h)))
    return float(response_db(h, f).max())


def stream():
    """The test's items: an LCG, the top byte of each step, I then Q of offset o at 2 o."""
    n = 2 * (SEGS[-1][0] + SEGS[-1][1])
    x = SEED
    items = np.empty(n, dtype=np.int64)
    for i in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        b = x >> 24
        items[i] = b - 256 if b >= 128 else b
    return items


def c6_item(raw, shift):
    """The C6's packing (esp_sdr_gapless.c): raw >> shift, a floor, saturated to int8."""
    return np.clip(raw >> shift, -128, 127)


def sample(items, shift, dc):
    """What an item stands for: the middle of its step under the C6's floor, less the DC."""
    return items * float(1 << shift) + 0.5 * ((1 << shift) - 1) - dc


def check_floor():
    """Raw values about any DC that fill every step alike come back with no DC, at every shift."""
    d = np.array([-4, -3, -2, -1, 1, 2, 3, 4])
    for shift in range(3):
        # Shift 0 saturates within 4 of the int8 range.
        for dc in range(-124, 124) if shift == 0 else range(-128, 128):
            assert sample(c6_item(dc + d, shift), shift, dc).sum() == 0.0, (shift, dc)


def checksum_angles(m):
    # Golden ratio steps: every channel weighs differently in the phase-sensitive checksum.
    k = np.arange(m)
    return TAU * np.modf(k * 0.6180339887498949)[0]


def reference(m, items):
    d = m // 2
    h = design(m, TAPS)
    l = len(h)
    end = SEGS[-1][0] + SEGS[-1][1]
    # Pairs by offset, with l of zeros before offset 0.
    x = np.zeros(end + l, dtype=complex)
    known = np.zeros(end + l, dtype=bool)
    for off, pairs, shift, dc_i, dc_q in SEGS:
        o = np.arange(off, off + pairs)
        x[o + l] = sample(items[2 * o], shift, dc_i) + 1j * sample(items[2 * o + 1], shift, dc_q)
        known[o + l] = True
    w = np.exp(-1j * TAU * np.arange(m) / m)
    k = np.arange(m)
    th = checksum_angles(m)
    frames = []
    last = None
    m_first = -(-I0 // d)
    for mm in range(m_first, (I0 + end + d - 1) // d):
        n_new = mm * d
        o = n_new - I0
        if o < 0 or o >= end or not known[o + l]:
            continue
        win = np.arange(o, o - l, -1) + l
        # (k (N - n)) mod M exactly, whatever the size of N.
        e = (n_new - np.arange(l)) % m
        y = w[(k[:, None] * e[None, :]) % m] @ (h * x[win])
        flags = (1 if known[win].all() else 0) | (2 if last is None or mm != last + 1 else 0)
        last = mm
        frames.append((mm, flags, y))
    pw = np.array([np.sum(np.abs(y) ** 2) for _, _, y in frames])
    ws = np.array([np.sum(y.real * np.cos(th) + y.imag * np.sin(th)) for _, _, y in frames])
    # The full frames: a run's first, its first complete, the frame before that, the last.
    sel = [0]
    for i in range(1, len(frames)):
        if frames[i][1] & 1 and not frames[i - 1][1] & 1:
            sel += [i - 1, i]
            break
    sel.append(len(frames) - 1)
    sel = sorted(set(sel))[:FULL]
    return h, frames, pw, ws, sel


def c_float(a):
    """A float literal of 7 significant digits: always with a point or an exponent."""
    t = "%.7g" % float(a)
    if "." not in t and "e" not in t and "n" not in t:
        t += ".0"
    return t + "f"


def c_floats(v, per_line=5):
    out = []
    for i in range(0, len(v), per_line):
        out.append("\t" + " ".join(c_float(a) + "," for a in v[i:i + per_line]))
    return "\n".join(out)


def runs(v):
    """(first value, length) of each run of consecutive integers."""
    out = []
    for a in v:
        if out and a == out[-1][0] + out[-1][1]:
            out[-1][1] += 1
        else:
            out.append([a, 1])
    return out


def flag_runs(v):
    """(value, length) of each run of equal values."""
    out = []
    for a in v:
        if out and a == out[-1][0]:
            out[-1][1] += 1
        else:
            out.append([a, 1])
    return out


def c_ints(v, per_line=16):
    out = []
    for i in range(0, len(v), per_line):
        out.append("\t" + " ".join("%d," % int(a) for a in v[i:i + per_line]))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", nargs="?",
                    default=pathlib.Path(__file__).resolve().parent.parent / "src" / "pfb_ref.h")
    args = ap.parse_args()

    print("prototype: Kaiser beta %.4f (%.0f dB); response of the designed taps" %
          (kaiser_beta(STOP_DB), STOP_DB))
    print("   M   T  edge fs/(2M)  stop from fs/M  0.25 fs/M  0.4 fs/M")
    for t in (4, 8, 12, 16):
        for m in CHANNELS:
            h = design(m, t)
            edge = float(response_db(h, [0.5 / m])[0])
            stop = stop_band_db(h, m)
            pb = response_db(h, [0.25 / m, 0.4 / m])
            print("  %2d  %2d  %8.3f dB  %10.1f dB  %6.3f dB  %6.3f dB" %
                  (m, t, edge, stop, pb[0], pb[1]))
            if t == TAPS:
                assert abs(edge + 6.02) < 0.05, "edge %.3f dB" % edge
                assert stop <= -60.0, "M %d: stop band %.1f dB" % (m, stop)
    check_floor()
    print("the middle of the step takes the DC off the C6's floored items, shifts 0 to 2")

    items = stream()
    body = []
    tabs = []
    for m in CHANNELS:
        h, frames, pw, ws, sel = reference(m, items)
        m0 = frames[0][0]
        edge = float(response_db(h, [0.5 / m])[0])
        stop = stop_band_db(h, m)
        n = len(frames)
        print("M %2d: %d frames, %d complete, full frames %s" %
              (m, n, sum(f[1] & 1 for f in frames), sel))
        full = np.concatenate([np.column_stack([frames[i][2].real, frames[i][2].imag]).ravel()
                               for i in sel])
        dm = runs([f[0] - m0 for f in frames])
        fl = flag_runs([f[1] for f in frames])
        body.append("static const uint16_t ref%d_dm[] = {\n%s\n};" %
                    (m, c_ints([a for r in dm for a in r], 8)))
        body.append("static const uint16_t ref%d_flags[] = {\n%s\n};" %
                    (m, c_ints([a for r in fl for a in r], 8)))
        body.append("static const float ref%d_pw[%d] = {\n%s\n};" % (m, n, c_floats(pw)))
        body.append("static const float ref%d_ws[%d] = {\n%s\n};" % (m, n, c_floats(ws)))
        body.append("static const uint16_t ref%d_sel[%d] = {%s};" %
                    (m, len(sel), ", ".join(str(i) for i in sel)))
        body.append("static const float ref%d_full[%d] = {\n%s\n};" %
                    (m, len(full), c_floats(full)))
        tabs.append("\t{%d, %d, %dULL, %.6ff, %.6ff, %.10e,\n"
                    "\t %d, %d, %d, ref%d_dm, ref%d_flags, ref%d_pw, ref%d_ws, ref%d_sel,"
                    " ref%d_full}," %
                    (m, n, m0, edge, stop, float(np.sum(h * h)), len(sel), len(dm), len(fl),
                     m, m, m, m, m, m))

    segs = ",\n".join("\t{%d, %d, %d, %d, %d}" % s for s in SEGS)
    text = """/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Generated by tools/gen_ref.py: do not edit. The channelizer's definition evaluated directly in
 * numpy (double) over a gapped int8 stream, T = %d, %g dB design, DC gain 1. The items are an
 * LCG's top bytes (x = x * 1664525 + 1013904223 from x = %d), I then Q of the pair at offset o
 * from PFB_REF_I0 at 2 o; the segments take theirs by offset. A sample is (item << shift) +
 * (2^shift - 1) / 2 - dc with the segment's shift and DC: the middle of the item's step under the
 * C6's flooring shift, less the DC.
 */

#ifndef PFB_REF_H_
#define PFB_REF_H_

#include <stdint.h>

#define PFB_REF_I0    %dULL
#define PFB_REF_SEED  %dU
#define PFB_REF_TAPS  %dU
#define PFB_REF_ITEMS %dU

struct pfb_ref_seg {
	uint32_t off, pairs, shift;
	/* The segment's DC, raw units. */
	int32_t dc_i, dc_q;
};

static const struct pfb_ref_seg pfb_ref_segs[] = {
%s
};

struct pfb_ref {
	uint16_t m, frames;
	/* The first frame's index; the others' less it. */
	uint64_t m0;
	/* numpy's prototype: |H| at fs / (2 M), highest |H| from fs / M on (dB), sum h^2. */
	float edge_db, stop_db;
	double h2;
	uint16_t nsel, ndm, nflags;
	/* Runs of frames: (first index less m0, frames); of flags: (flags, frames). */
	const uint16_t *dm, *flags;
	/* Per frame: sum |y_k|^2; sum Re(y_k) cos(a_k) + Im(y_k) sin(a_k), a_k = tau frac(k g). */
	const float *pw, *ws;
	/* Frames given in full, M outputs (I then Q) each. */
	const uint16_t *sel;
	const float *full;
};

%s

static const struct pfb_ref pfb_refs[] = {
%s
};

#endif /* PFB_REF_H_ */
""" % (TAPS, STOP_DB, SEED, I0, SEED, TAPS, len(items), segs, "\n\n".join(body), "\n".join(tabs))
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print("wrote %s (%d bytes)" % (args.out, len(text)))


if __name__ == "__main__":
    main()
