# SPDX-License-Identifier: GPL-3.0-or-later
# Triggered snapshots (esdr.trig_start(): the WHY2025 badge's C6, the MCH2022 badge's ESP32)
# at FREQ for SECS seconds, each channelized into NFFT channels: the busy channels of some,
# then how many came, how many were broken and what channelize() cost. With F2, then the
# snapshots per half second for 4 s at FREQ, after retuning to F2 and back again (trigger mode
# stopped and started again, as the Bursts egg retunes): a trigger that settles slowly after a
# retune shows as a burst of snapshots at first.
import array
import time

import esdr

FREQ = globals().get("FREQ", 2440e6)
F2 = globals().get("F2", None)
SECS = globals().get("SECS", 10)
NFFT = globals().get("NFFT", 64)
RATIO = globals().get("RATIO", 8)
GAIN = globals().get("GAIN", 30)

esdr.init()
print("centre", esdr.tune(FREQ, offset=False))
esdr.rx_gain(GAIN)
snap = bytearray(2 * esdr.TRIG_SAMPLES)
mean = array.array("f", bytes(4 * NFFT))
peak = array.array("f", bytes(4 * NFFT))
esdr.trig_start(ratio=RATIO)
t0 = time.ticks_ms()
seq = n = cost = 0
while time.ticks_diff(time.ticks_ms(), t0) < SECS * 1000:
    r = esdr.trig_get(snap, seq)
    if r is None:
        time.sleep_ms(1)
        continue
    seq, trig = r
    t = time.ticks_us()
    esdr.channelize(snap, NFFT, mean, peak)
    cost += time.ticks_diff(time.ticks_us(), t)
    n += 1
    if n <= 3 or n % 100 == 0:
        busy = [c for c in range(NFFT) if peak[c] > max(mean) + 10]
        print("snap", seq, "trig at", trig, "busy channels", busy[:16],
              "peak %.1f dB" % max(peak))
esdr.trig_stop()
print("TRIG", esdr.trig_stats(), "taken", n, "channelize us", cost // max(n, 1))


def retune(freq):
    esdr.trig_stop()
    esdr.tune(freq, offset=False)
    esdr.trig_start(ratio=RATIO)
    seq = 0
    counts = []
    for _ in range(4 * 2):
        t0 = time.ticks_ms()
        n = 0
        while time.ticks_diff(time.ticks_ms(), t0) < 500:
            r = esdr.trig_get(snap, seq)
            if r is not None:
                n += r[0] - seq
                seq = r[0]
            time.sleep_ms(2)
        counts.append(n)
    print("RETUNE %.0f MHz: snapshots per 0.5 s" % (freq / 1e6), counts)


if F2 is not None:
    retune(FREQ)
    retune(F2)
    retune(FREQ)
    esdr.trig_stop()
