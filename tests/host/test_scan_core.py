# SPDX-License-Identifier: GPL-3.0-or-later
"""The Scan app's measurement (apps/micropython/cmod/esdr_scan.c) against numpy: the spectrum of
a burst in fixed point (frames centred: on their means, the S3's on the middle of their range;
Blackman-Harris, mean and strongest frame per bin, dB of a full-scale tone) and the fold into the
panorama."""

import ctypes

import numpy as np
import pytest

N = 256
FS = 80e6
EMPTY = -200.0


class Scan:
    def __init__(self, lib):
        self.lib = lib
        # struct esdr_scan is some 6.5 KB; the C side lays it out.
        self.mem = ctypes.create_string_buffer(65536)
        lib.esdr_scan_init(self.mem)

    def spectrum(self, iq, frames_max=0, peak=True):
        x = np.ascontiguousarray(np.asarray(iq, dtype=np.int16).reshape(-1, 2))
        mean = np.zeros(N, np.float32)
        pk = np.zeros(N, np.float32)
        clipped = ctypes.c_uint32(0)
        frames = self.lib.esdr_scan_spectrum(
            self.mem, x.ctypes.data, len(x), frames_max, mean.ctypes.data,
            pk.ctypes.data if peak else None, ctypes.byref(clipped))
        return frames, mean, pk, clipped.value

    def fold(self, mean, peak, segs, recent, maxh, avg, floor, alpha, rise=1.0, corr=None):
        segs = np.ascontiguousarray(np.asarray(segs, dtype=np.uint16).reshape(-1))
        pmax, fmean = ctypes.c_float(0), ctypes.c_float(0)
        ret = self.lib.esdr_scan_fold(
            mean.ctypes.data, peak.ctypes.data if peak is not None else None,
            corr.ctypes.data if corr is not None else None, segs.ctypes.data, len(segs) // 3,
            recent.ctypes.data, maxh.ctypes.data, avg.ctypes.data, floor.ctypes.data,
            len(recent), ctypes.c_float(alpha), ctypes.c_float(rise), ctypes.byref(pmax),
            ctypes.byref(fmean))
        return ret, pmax.value, fmean.value

    def q8(self, db):
        db = np.ascontiguousarray(db, dtype=np.float32)
        out = np.zeros(len(db), np.uint8)
        self.lib.esdr_scan_q8(db.ctypes.data, out.ctypes.data, len(db))
        return out


@pytest.fixture
def scan(scan_clib):
    if scan_clib is None:
        pytest.skip("no C compiler")
    return Scan(scan_clib)


def bh(n=N):
    k = np.arange(n)
    x = 2 * np.pi * k / n
    return 0.35875 - 0.48829 * np.cos(x) + 0.14128 * np.cos(2 * x) - 0.01168 * np.cos(3 * x)


def reference(iq, frames_max=0, kind="int32"):
    """numpy's float spectrum of the same frames: dB of a full-scale tone, DC at N / 2. Each frame
    centred as the kind does: on its mean (int32), on the middle of its range in I and in Q
    (int16; it differs in the bins within 3 of DC only)."""
    x = np.asarray(iq, dtype=np.float64).reshape(-1, 2)
    z = x[:, 0] + 1j * x[:, 1]
    frames = len(z) // N
    if frames_max:
        frames = min(frames, frames_max)
    w = bh()
    acc = np.zeros(N)
    peak = np.zeros(N)
    for f in range(frames):
        s = z[f * N:(f + 1) * N]
        if kind == "int16":
            x16 = x[f * N:(f + 1) * N].astype(np.int64)
            c = (x16.max(axis=0) + x16.min(axis=0)) // 2
            s = s - (c[0] + 1j * c[1])
        else:
            s = s - s.mean()
        p = np.abs(np.fft.fft(s * w) / N) ** 2
        acc += p
        peak = np.maximum(peak, p)
    ref = (16384 * w.mean()) ** 2
    mean_db = np.fft.fftshift(10 * np.log10(acc / frames / ref + 1e-30))
    peak_db = np.fft.fftshift(10 * np.log10(peak / ref + 1e-30))
    return frames, mean_db, peak_db


def tone(n, hz, amp, phase=0.0, dc=(0, 0)):
    t = np.arange(n)
    z = amp * np.exp(1j * (2 * np.pi * hz / FS * t + phase))
    iq = np.stack([np.round(z.real + dc[0]), np.round(z.imag + dc[1])], axis=1)
    return np.clip(iq, -16384, 16383).astype(np.int16)


