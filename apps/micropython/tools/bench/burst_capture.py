# SPDX-License-Identifier: GPL-3.0-or-later
# Waits for a burst at FREQ (receiving RATE S/s, blocks of 1250 samples, two in a row at 10
# times the median power) and prints 12000 samples around it as hex lines with checksums for
# burst_analyse.py; a copy stays in /FLASH:/burst.bin. Long base64 dumps get truncated over
# mpremote, short checked lines do not.
import array
import binascii
import time

import esdr

FREQ = globals().get("FREQ", 2440e6)
RATE = globals().get("RATE", 250000)
GAIN = globals().get("GAIN", 32)
N = 50000
B = 1250

esdr.init()
esdr.tune(FREQ)
esdr.rx_gain(GAIN)
buf = bytearray(4 * N)
for t in range(30):
    esdr.rx_start(RATE, buf, N)
    esdr.rx_wait(3000)
    a = array.array("h", bytes(buf))
    pw = []
    for k in range(0, 2 * N, 2 * B):
        s = 0
        for j in range(k, k + 2 * B, 10):
            s += a[j] * a[j] + a[j + 1] * a[j + 1]
        pw.append(s)
    med = max(1, sorted(pw)[len(pw) // 2])
    bi = max(range(len(pw) - 1), key=lambda i: min(pw[i], pw[i + 1]))
    if min(pw[bi], pw[bi + 1]) > 10 * med and 6000 <= bi * B <= N - 8000:
        break
else:
    raise SystemExit("no burst")
start = bi * B - 4000
d = bytes(memoryview(buf)[4 * start:4 * (start + 12000)])
with open("/FLASH:/burst.bin", "wb") as f:
    f.write(d)
print("burst at try", t, "power", pw[bi] // med, "x median")
for k in range(0, len(d), 96):
    c = d[k:k + 96]
    print("H%06d %s %04x" % (k, binascii.hexlify(c).decode(), sum(c) & 0xffff))
    if k % 4800 == 0:
        time.sleep_ms(20)
print("END", len(d))
