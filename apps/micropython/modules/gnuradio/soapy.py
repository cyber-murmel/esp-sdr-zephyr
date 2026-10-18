# SPDX-License-Identifier: GPL-3.0-or-later
"""soapy.source for the board's own radio, set up as the host plugin "driver=esdr" is
(apps/osmosdr/soapy): one RF gain index (any gain name sets it, clamped), the continuous rates
16 MS/s / 64 .. 1024 with offset tuning, and the native rates 80, 40 and 16 MS/s in bursts
(esdr.BURST_RATES: contiguous within 16380 samples, enough for displays and short captures).
Any driver string is accepted: the board has one radio. Settings change between captures;
while only displays take the stream (qtgui sinks), a change restarts it at once."""

import esdr
from gnuradio import gr

_TYPES = {"fc32": gr.sizeof_gr_complex, "cs16": 2 * gr.sizeof_short}


def _driver(device):
    args = dict(kv.split("=", 1) for kv in device.replace(" ", "").split(",") if "=" in kv)
    drv = args.get("driver", "")
    if drv != "esdr":
        print("soapy: driver={} runs on the board's radio".format(drv or "?"))


def _snap_rate(rate):
    rates = esdr.RATES + esdr.BURST_RATES
    best = min(rates, key=lambda r: abs(r - rate))
    if abs(best - rate) > 0.5:
        print("soapy: sample rate {} -> {} (rates: {})".format(rate, best, rates))
    if best in esdr.BURST_RATES:
        print("soapy: {} S/s in bursts of {} samples".format(best, _BURST))
    return best


