# SPDX-License-Identifier: GPL-3.0-or-later
"""soapy.source for the board's own radio, set up as the host plugin "driver=esdr" is
(apps/osmosdr/soapy): one RF gain index (any gain name sets it, clamped), the rates
16 MS/s / 64 .. 1024, offset tuning. Any driver string is accepted: the board has one
radio. Settings change between captures; while only displays take the stream (qtgui sinks),
a change restarts it at once."""

import esdr
from gnuradio import gr

_TYPES = {"fc32": gr.sizeof_gr_complex, "cs16": 2 * gr.sizeof_short}


def _driver(device):
    args = dict(kv.split("=", 1) for kv in device.replace(" ", "").split(",") if "=" in kv)
    drv = args.get("driver", "")
    if drv != "esdr":
        print("soapy: driver={} runs on the board's radio".format(drv or "?"))


def _snap_rate(rate):
    best = min(esdr.RATES, key=lambda r: abs(r - rate))
    if abs(best - rate) > 0.5:
        print("soapy: sample rate {} -> {} (rates: {})".format(rate, best, esdr.RATES))
    return best


class source(gr.basic_block):
    _kind = "source"

    def __init__(self, device, type="fc32", nchan=1, dev_args="", stream_args="",
                 tune_args=None, other_settings=None):
        super().__init__("soapy_source")
        _driver(device)
        if type not in _TYPES:
            raise ValueError("stream type {} (fc32 or cs16)".format(type))
        if nchan != 1:
            raise ValueError("one channel")
        self.in_itemsize = 0
        self.out_itemsize = _TYPES[type]
        self._rate = esdr.RATES[1]
        self._freq = 2.45e9
        self._gain = 40
        self._auto = False
        self._bw = 0
        self._buf = None
        self._n = 0
        self._live = False
        self._monitor = False
        esdr.init()

    def _changed(self):
        # Only a display-only stream restarts at once; a capture keeps its settings.
        if self._live:
            esdr.rx_stop()
            self._run(0)

    # -- settings --
    def set_sample_rate(self, channel, rate):
        self._rate = _snap_rate(rate)
        self._changed()

    def get_sample_rate(self, channel=0):
        return self._rate

    def set_frequency(self, channel, name_or_freq, freq=None):
        self._freq = float(name_or_freq if freq is None else freq)
        self._changed()

    def get_frequency(self, channel=0, name=None):
        return self._freq

    def set_gain_mode(self, channel, automatic):
        self._auto = bool(automatic)
        self._changed()

    def set_gain(self, channel, name_or_gain, gain=None):
        value = name_or_gain if gain is None else gain
        self._auto = False
        self._gain = int(max(0, min(esdr.rx_gain_max(), float(value))))
        self._changed()

    def get_gain(self, channel=0, name=None):
        return self._gain

    def set_bandwidth(self, channel, bandwidth):
        self._bw = bandwidth
        self._changed()

    def set_antenna(self, channel, name):
        pass

    def set_dc_offset_mode(self, channel, automatic):
        pass

    def set_frequency_correction(self, channel, ppm):
        if ppm:
            print("soapy: frequency correction not supported on the board")

    # -- runtime, called by gr.top_block --
    def _run(self, count):
        self._centre = esdr.tune(self._freq)
        esdr.rx_gain(-1 if self._auto else self._gain)
        esdr.rx_bandwidth(self._bw)
        esdr.rx_start(self._rate, self._buf, count, monitor=self._monitor)

    def _start(self, tb):
        count = tb._items
        self._buf = None
        if any(s._want_buffer() for s in tb._sinks):
            self._buf = _alloc(count)
        self._monitor = any(getattr(s, "_wants_monitor", False) for s in tb._sinks)
        self._live = self._monitor and self._buf is None and not count
        self._run(count)

    def _stop(self, tb):
        self._live = False
        esdr.rx_stop()

    def _wait(self, tb):
        esdr.rx_wait()
        self._live = False

    def _captured(self):
        self._n = esdr.rx_stats()["samples"]
        buf, self._buf = self._buf, None
        return buf, self._n

    def stats(self):
        return esdr.rx_stats()