def noise(n, rms, seed=1):
    rng = np.random.default_rng(seed)
    return np.clip(np.round(rng.normal(0, rms, (n, 2))), -16384, 16383).astype(np.int16)


def test_a_tone_lands_in_its_bin_at_its_level(scan):
    # Bin 37 above DC (37 * 312.5 kHz), at -6 dBFS.
    frames, mean, peak, clipped = scan.spectrum(tone(16380, 37 * FS / N, 8192))
    assert frames == 63 and clipped == 0
    assert int(np.argmax(mean)) == N // 2 + 37
    assert mean[N // 2 + 37] == pytest.approx(20 * np.log10(0.5), abs=0.1)
    assert peak[N // 2 + 37] == pytest.approx(mean[N // 2 + 37], abs=0.1)
    # Blackman-Harris: everything 5 bins away and more over 70 dB down (the floor there is the
    # fixed point's rounding, some 80 dB under full scale a bin, and the tone's own int16 steps).
    # The S3's 16-bit FFT truncates: a clean tone this strong leaves spurs some 62 dB down, the
    # worst at DC and next to it, which the panorama takes from the next hop (the guard, 8 bins
    # each side on the S3); its noise-like rounding stays 74 dB under full scale.
    far = np.delete(mean, range(N // 2 + 37 - 4, N // 2 + 37 + 5))
    if scan.lib.kind == "int32":
        assert far.max() < mean.max() - 70
    else:
        guard = set(range(N // 2 - 8, N // 2 + 9)) | set(range(N // 2 + 37 - 4, N // 2 + 37 + 5))
        assert np.delete(mean, sorted(guard)).max() < mean.max() - 60
        assert np.median(far) < mean.max() - 70


def test_negative_frequencies_below_dc(scan):
    _, mean, _, _ = scan.spectrum(tone(4096, -100 * FS / N, 4096))
    assert int(np.argmax(mean)) == N // 2 - 100


def test_noise_matches_numpy(scan):
    iq = noise(16380, 400)
    frames, mean, peak, _ = scan.spectrum(iq)
    rf, rmean, rpeak = reference(iq, kind=scan.lib.kind)
    assert frames == rf == 63
    # Per bin within 0.25 dB (the fixed point and the Q15 window), on average far closer.
    assert np.abs(mean - rmean).max() < 0.25
    assert abs(np.mean(mean - rmean)) < 0.05
    assert np.abs(peak - rpeak).max() < 1.0
    assert abs(np.mean(peak - rpeak)) < 0.1


def test_a_quiet_floor_stays_above_the_rounding(scan):
    # The radios' floors at gain 48: 280 to 1200 as int16 (the XIAO 350), within 0.05 dB. At
    # 30 (about one raw LSB, the S3's floor at gain 0) the int32 FFT's rounding reads some 0.6 dB
    # high; the S3's 16-bit one scales each frame up first, within 0.05 dB down to 10.
    levels = ((300, 0.05), (100, 0.15), (30, 0.7))
    if scan.lib.kind == "int16":
        levels = ((300, 0.05), (100, 0.05), (30, 0.05), (10, 0.05))
    for rms, tol in levels:
        iq = noise(16380, rms, seed=3)
        _, mean, _, _ = scan.spectrum(iq)
        _, rmean, _ = reference(iq, kind=scan.lib.kind)
        assert abs(np.mean(mean - rmean)) < tol, rms


def test_the_16_bit_fft_takes_its_rounding_bias_off(scan):
    if scan.lib.kind != "int16":
        pytest.skip("the S3's 16-bit FFT only")
    # Its kernel truncates, which adds up to a fixed pattern (some 8 units at DC, 3 to 6 in the
    # 30 bins above it), taken off: with it, the bins 9 to 40 above DC average 74 dB under a
    # tone near full scale, and those next to DC stay 65 dB under an 8192 one; without, 69 and
    # 58.
    _, mean, _, _ = scan.spectrum(tone(16380, -100 * FS / N, 16000))
    assert mean[N // 2 - 100] - mean[N // 2 + 9:N // 2 + 41].mean() > 72
    _, mean, _, _ = scan.spectrum(tone(16380, 37 * FS / N, 8192))
    assert mean.max() - max(mean[N // 2 + 1], mean[N // 2 + 2]) > 62


def s3_tone(n, bins, amp, phase):
    """A tone as the S3's 10-bit samples times 32, hard clipped at amp past full scale."""
    t = np.arange(n)
    z = amp * np.exp(1j * (2 * np.pi * bins * t / N + phase))

    def q(v):
        return np.clip(np.floor(v / 32), -512, 511) * 32
    return np.stack([q(z.real), q(z.imag)], axis=1).astype(np.int16)


def test_a_clipped_tone_matches_numpy(scan):
    # The ADC's clipped square-ish wave, frames as wide as the samples go (the 16-bit FFT takes
    # them in at a gain of 1: no S3 sample lies further than 16352 from the middle of its frame's
    # range); every bin within 30 dB of the strongest within 0.15 dB of numpy (0.04 measured,
    # either FFT).
    iq = s3_tone(8 * N, 0.4, 24576, 4.0)
    _, mean, peak, clipped = scan.spectrum(iq)
    _, rmean, rpeak = reference(iq, kind=scan.lib.kind)
    near = rmean > rmean.max() - 30
    assert clipped == 8 * N
    assert np.abs(mean - rmean)[near].max() < 0.15
    assert np.abs(peak - rpeak)[near].max() < 0.15


def test_frames_wider_than_16_bits_are_taken_in_smaller(scan):
    if scan.lib.kind != "int16":
        pytest.skip("the S3's 16-bit FFT only (the int32 one clips |x - mean| at 16384)")
    # Wider than the S3's samples get: a tone of 24000 over noise (its frames more than 23000
    # from the middle of their range: a gain of 1/2), then a frame across all of int16 (1/2 as
    # well; with the middle of the range as the centre no frame needs less).
    rng = np.random.default_rng(2)
    t = np.arange(8 * N)
    z = rng.normal(0, 200, 8 * N) + 1j * rng.normal(0, 200, 8 * N)
    z += 24000 * np.exp(2j * np.pi * 20.3 * t / N)
    iq = np.clip(np.round(np.stack([z.real, z.imag], axis=1)), -32768, 32767).astype(np.int16)
    _, mean, peak, clipped = scan.spectrum(iq)
    _, rmean, rpeak = reference(iq, kind=scan.lib.kind)
    near = rmean > rmean.max() - 30
    assert mean[N // 2 + 20] == pytest.approx(rmean[N // 2 + 20], abs=0.1)
    assert np.abs(mean - rmean)[near].max() < 0.5
    assert np.abs(peak - rpeak)[near].max() < 0.5
    full = np.zeros((N, 2), np.int16)
    full[:, 0] = 32767
    full[::2, 0] = -32768
    full[:, 1] = -32768
    full[1::2, 1] = 32767
    _, mean, _, _ = scan.spectrum(full)
    _, rmean, _ = reference(full, kind=scan.lib.kind)
    near = rmean > rmean.max() - 30
    assert np.abs(mean - rmean)[near].max() < 0.05


def test_each_frame_loses_its_dc(scan):
    # A large offset that moves between frames (the LO's leak and its wander) stays out of all
    # but the bins next to DC.
    iq = noise(16380, 200, seed=5).astype(np.int32)
    for f in range(63):
        iq[f * N:(f + 1) * N] += (3000 + 40 * f, -2000 - 30 * f)
    iq = np.clip(iq, -16384, 16383).astype(np.int16)
    _, mean, _, _ = scan.spectrum(iq)
    _, quiet, _, _ = scan.spectrum(noise(16380, 200, seed=5))
    assert np.abs(mean - quiet).max() < 0.5


def test_clipped_samples_are_counted(scan):
    iq = tone(1024, 5e6, 2000)
    iq[10] = (16320, 0)
    iq[20] = (0, -16384)
    iq[30] = (16319, -16319)
    _, _, _, clipped = scan.spectrum(iq)
    assert clipped == 2


def test_frames_cap_and_short_input(scan):
    frames, _, _, _ = scan.spectrum(noise(16380, 100), frames_max=32)
    assert frames == 32
    frames, _, _, _ = scan.spectrum(noise(255, 100))
    assert frames == 0


def test_peak_holds_a_short_burst(scan):
    # A burst in one frame of 63: the mean shows it some 18 dB down, the peak at its level.
    iq = noise(16380, 100, seed=7).astype(np.int32)
    burst = tone(N, 20 * FS / N, 8192).astype(np.int32)
    iq[10 * N:11 * N] += burst
    _, mean, peak, _ = scan.spectrum(np.clip(iq, -16384, 16383))
    b = N // 2 + 20
    assert peak[b] == pytest.approx(20 * np.log10(0.5), abs=0.3)
    assert mean[b] == pytest.approx(peak[b] - 10 * np.log10(63), abs=0.5)


def empty(n):
    return [np.full(n, EMPTY, np.float32) for _ in range(4)]


def test_fold_places_segments_and_holds(scan):
    mean = np.arange(N, dtype=np.float32) - 150.0  # -150 .. 105
    peak = mean + 10.0
    recent, maxh, avg, floor = empty(1000)
    # Two segments of the hop: bins 100..109 to 500.., bins 150..154 to 600..
    ret, pmax, fmean = scan.fold(mean, peak, [100, 500, 10, 150, 600, 5], recent, maxh, avg,
                                 floor, 0.5)
    assert ret == 0
    assert list(recent[500:510]) == list(mean[100:110])
    assert list(recent[600:605]) == list(mean[150:155])
    assert list(maxh[600:605]) == list(peak[150:155])
    # Empty bins take the first value.
    assert list(avg[500:510]) == list(mean[100:110])
    assert list(floor[500:510]) == list(mean[100:110])
    assert pmax == pytest.approx(float(peak[154]))
    assert fmean == pytest.approx(float(np.concatenate([mean[100:110], mean[150:155]]).mean()))
    assert (recent[:500] == EMPTY).all() and (recent[605:] == EMPTY).all()
    # A second hop over the same bins, 20 dB down: the holds keep the larger, the average goes in
    # power, the floor follows down at once.
    mean2 = mean - 20.0
    scan.fold(mean2, mean2 + 3.0, [100, 500, 10], recent, maxh, avg, floor, 0.5)
    assert list(maxh[500:510]) == list(peak[100:110])
    assert list(recent[500:510]) == list(mean[100:110])
    expect = 10 * np.log10(0.5 * 10 ** (mean2[100:110] / 10) + 0.5 * 10 ** (mean[100:110] / 10))
    assert np.allclose(avg[500:510], expect, atol=1e-3)
    assert list(floor[500:510]) == list(mean2[100:110])
    # And up again: the floor rises by rise a fold at most.
    scan.fold(mean, peak, [100, 500, 10], recent, maxh, avg, floor, 0.5, rise=1.5)
    assert np.allclose(floor[500:510], mean2[100:110] + 1.5)


def test_fold_of_a_clipped_hop_fills_recent_only(scan):
    mean = np.full(N, -60.0, np.float32)
    recent, maxh, avg, floor = empty(10)
    scan.fold(mean, mean, [0, 0, 10], recent, maxh, avg, floor, 0.0)
    assert (recent == -60.0).all()
    assert (maxh == EMPTY).all() and (avg == EMPTY).all() and (floor == EMPTY).all()


def test_fold_adds_the_correction(scan):
    mean = np.full(N, -60.0, np.float32)
    corr = np.linspace(0, 3, N).astype(np.float32)
    recent, maxh, avg, floor = empty(N)
    scan.fold(mean, None, [0, 0, N], recent, maxh, avg, floor, 1.0, corr=corr)
    assert np.allclose(recent, -60.0 + corr) and np.allclose(maxh, -60.0 + corr)


def test_fold_refuses_a_segment_outside(scan):
    mean = np.zeros(N, np.float32)
    recent, maxh, avg, floor = empty(10)
    assert scan.fold(mean, mean, [250, 0, 10], recent, maxh, avg, floor, 0.5)[0] == -1
    assert scan.fold(mean, mean, [0, 5, 10], recent, maxh, avg, floor, 0.5)[0] == -1
    assert (recent == EMPTY).all()
    assert scan.fold(mean, mean, [], recent, maxh, avg, floor, 0.5)[0] == 0


def test_q8_steps_and_limits(scan):
    q = scan.q8([EMPTY, -150.0, -127.5, -127.25, -127.0, -60.0, -60.25, -0.5, 0.0, 3.0])
    assert list(q) == [0, 0, 0, 1, 1, 135, 135, 254, 255, 255]