_BURST = 16380


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
        self._link = None
        # A packet flowgraph's receive tuning correction (Hz, gnuradio/_link.py's AFC).
        self._afc = 0.0
        esdr.init()

    def _changed(self):
        # Only a display-only stream restarts at once; a capture keeps its settings; a packet
        # receiver starts again with them.
        if self._link is not None:
            self._link.retune()
        elif self._live:
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
        self._afc = 0.0
        if self._link is not None:
            self._link.afc_want = None
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

    def set_dc_offset(self, channel, offset):
        pass

    def set_iq_balance(self, channel, balance):
        pass

    def set_frequency_correction(self, channel, ppm):
        if ppm:
            print("soapy: frequency correction not supported on the board")

    # -- runtime, called by gr.top_block --
    def _run(self, count):
        burst = self._rate in esdr.BURST_RATES
        # 40 MHz mode: the analog filters pass +-18 MHz, for wide bursts or a wide bandwidth.
        esdr.channel_bw(2 if (burst and self._rate >= 40e6) or self._bw > 20e6 else 0)
        # Bursts are not mixed down: the LO sits at the centre.
        self._centre = esdr.tune(self._freq, offset=not burst)
        esdr.rx_gain(-1 if self._auto else self._gain)
        esdr.rx_bandwidth(self._bw)
        esdr.rx_start(self._rate, self._buf, count, monitor=self._monitor)

    def _rx_ring(self, ring):
        """A packet flowgraph's receiver (gnuradio/_link.py): into the ring, endlessly."""
        esdr.channel_bw(2 if self._bw > 20e6 else 0)
        self._centre = esdr.tune(self._freq + self._afc, offset=True)
        esdr.rx_gain(-1 if self._auto else self._gain)
        esdr.rx_bandwidth(self._bw)
        esdr.rx_start(self._rate, ring, 0, wrap=True)

    def _rx_fsk(self, state, slow=1):
        """As _rx_ring(), with the radio engine running the FSK receiver state on the samples
        (esdr.fsk_frames() collects its frames), at 1/slow of the rate."""
        esdr.channel_bw(2 if self._bw > 20e6 else 0)
        self._centre = esdr.tune(self._freq + self._afc, offset=True)
        esdr.rx_gain(-1 if self._auto else self._gain)
        esdr.rx_bandwidth(self._bw)
        esdr.rx_start(self._rate, None, 0, fsk=state, slow=slow)

    def _start(self, tb):
        count = tb._items
        if self._rate in esdr.BURST_RATES and count > _BURST:
            print("soapy: {} samples at {} S/s: {} bursts, not contiguous".format(
                count, self._rate, (count + _BURST - 1) // _BURST))
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
# Streaming is gapless (esdr.tx_stats() restarts 0) up to this input rate: above it the DAC
# backend's block fill takes longer than a block plays. Periodic signals loop at the DAC rate.
_TX_RATE_MAX_OK = 500000
# A loop is at most this many DAC samples (one bank).
_LOOP_MAX = 16380
_LOOP_MIN = 256


def _snap_tx_rate(rate):
    """The input rate nearest to rate that divides 40 MS/s, or 80 MS/s (the DAC's two rates)."""
    rate = float(rate)
    if rate >= 60e6:
        best = 80000000
    else:
        n0 = max(1, int(round(_DAC_HZ / rate)))
        best = None
        for d in range(0, n0 + 1):
            for n in (n0 - d, n0 + d):
                if n >= 1 and _DAC_HZ % n == 0:
                    r = _DAC_HZ // n
                    if best is None or abs(r - rate) < abs(best - rate):
                        best = r
            if best is not None and (n0 - d) * (n0 + d) and d > abs(best - rate) * n0 / rate:
                break
    if abs(best - rate) > 0.5:
        print("soapy: transmit sample rate {} -> {} (40 MS/s over a whole number, or 80 MS/s)".format(rate, best))
    return best


class sink(gr.basic_block):
    """soapy.sink for the board's own radio, as the host plugin "driver=esdr": one transmit
    gain step 0 (weakest, the default) to esdr.tx_gain_max() that any gain name sets (clamped),
    a centre within 500 Hz of the frequency asked for. Transmitting holds the radio: no receive
    flowgraph meanwhile. Settings changed while transmitting restart the transmission.

    A periodic signal (an analog.sig_source_c tone, a repeating vector_source_c or file_source
    short enough for one DAC bank) loops at the DAC's own rate, 40 MS/s (80 MS/s for a sample
    rate of 80 MS/s), with the analog filters in their 40 MHz mode (+-18 MHz): any tone within
    the band, no CPU. Other signals stream through the interpolating DAC backend, gapless up to
    500 kS/s."""

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
        self._bw = 0
        self._tb = None
        self._loop = 0
        self._stream_rate = 250000
        self._link = None
        self.centre = None
        self.mode = None
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
        self._bw = float(bandwidth)
        self._restart()

    def set_antenna(self, channel, name):
        pass

    def set_dc_offset(self, channel, offset):
        pass

    def set_iq_balance(self, channel, balance):
        pass

    def set_frequency_correction(self, channel, ppm):
        if ppm:
            print("soapy: frequency correction not supported on the board")

    # -- runtime, called by gr.top_block --
    def _dac_fs(self):
        return 80000000 if self._rate == 80000000 else _DAC_HZ

    def _loop_gen(self, gen, fs):
        """gen with its cycles per DAC sample, or None when no loop holds it within 1 Hz."""
        wave, cycles, amp, dci, dcq = gen
        c = cycles * self._rate / fs
        if abs(c) >= 0.5:
            return None
        k, n = esdr.tx_period(c)
        if abs(c - k / n) * fs > 1.0:
            return None
        return (wave, c, amp, dci, dcq)

    def _plan(self, args, scale):
        """("loop", kwargs) or ("stream", kwargs) for this source."""
        tb = self._tb
        fs = self._dac_fs()
        if not tb._items:
            if "gen" in args:
                g = self._loop_gen(args["gen"](scale), fs)
                if g is not None:
                    return "loop", {"gen": g}
            elif args["repeat"] and fs % self._rate == 0:
                n = len(args["buf"]) // 4
                interp = fs // self._rate
                if _LOOP_MIN <= n * interp <= _LOOP_MAX:
                    return "loop", {"buf": args["buf"], "interp": interp, "scale": scale}
        if "gen" in args:
            # A generated signal is the same at any rate that holds it: stream it at the
            # fastest gapless rate, or the flowgraph's if that is lower.
            wave, cycles, amp, dci, dcq = args["gen"](scale)
            rate = min(self._rate, _TX_RATE_MAX_OK)
            c = cycles * self._rate / rate
            if abs(c) < 0.45 or wave == 0:
                self._stream_rate = rate
                return "stream", {"gen": (wave, c, amp, dci, dcq)}
        if self._rate * 2 > _DAC_HZ:
            raise ValueError("soapy: {} S/s only for a periodic signal (a loop at the DAC rate)"
                             .format(self._rate))
        if self._rate > _TX_RATE_MAX_OK:
            print("soapy: streaming above {} S/s is not gapless on the board".format(_TX_RATE_MAX_OK))
        self._stream_rate = self._rate
        if "gen" in args:
            return "stream", {"gen": args["gen"](scale)}
        return "stream", {"buf": args["buf"], "repeat": args["repeat"], "scale": scale}

    def _run(self):
        tb = self._tb
        mode, kw = self._plan(tb._source._tx_args(), tb._tx_scale())
        self.mode = mode
        # The 40 MHz mode opens the analog filters to +-18 MHz: for loops (any tone in the DAC's
        # band) or when the flowgraph asks for more than 20 MHz.
        esdr.channel_bw(2 if mode == "loop" or self._bw > 20e6 else 0)
        if mode == "loop":
            self._loop = self._dac_fs()
            self.centre = esdr.tx_loop(self._loop, self._freq, self._gain, **kw)
        else:
            self._loop = 0
            count = tb._items
            if count and self._stream_rate != self._rate:
                # head counts samples at the flowgraph's rate.
                count = max(1, count * self._stream_rate // self._rate)
            self.centre = esdr.tx_start(int(self._stream_rate), self._freq, self._gain, count=count, **kw)

    def _start(self, tb):
        self._tb = tb
        for b in tb._chain:
            b._tb = tb
        self._run()

    def _check_burst_rate(self):
        if self._rate > _TX_RATE_MAX_OK:
            raise ValueError("soapy: packet bursts stream at up to {} S/s".format(_TX_RATE_MAX_OK))

    def _burst(self, buf):
        """A packet flowgraph's burst (gnuradio/_link.py): buf played once, at the settings of
        the moment."""
        esdr.channel_bw(2 if self._bw > 20e6 else 0)
        self.mode = "burst"
        self.centre = esdr.tx_start(int(self._rate), self._freq, self._gain, buf=buf)

    def _restart(self):
        # A packet flowgraph's next burst takes the new settings.
        if self._link is not None:
            return
        if self._tb is not None and self._tb._running and esdr.tx_busy():
            esdr.tx_stop()
            self._run()

    def _live(self):
        tb = self._tb
        args = tb._source._tx_args()
        scale = tb._tx_scale()
        if "gen" not in args:
            esdr.tx_scale(scale)
            return
        if self._loop:
            g = self._loop_gen(args["gen"](scale), self._loop)
            if g is None:
                # No longer loops exactly (a tone moved below the loop's resolution): stream it.
                self._restart()
                return
            esdr.tx_gen(*g)
        else:
            wave, cycles, amp, dci, dcq = args["gen"](scale)
            c = cycles * self._rate / self._stream_rate
            if abs(c) >= 0.45 or self._loop_gen(args["gen"](scale), self._dac_fs()) is not None:
                # Out of the streaming rate's band, or now loops exactly: start over.
                self._restart()
                return
            esdr.tx_gen(wave, c, amp, dci, dcq)

    def _stop(self, tb):
        pass

    def stats(self):
        return esdr.tx_stats()