def _alloc(count):
    """The capture buffer: count samples, or (count 0) as many as the heap holds."""
    import gc

    gc.collect()
    if count:
        return bytearray(4 * count)
    size = (gc.mem_free() * 7 // 8) & ~3
    while size >= 4096:
        try:
            buf = bytearray(size)
            print("soapy: capture buffer {} samples".format(size // 4))
            return buf
        except MemoryError:
            size = (size * 3 // 4) & ~3
    raise MemoryError("no room for a capture buffer")


# ---- transmit ----

_DAC_HZ = 40000000
# Measured gapless (esdr.tx_stats() restarts 0) up to this input rate: above it the DAC backend's
# block fill takes longer than a block plays.
_TX_RATE_MAX_OK = 500000


def _snap_tx_rate(rate):
    """The input rate nearest to rate that divides the 40 MS/s DAC rate by 2 or more."""
    n0 = max(2, int(round(_DAC_HZ / float(rate))))
    best = None
    for d in range(0, n0):
        for n in (n0 - d, n0 + d):
            if n >= 2 and _DAC_HZ % n == 0:
                r = _DAC_HZ // n
                if best is None or abs(r - rate) < abs(best - rate):
                    best = r
        if best is not None and d > abs(best - rate) * n0 / max(1.0, float(rate)):
            break
    if abs(best - rate) > 0.5:
        print("soapy: transmit sample rate {} -> {} (40 MS/s over a whole number)".format(rate, best))
    if best > _TX_RATE_MAX_OK:
        print("soapy: transmit above {} S/s is not gapless on the board".format(_TX_RATE_MAX_OK))
    return best


class sink(gr.basic_block):
    """soapy.sink for the board's own radio, as the host plugin "driver=esdr": one transmit
    gain step 0 (weakest, the default) to esdr.tx_gain_max() that any gain name sets (clamped),
    a centre within 500 Hz of the frequency asked for. Transmitting holds the radio: no receive
    flowgraph meanwhile. Settings changed while transmitting restart the transmission."""

    _kind = "txsink"

    def __init__(self, device, type="fc32", nchan=1, dev_args="", stream_args="",
                 tune_args=None, other_settings=None):
        super().__init__("soapy_sink")
        _driver(device)
        if type not in _TYPES:
            raise ValueError("stream type {} (fc32 or cs16)".format(type))
        if nchan != 1:
            raise ValueError("one channel")
        self.in_itemsize = _TYPES[type]
        self.out_itemsize = 0
        self._rate = 250000
        self._freq = 2.45e9
        self._gain = 0
        self._tb = None
        self.centre = None
        esdr.init()

    # -- settings --
    def set_sample_rate(self, channel, rate):
        self._rate = _snap_tx_rate(rate)
        self._restart()

    def get_sample_rate(self, channel=0):
        return self._rate

    def set_frequency(self, channel, name_or_freq, freq=None):
        self._freq = float(name_or_freq if freq is None else freq)
        self._restart()

    def get_frequency(self, channel=0, name=None):
        return self._freq

    def set_gain(self, channel, name_or_gain, gain=None):
        value = float(name_or_gain if gain is None else gain)
        step = int(max(0, min(esdr.tx_gain_max(), value)))
        if step != value:
            print("soapy: transmit gain {} -> step {} (0 .. {})".format(value, step, esdr.tx_gain_max()))
        self._gain = step
        self._restart()

    def get_gain(self, channel=0, name=None):
        return self._gain

    def set_gain_mode(self, channel, automatic):
        pass

    def set_bandwidth(self, channel, bandwidth):
        pass

    def set_antenna(self, channel, name):
        pass

    def set_dc_offset(self, channel, offset):
        pass

    def set_frequency_correction(self, channel, ppm):
        if ppm:
            print("soapy: frequency correction not supported on the board")

    # -- runtime, called by gr.top_block --
    def _run(self):
        tb = self._tb
        args = tb._source._tx_args()
        scale = tb._tx_scale()
        if "gen" in args:
            kw = {"gen": args["gen"](scale)}
        else:
            kw = {"buf": args["buf"], "repeat": args["repeat"], "scale": scale}
        self.centre = esdr.tx_start(int(self._rate), self._freq, self._gain, count=tb._items, **kw)

    def _start(self, tb):
        self._tb = tb
        for b in tb._chain:
            b._tb = tb
        self._run()

    def _restart(self):
        if self._tb is not None and self._tb._running and esdr.tx_busy():
            esdr.tx_stop()
            self._run()

    def _live(self):
        args = self._tb._source._tx_args()
        scale = self._tb._tx_scale()
        if "gen" in args:
            esdr.tx_gen(*args["gen"](scale))
        else:
            esdr.tx_scale(scale)

    def _stop(self, tb):
        pass

    def stats(self):
        return esdr.tx_stats()
