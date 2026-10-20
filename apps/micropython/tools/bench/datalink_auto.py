# SPDX-License-Identifier: GPL-3.0-or-later
# The Data link egg in auto mode for SECS seconds (run it on both boards at once), then its
# node's counters ("acked" against "msgs" is the measure), the link's and the receiver's.
import sys
import time

import launcher

SECS = globals().get("SECS", 25)
TXG = globals().get("TXG", 8)
RXG = globals().get("RXG", 32)

p = [a["path"] for a in launcher.apps() if a["path"].endswith("/datalink")][0]
sys.path[:0] = [p.rsplit("/", 2)[0], p]
import datalink as d
from gnuradio import _gui

tb = d.datalink()
tb.set_auto(True)
tb.set_tx_gain(TXG)
tb.set_rx_gain(RXG)
tb.start()
tb.show()
t0 = time.ticks_ms()
out = {}


def stop():
    if time.ticks_diff(time.ticks_ms(), t0) > SECS * 1000:
        import esdr

        out["link"] = dict(tb._link.stats) if tb._link else None
        out["rx"] = esdr.rx_stats()
        _gui.quit()


_gui.poll(stop)
_gui.run()
b = tb.epy_block_0
print("NODE %04x" % b.node, b.n, "snr", b.snr, "rssi", b.rssi, "off", b.offset, "rtt", b.rtt)
print("LINK", out.get("link"))
print("RX", out.get("rx"))
