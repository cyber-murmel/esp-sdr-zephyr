# SPDX-License-Identifier: GPL-3.0-or-later
# The WHY2025 badge's triggered snapshots per half second for SECS seconds at F1, then again
# after retuning to F2 (as the channelizer egg retunes: trigger mode stopped and started
# again). A trigger that settles slowly after a retune shows as a burst of snapshots at first.
import time

import esdr

F1 = globals().get("F1", 2440e6)
F2 = globals().get("F2", 2460e6)
SECS = globals().get("SECS", 4)
RATIO = globals().get("RATIO", 8)

esdr.init()
esdr.rx_gain(30)
snap = bytearray(2 * esdr.TRIG_SAMPLES)


def run(freq):
    esdr.trig_stop()
    esdr.tune(freq, offset=False)
    esdr.trig_start(ratio=RATIO)
    seq = 0
    counts = []
    for _ in range(int(SECS * 2)):
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


run(F1)
run(F2)
run(F1)
esdr.trig_stop()
