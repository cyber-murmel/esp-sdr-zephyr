# SPDX-License-Identifier: GPL-3.0-or-later
# The WHY2025 badge's triggered snapshots (esdr.trig_start()) at FREQ for SECS seconds, each
# channelized into NFFT channels: the busy channels of some, then how many came, how many
# were broken and what channelize() cost.
import array
import time

import esdr

FREQ = globals().get("FREQ", 2440e6)
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
