#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find gaps in a received single-tone I/Q recording (esdr_serial.py rx --out FILE.npy).

    gapcheck.py FILE.npy --rate HZ [--block 64] [--step-deg 30] [--dip 0.3] [--json]
    gapcheck.py --selftest

A continuous tone advances its phase by the same amount every sample. The tone is tracked
segment by segment (two boards' crystals put it off the nominal offset, and a transmitter that
has just started drifts by a few ppm while it warms up), demodulated, and averaged over blocks of
--block samples, which works well below 0 dB per-sample SNR. Then:

- k lost samples are a spike of k * 360 * f / fs degrees in the first difference of the block
  phase (over two blocks when the gap falls inside one);
- missing or zero-filled samples are a dip of the block amplitude.

Slow residue of the tracking and level changes are taken off with running medians. Exit status
1 when a gap is found.

Blind spot: k lost samples are invisible when k * f / fs is nearly a whole number. The report
lists the first such k for the tone it found; pick a tone that keeps them large.
"""

import argparse
import json
import math
import sys

import numpy as np


def track_freq(x, fs, seg=2048):
    """The tone's frequency per segment of @p seg samples (FFT peak, parabolic interpolation,
    each search within 3 kHz of the previous), median-smoothed over about 50 ms. Returns the
    segment centres, the frequencies and the median peak over the median bin in dB."""
    n = len(x) // seg
    nfft = seg * 8
    win = np.hanning(seg)
    freqs = np.fft.fftfreq(nfft, 1 / fs)
    fk = np.empty(n)
    pk = np.empty(n)
    prev = None
    for k in range(n):
        sp = np.abs(np.fft.fft(x[k * seg:(k + 1) * seg] * win, nfft)) ** 2
        gated = sp if prev is None else np.where(np.abs(freqs - prev) < 3000.0, sp, 0.0)
        i = int(np.argmax(gated))
        a, b, c = (np.log(sp[(i + d) % nfft] + 1e-30) for d in (-1, 0, 1))
        den = a - 2 * b + c
        fk[k] = freqs[i] + (0.5 * (a - c) / den if den != 0 else 0.0) * fs / nfft
        pk[k] = 10 * np.log10(sp[i] / np.median(sp))
        prev = fk[k]
    w = max(3, int(0.05 * fs / seg) | 1)
    med = np.array([np.median(fk[max(0, k - w // 2):k + w // 2 + 1]) for k in range(n)])
    return (np.arange(n) + 0.5) * seg, med, float(np.median(pk))


def running_median(v, w):
    pad = np.pad(v, (w // 2, w // 2), mode="edge")
    return np.median(np.lib.stride_tricks.sliding_window_view(pad, w), axis=1)


def hidden_gaps(f, fs, tol=0.02, kmax=2000):
    """Lost-sample counts that move the phase by almost a whole number of cycles."""
    k = np.arange(1, kmax)
    frac = np.abs(k * f / fs - np.round(k * f / fs))
    return [int(v) for v in k[frac < tol][:5]]


def groups(idx, gap=2):
    out = []
    for i in idx:
        if out and i - out[-1][1] <= gap:
            out[-1][1] = int(i)
        else:
            out.append([int(i), int(i)])
    return out


def analyse(x, fs, block=64, step_deg=30.0, dip=0.3):
    """Gaps in the complex recording @p x at @p fs: a dict with the tone, the noise of the block
    phase, the phase steps and the amplitude dips (sample positions)."""
    x = np.asarray(x, np.complex128)
    x = x - x.mean()
    tc, fk, peak = track_freq(x, fs)
    n = len(x) // block * block
    phi = math.tau * np.cumsum(np.interp(np.arange(n), tc, fk)) / fs
    m = (x[:n] * np.exp(-1j * phi)).reshape(-1, block).mean(axis=1)
    amp = np.abs(m)
    nb = len(m)
    wm = min(nb // 2 * 2 - 1, 257)
    d = np.diff(np.degrees(np.unwrap(np.angle(m))))
    e = d - running_median(d, wm)
    e2 = e[:-1] + e[1:]
    sigma = 1.4826 * float(np.median(np.abs(e2 - np.median(e2))))
    thr = max(step_deg, 8 * sigma)
    steps = []
    for lo, hi in groups(np.flatnonzero(np.abs(e2) > thr)):
        j = lo + int(np.argmax(np.abs(e2[lo:hi + 1])))
        v = float(e2[lo:hi + 1].sum()) if hi > lo else float(e2[j])
        steps.append((j + 1, (v + 180.0) % 360.0 - 180.0))
    rel = amp / np.maximum(running_median(amp, wm), 1e-9)
    sig_a = 1.4826 * float(np.median(np.abs(rel - 1.0)))
    dips = np.flatnonzero(rel < 1.0 - max(5 * sig_a, dip))
    f = float(np.median(fk))
    per = 360.0 * f / fs
    return dict(
        samples=len(x), rate_hz=float(fs), tone_hz=f, tone_hz_first=float(fk[0]),
        tone_hz_last=float(fk[-1]), peak_db=float(peak), block=block,
        block_phase_sigma_deg=sigma, step_threshold_deg=thr, deg_per_sample=per,
        phase_steps=[dict(at=int(s * block), size_deg=v, lost_samples_est=v / per if per else None)
                     for s, v in steps],
        amplitude_dips=[dict(at=int(a * block), blocks=int(b - a + 1)) for a, b in groups(dips)],
        hidden_gap_samples=hidden_gaps(f, fs),
    )


def is_clean(r):
    return not r["phase_steps"] and not r["amplitude_dips"]


def report(r):
    print(f"{r['samples']} samples at {r['rate_hz'] / 1e3:.3f} kS/s, tone {r['tone_hz_first'] / 1e3:+.3f} -> "
          f"{r['tone_hz_last'] / 1e3:+.3f} kHz (peak {r['peak_db']:.1f} dB over median), blocks of {r['block']}")
    print(f"block phase noise {r['block_phase_sigma_deg']:.1f} deg, step threshold "
          f"{r['step_threshold_deg']:.0f} deg; blind to gaps of {r['hidden_gap_samples']} samples")
    for s in r["phase_steps"]:
        print(f"  phase step near sample {s['at']}: {s['size_deg']:+.0f} deg (~{s['lost_samples_est']:+.1f} samples)")
    for s in r["amplitude_dips"]:
        print(f"  amplitude dip near sample {s['at']} ({s['blocks']} blocks)")
    ok = is_clean(r)
    print("CLEAN" if ok else f"GAPS: {len(r['phase_steps'])} phase steps, {len(r['amplitude_dips'])} dips")
    return ok


def selftest():
    """A noisy drifting tone (per-sample SNR about -6 dB): clean; 3 samples lost; 400 missing."""
    rng = np.random.default_rng(1)
    fs, n, a = 250e3, 1 << 20, 10.0
    t = np.arange(n)
    f = 53e3 + 2e3 * t / n  # warm-up drift
    x = a * np.exp(1j * math.tau * np.cumsum(f) / fs) + (5 + 3j)
    x = x + rng.normal(0, a * 1.4, n) + 1j * rng.normal(0, a * 1.4, n)
    r = analyse(x, fs)
    assert is_clean(r), r
    r = analyse(np.concatenate([x[:500000], x[500003:]]), fs)
    assert len(r["phase_steps"]) == 1 and abs(r["phase_steps"][0]["at"] - 500000) < 200, r
    want = 3 * 360.0 * f[500000] / fs
    assert abs((r["phase_steps"][0]["size_deg"] - want + 180) % 360 - 180) < 20, r
    z = x.copy()
    z[900000:900400] = rng.normal(0, a * 1.4, 400) + 1j * rng.normal(0, a * 1.4, 400)
    r = analyse(z, fs)
    assert r["amplitude_dips"] and abs(r["amplitude_dips"][0]["at"] - 900200) < 400, r
    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file", nargs="?")
    ap.add_argument("--rate", type=float, default=250000)
    ap.add_argument("--block", type=int, default=64)
    ap.add_argument("--step-deg", type=float, default=30.0)
    ap.add_argument("--dip", type=float, default=0.3, help="relative depth of an amplitude dip")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.file:
        ap.error("file required")
    r = analyse(np.load(a.file), a.rate, a.block, a.step_deg, a.dip)
    if a.json:
        print(json.dumps(r))
        return 0 if is_clean(r) else 1
    return 0 if report(r) else 1


if __name__ == "__main__":
    sys.exit(main())
