# SPDX-License-Identifier: GPL-3.0-or-later
"""Sample formats for the ESP32 -> FPGA link, judged on real bursts.

usage: python bfp_eval.py burst.npy ...   (complex64 arrays of 10-bit I/Q, as fpga dump gives)

Each format is encoded and decoded as specified, then compared with the lossless 10-bit
samples (IQ10P): Welch spectra (Blackman-Harris, N = 256 and 512) for the noise floor's shift
and the worst error in bins within 40 dB of the peak, time-domain SQNR, and a weak tone 60 dB
(0.3 times the burst's RMS, some 13 dB over its floor at N = 512) next to one at -3 dBFS, both
added to the burst, for how much of its margin over the floor each format keeps.
"""

import math
import sys

import numpy as np

TAU = 2 * math.pi
BLOCK = 16          # I/Q pairs per shared exponent (BFP)


def bfp(x, m):
    """Block floating point: per 16 pairs one exponent byte, each component an m-bit signed
    mantissa; value = mantissa << exponent. Returns (decoded, bits per pair)."""
    comp = np.stack([x.real, x.imag], axis=-1).astype(np.int64)
    n = len(comp) // BLOCK * BLOCK
    blocks = comp[:n].reshape(-1, BLOCK * 2)
    lo, hi = -(1 << (m - 1)), (1 << (m - 1)) - 1
    out = np.empty_like(blocks)
    for b, v in enumerate(blocks):
        e = 0
        while True:
            q = (v + ((1 << e) >> 1)) >> e
            if q.min() >= lo and q.max() <= hi:
                break
            e += 1
        out[b] = q << e
    out = out.reshape(-1, 2)
    return out[:, 0] + 1j * out[:, 1], 2 * m + 8 / BLOCK


E2M1 = np.array([0, 0.5, 1, 1.5, 2, 3, 4, 6])


def fp8_e4m3(v):
    """Round positive values to FP8 E4M3 (bias 7, max 448)."""
    v = np.clip(v, 2 ** -9, 448)
    e = np.floor(np.log2(v))
    e = np.maximum(e, -6)
    step = 2.0 ** (e - 3)
    return np.round(v / step) * step


def nvfp4(x):
    """NVFP4: blocks of 16 scalars share an E4M3 scale (max / 6), each an E2M1 value."""
    comp = np.stack([x.real, x.imag], axis=-1).reshape(-1).astype(np.float64)
    n = len(comp) // 16 * 16
    blocks = comp[:n].reshape(-1, 16)
    out = np.empty_like(blocks)
    for b, v in enumerate(blocks):
        amax = np.abs(v).max()
        s = fp8_e4m3(np.array([amax / 6]))[0] if amax else 1.0
        r = np.abs(v) / s
        idx = np.abs(r[:, None] - E2M1[None, :]).argmin(axis=1)
        out[b] = np.sign(v) * E2M1[idx] * s
    out = out.reshape(-1, 2)
    return out[:, 0] + 1j * out[:, 1], 2 * 4 + 2 * 8 / 16


def iq8(x):
    return (np.floor(x.real / 4) * 4 + 1j * np.floor(x.imag / 4) * 4), 16


def iq4(x):
    """The S3 osmosdr 4-bit items: the top four of ten bits."""
    return (np.floor(x.real / 64) * 64 + 1j * np.floor(x.imag / 64) * 64), 8


FORMATS = {
    "IQ10P": lambda x: (x, 20),
    "BFP8": lambda x: bfp(x, 8),
    "BFP6": lambda x: bfp(x, 6),
    "BFP4": lambda x: bfp(x, 4),
    "NVFP4": nvfp4,
    "IQ8": iq8,
    "4-bit": iq4,
}


def welch(x, n):
    k = np.arange(n)
    w = (0.35875 - 0.48829 * np.cos(TAU * k / n) + 0.14128 * np.cos(2 * TAU * k / n)
         - 0.01168 * np.cos(3 * TAU * k / n))
    frames = len(x) // n
    p = np.zeros(n)
    for f in range(frames):
        p += np.abs(np.fft.fft((x[f * n:(f + 1) * n] - x.mean()) * w)) ** 2
    return 10 * np.log10(np.fft.fftshift(p / frames) / (512 * w.sum()) ** 2 + 1e-30)


def compare(ref, x, n):
    a, b = welch(ref, n), welch(x, n)
    floor = np.median(b) - np.median(a)
    strong = a > a.max() - 40
    worst = np.abs(b - a)[strong].max()
    return floor, worst


def sqnr(ref, x):
    n = min(len(ref), len(x))
    err = x[:n] - ref[:n]
    return 10 * math.log10(np.mean(np.abs(ref[:n]) ** 2) / max(np.mean(np.abs(err) ** 2), 1e-30))


def tones(noise, n=512):
    """A tone at -3 dBFS and a weak one at 0.3 times the burst's RMS, on the burst's noise."""
    k = np.arange(len(noise))
    rms = np.sqrt(np.mean(np.abs(noise - noise.mean()) ** 2))
    strong = 362 * np.exp(1j * TAU * (n / 8 + 0.3) * k / n)
    weak = 0.3 * rms * np.exp(1j * TAU * (-n / 5) * k / n)
    x = noise + strong + weak
    x = np.clip(np.round(x.real), -512, 511) + 1j * np.clip(np.round(x.imag), -512, 511)
    return x, (-n // 5) % n


def weak_margin(x, n, kbin):
    p = welch(x, n)
    floor = np.median(p)
    k = (kbin + n // 2) % n
    return p[k - 1:k + 2].max() - floor


def main(paths):
    print(f"{'burst':16} {'format':6} {'bits':>5} {'SQNR':>6} "
          f"{'floor256':>8} {'worst256':>8} {'floor512':>8} {'worst512':>8} {'weak':>6}")
    print("(dB; floor: median spectrum shift, worst: largest error within 40 dB of the peak, "
          "weak: the weak tone's margin over the floor against IQ10P's)")
    summary = {name: [] for name in FORMATS}
    for path in paths:
        ref = np.load(path).astype(np.complex128)
        tone_ref, kbin = tones(ref)
        weak_ref = weak_margin(tone_ref, 512, kbin)
        for name, enc in FORMATS.items():
            x, bits = enc(ref)
            f256, w256 = compare(ref, x, 256)
            f512, w512 = compare(ref, x, 512)
            t, _ = enc(tone_ref)
            weak = weak_margin(t, 512, kbin) - weak_ref
            s = sqnr(ref, x)
            summary[name].append((bits, s, f512, w512, weak))
            print(f"{path.split('/')[-1][:16]:16} {name:6} {bits:5.2f} {s:6.1f} "
                  f"{f256:8.2f} {w256:8.2f} {f512:8.2f} {w512:8.2f} {weak:6.1f}")
    print()
    print(f"{'format':6} {'bits':>5} {'SQNR min':>8} {'floor max':>9} {'worst max':>9} "
          f"{'weak min':>8}   (over all bursts; N = 512)")
    for name, rows in summary.items():
        a = np.array(rows)
        print(f"{name:6} {a[0, 0]:5.2f} {a[:, 1].min():8.1f} {np.abs(a[:, 2]).max():9.2f} "
              f"{a[:, 3].max():9.2f} {a[:, 4].min():8.1f}")


if __name__ == "__main__":
    main(sys.argv[1:])
