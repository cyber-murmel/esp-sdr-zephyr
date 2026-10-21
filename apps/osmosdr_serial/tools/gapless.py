#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Analyse a file of gapless blocks (esdr_usb gapless -o FILE): the segments' axis, what was
kept, and with a carrier on air whether its phase holds across the gaps (which proves every
segment's index on the 80 MS/s axis).

  gapless.py FILE [--tone] [--min-pairs N] [--plot PNG]

--tone finds the strongest carrier in the long segments and fits its phase in each segment
against the absolute index: with right indices the phase only drifts slowly (the carrier's
frequency error), a wrong index shows as a jump of 2 pi f / fs per missing sample.
"""

import argparse
import struct
import sys

import numpy as np

from esdr_serial import BLK, BLOCK_BYTES, BLOCK_MAGIC  # block header: magic, flags, samples, index

SEG_HDR = struct.Struct("<IHBBhbbB3x")      # delta, pairs, shift, flags, power, dc_i, dc_q, gain
BLK_SEG = 0x0080
SEG_DROPPED, SEG_RESTART, SEG_LOUD = 0x01, 0x02, 0x04
FS = 80e6
TAU = 2 * np.pi
# Carrier amplitude, raw units, below which the phase check means nothing.
MIN_CARRIER = 8.0


def segments(path):
    """Yield (index, shift, flags, power_db, dc_i, dc_q, gain, iq complex64)."""
    data = np.fromfile(path, dtype=np.uint8)
    for off in range(0, len(data) - BLOCK_BYTES + 1, BLOCK_BYTES):
        b = data[off:off + BLOCK_BYTES].tobytes()
        magic, flags, samples, index = BLK.unpack_from(b, 0)
        if magic != BLOCK_MAGIC or not flags & BLK_SEG:
            raise ValueError(f"block at {off}: not a gapless block")
        pos = BLK.size
        while pos + SEG_HDR.size <= BLOCK_BYTES:
            delta, pairs, shift, sflags, power, dci, dcq, gain = SEG_HDR.unpack_from(b, pos)
            if pairs == 0:
                break
            pos += SEG_HDR.size
            items = np.frombuffer(b, dtype=np.int8, count=2 * pairs, offset=pos)
            pos += (2 * pairs + 3) & ~3
            v = items.astype(np.float32) * (1 << shift)
            iq = (v[0::2] - dci) + 1j * (v[1::2] - dcq)
            yield index + delta, shift, sflags, power / 10.0, dci, dcq, gain, iq.astype(np.complex64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--tone", action="store_true")
    ap.add_argument("--min-pairs", type=int, default=2048)
    ap.add_argument("--plot")
    a = ap.parse_args()

    segs = list(segments(a.file))
    if not segs:
        sys.exit("no segments")
    idx = np.array([s[0] for s in segs], dtype=np.int64)
    n = np.array([len(s[7]) for s in segs], dtype=np.int64)
    end = idx + n
    back = int(np.sum(idx[1:] < end[:-1]))
    joined = int(np.sum(idx[1:] == end[:-1]))
    span = (end[-1] - idx[0]) / FS
    flags = np.array([s[2] for s in segs])
    gains = np.array([s[6] for s in segs])
    shifts = np.array([s[1] for s in segs])
    print(f"{len(segs)} segments, {n.sum()} pairs over {span:.3f} s of axis "
          f"({100 * n.sum() / (span * FS):.2f} % kept), {joined} joined to the one before, "
          f"{back} overlapping or backwards")
    print(f"flags: {int(np.sum(flags & SEG_LOUD != 0))} loud, "
          f"{int(np.sum(flags & SEG_DROPPED != 0))} after drops, "
          f"{int(np.sum(flags & SEG_RESTART != 0))} restarts; shifts "
          f"{np.bincount(shifts, minlength=3)[:3].tolist()}; gains {sorted(set(gains.tolist()))}")
    pw = np.array([s[3] for s in segs])
    print(f"first-window power {pw.min():.1f} .. {pw.max():.1f} dBFS, median {np.median(pw):.1f}")
    if back:
        sys.exit("FAIL: segments overlap or go backwards")

    if a.tone:
        tone_check(segs, a)


def runs_of(segs):
    """Contiguous pieces joined into runs; a restart or a gain change starts a new one."""
    out = []
    for idx, shift, flags, pw, dci, dcq, gain, iq in segs:
        r = out[-1] if out else None
        if r is not None and idx == r["end"] and not flags & SEG_RESTART and gain == r["gain"]:
            r["iq"].append(iq)
            r["end"] += len(iq)
        else:
            out.append({"start": idx, "end": idx + len(iq), "iq": [iq], "gain": gain,
                        "restart": bool(flags & SEG_RESTART)})
    for r in out:
        r["iq"] = np.concatenate(r["iq"])
    return out


def refine_freq(x, t0):
    """The strongest carrier in x (taken at absolute indices t0..): FFT peak, then the slope of
    the sample phases about it."""
    n = len(x)
    nfft = 1 << (int(np.ceil(np.log2(n))) + 2)
    spec = np.abs(np.fft.fft(x * np.hanning(n), nfft))
    spec[0] = 0
    k = int(np.argmax(spec))
    a, b, c = spec[(k - 1) % nfft], spec[k], spec[(k + 1) % nfft]
    k = k + 0.5 * (a - c) / (a - 2 * b + c)
    f = (k if k < nfft / 2 else k - nfft) * FS / nfft
    t = t0 + np.arange(n)
    ph = np.unwrap(np.angle(x * np.exp(-1j * TAU * f * (t - t0) / FS)))
    slope = np.polyfit(np.arange(n), ph, 1)[0]
    return f + slope * FS / TAU


def tone_check(segs, a):
    runs = runs_of(segs)
    long_runs = [r for r in runs if len(r["iq"]) >= a.min_pairs]
    if not long_runs:
        sys.exit("no long runs for the carrier fit")
    ref = max(long_runs, key=lambda r: len(r["iq"]))
    f = refine_freq(ref["iq"], ref["start"])
    print(f"{len(runs)} runs ({len(long_runs)} of {a.min_pairs}+ pairs, the longest "
          f"{len(ref['iq'])}); carrier {f / 1e6:+.6f} MHz from the longest")
    # Runs between restarts share an exact axis; a restart's own offset is not known.
    groups, cur = [], []
    for r in long_runs:
        if r["restart"] and cur:
            groups.append(cur)
            cur = []
        cur.append(r)
    if cur:
        groups.append(cur)
    jump = TAU * abs(f) / FS
    worst, steps, amps = 0.0, 0, []
    for gi, g in enumerate(groups):
        if len(g) < 3:
            continue
        times = np.array([r["start"] / FS for r in g])
        ph, amp = [], []
        for r in g:
            t = r["start"] + np.arange(len(r["iq"]))
            c = np.sum(r["iq"] * np.exp(-1j * TAU * f * t / FS))
            ph.append(np.angle(c))
            amp.append(np.abs(c) / len(r["iq"]))
        ph = np.unwrap(np.array(ph))
        amp = np.array(amp)
        amps.append(float(np.median(amp)))
        # Runs another signal disturbed (their amplitude off the carrier's by over 2 dB, the
        # phase of the sum pulled away) are left out; so are lone runs whose phase leaves and
        # comes back (one disturbed run, not an index: a wrong index shifts all runs after it).
        ok = np.abs(20 * np.log10(np.maximum(amp, 1e-9) / np.median(amp))) < 2.0
        d = np.diff(ph)
        k = 25
        trend = np.array([np.median(d[max(0, i - k):i + k + 1]) for i in range(len(d))])
        r = d - trend
        lone = np.zeros(len(g), dtype=bool)
        lone[1:-1] = (np.abs(r[:-1]) > jump / 4) & (np.abs(r[:-1] + r[1:]) < jump / 4)
        keep = ok & ~lone
        times_k, ph_k = times[keep], ph[keep]
        lin = ph_k - np.polyval(np.polyfit(times_k, ph_k, 1), times_k)
        quad = ph_k - np.polyval(np.polyfit(times_k, ph_k, 2), times_k)
        # A wrong index jumps by 2 pi f / fs per sample between two runs; the oscillators'
        # wander moves the phase slowly. Judge the steps between neighbouring runs about the
        # local trend.
        dk = np.diff(ph_k)
        step = np.abs(dk - np.array([np.median(dk[max(0, i - k):i + k + 1]) for i in range(len(dk))]))
        worst = max(worst, float(np.max(step)))
        steps += len(step)
        print(f"group {gi}: {len(g)} runs over {times[-1] - times[0]:.3f} s ({int(np.sum(~ok))} "
              f"disturbed, {int(np.sum(lone & ok))} lone left out), amplitude {np.median(amp):.1f} raw; "
              f"phase about a line rms {np.std(lin):.3f} rad, about a quadratic rms "
              f"{np.std(quad):.3f} rad, largest step between runs {np.max(step):.3f} rad")
    # Without a carrier well over the noise the phases say nothing.
    carrier = bool(amps) and max(amps) >= MIN_CARRIER
    if not carrier:
        print(f"no carrier: the strongest line is {max(amps, default=0):.1f} raw, under {MIN_CARRIER}")
    ok = carrier and steps > 0 and worst < jump / 2
    print(f"{steps} steps between neighbouring runs, the largest {worst:.3f} rad; one sample off "
          f"would be {jump:.3f} rad: {'PASS' if ok else 'CHECK'}")
    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.plot([r["start"] / FS for r in long_runs], [len(r["iq"]) for r in long_runs], ".")
        plt.savefig(a.plot)
    return ok


if __name__ == "__main__":
    main()
