#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check an I/Q recording written by a GNU Radio file_sink (on the board or a host).

    check_capture.py rx.fc32 [--format fc32|sc16] [--rate HZ] [--center HZ]

Prints the length, level, DC share and the strongest spectral line (averaged 4096-point
Hann FFTs, DC bin left out). Exit status 1 for an empty file or non-finite values.
"""

import argparse
import sys

import numpy as np

NFFT = 4096


def load(path, fmt):
    if fmt == "fc32":
        return np.fromfile(path, dtype=np.complex64)
    raw = np.fromfile(path, dtype=np.int16)
    raw = raw[: raw.size // 2 * 2].astype(np.float32) / 16384.0
    return raw[0::2] + 1j * raw[1::2]


def spectrum(x):
    """Mean power per bin over Hann-windowed segments, DC centred."""
    n = min(NFFT, x.size)
    w = np.hanning(n)
    segs = x[: x.size // n * n].reshape(-1, n)
    p = np.mean(np.abs(np.fft.fft(segs * w, axis=1)) ** 2, axis=0)
    return np.fft.fftshift(p), np.fft.fftshift(np.fft.fftfreq(n))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path")
    ap.add_argument("--format", choices=("fc32", "sc16"), default="fc32")
    ap.add_argument("--rate", type=float, default=125e3, help="sample rate, Hz")
    ap.add_argument("--center", type=float, default=0.0, help="centre frequency, Hz")
    args = ap.parse_args()

    x = load(args.path, args.format)
    if x.size == 0:
        print("empty")
        return 1
    bad = int(np.count_nonzero(~np.isfinite(x)))
    print("samples        {}  ({:.3f} s)".format(x.size, x.size / args.rate))
    print("non-finite     {}".format(bad))
    if bad:
        return 1
    total = float(np.mean(np.abs(x) ** 2))
    mean = complex(np.mean(x))
    print("mean           {:.5f}{:+.5f}j".format(mean.real, mean.imag))
    print("rms            {:.5f}  ({:.1f} dBFS)".format(total ** 0.5, 10 * np.log10(total + 1e-30)))
    print("peak           {:.5f}".format(float(np.max(np.abs(x)))))
    print("dc share       {:.1f} dB".format(10 * np.log10(abs(mean) ** 2 / total + 1e-30)))
    p, f = spectrum(x)
    p[np.argmin(np.abs(f))] = 0.0
    k = int(np.argmax(p))
    print("strongest line {:.0f} Hz  ({:+.0f} Hz from centre, {:.1f} dB over the median bin)".format(
        args.center + f[k] * args.rate, f[k] * args.rate, 10 * np.log10(p[k] / np.median(p))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
