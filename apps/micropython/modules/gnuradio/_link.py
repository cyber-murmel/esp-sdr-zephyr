# SPDX-License-Identifier: GPL-3.0-or-later
"""Packet flowgraphs on the board's radio, which receives or transmits, never both (half
duplex). gr.top_block plans a flowgraph here when it has the packet blocks:

  receive   soapy.source -> digital.gfsk_demod -> digital.correlate_access_code_bb_ts
            -> [blocks.repack_bits_bb ...] -> pdu.tagged_stream_to_pdu => messages
  transmit  messages => pdu.pdu_to_tagged_stream -> [digital.protocol_formatter_bb,
            blocks.tagged_stream_mux, ...] -> digital.gfsk_mod -> [blocks.multiply_const_cc]
            -> soapy.sink

Either chain may be missing. The stream blocks between the PDU blocks see one packet at a time
(a tagged stream's packet is a burst on the air). The radio receives into a ring in memory
(esdr.rx_start(wrap=True)), which esdr's FSK receiver (C, as digital/_fsk.py) reads at every
pass of the Qt GUI loop; where the radio engine runs the receiver itself (the ESP32-C5, short
of RAM for a ring: esdr.rx_start(fsk=...)), the pass collects its frames (esdr.fsk_frames()).
The frames come out of tagged_stream_to_pdu as PDUs whose metadata
holds what the access code measured: "snr" (dB), "rssi" (dBFS), "freq_offset" (Hz), "errors"
(access code bit errors). A PDU into pdu_to_tagged_stream becomes a burst: the receiver stops,
the burst plays (soapy.sink's frequency and gain), the receiver starts again. A burst waits
while a frame is being received (up to DEFER_MS) and for GUARD_MS after one, the time the
peer takes to listen again; bursts waiting go out newest first (a reply to a frame just heard
before what was waiting for it to end).

The receiver follows the peer's carrier (AFC): two boards' crystals differ by up to some
10 ppm, 24 kHz at 2.44 GHz, which leaves one tone at the edge of a receiver at half the rate
(the ESP32-C5's) and costs it bits. The carrier offset of a frame whose access code came with
few errors moves the receive tuning by that much, at the receiver's next start (after a burst)
or at once when it is idle with nothing to send. The transmitter stays on the frequency set,
so that two boards do not chase each other. "freq_offset" is the peer's carrier against the
frequency set; a new frequency starts the correction again.
"""

import time

import esdr
from gnuradio import _gui, gr

# The guard: the ESP32-C5 listens again some 25 ms after its burst (retuning takes 19 ms).
GUARD_MS = 40
DEFER_MS = 300
# The capture ring, and the most a pass takes from it (0.1 s at 250 kS/s).
RING_S = 0.5
PASS_SAMPLES = 25000
# The transmitter starts once this much is queued (CONFIG_ESP_SDR_TX_DAC_PREBUF_MS, the S3; a
# board's esdr.TX_PREBUF_S says otherwise): bursts are padded with silence to it, and stopped
# once their samples have played.
PREBUF_S = 0.02
PAD_SAMPLES = 256
# A burst that has not played within its length and this long is stopped regardless.
TX_LATE_MS = 300
# AFC: offsets below AFC_MIN_HZ leave the tuning alone, the correction stays within
# AFC_MAX_HZ, frames with more than AFC_ERRORS access code bit errors do not count.
AFC_MIN_HZ = 2000
AFC_MAX_HZ = 60000
AFC_ERRORS = 2
# A ring receive run that keeps losing input, more than GAPS_MAX stretches of samples within
# GAPS_MS, starts again when idle. (A run on the S3 that started while the display drew did so
# for as long as it lasted, until the ring's filter scheduling followed the recent slices.)
GAPS_MS = 500
GAPS_MAX = 10


def _follow(edges, block):
    return [(dst, dp) for src, sp, dst, dp in edges if src is block]


def _inputs(edges, block):
    found = [(dp, src) for src, sp, dst, dp in edges if dst is block]
    found.sort(key=lambda t: t[0])
    return [src for _, src in found]


class Link:
    def __init__(self, tb, blocks):
        self.tb = tb
        self.edges = tb._edges
        kinds = ("source", "txsink", "through") + gr._LINK_KINDS
        for b in blocks:
            if b._kind not in kinds:
                raise ValueError("block not supported in a packet flowgraph on the board: " +
                                 b.name())
        sources = [b for b in blocks if b._kind == "source"]
        sinks = [b for b in blocks if b._kind == "txsink"]
        if len(sources) > 1 or len(sinks) > 1:
            raise ValueError("a packet flowgraph has one soapy.source and one soapy.sink at most")
        self.source = sources[0] if sources else None
        self.sink = sinks[0] if sinks else None
        if self.source is None and self.sink is None:
            raise ValueError("a packet flowgraph needs soapy.source or soapy.sink")
        self.rate = (self.source or self.sink).get_sample_rate()
        self._plan_rx()
        self._plan_tx()
        self.ring = None
        # The radio engine runs the receiver (esdr.fsk_frames()), or the passes do on a ring.
        self.engine = hasattr(esdr, "fsk_frames")
        self.rx_on = False
        self.tx = False
        self.bursts = []
        self.txbuf = None  # one buffer for the bursts, used again
        self.afc_want = None  # a receive tuning correction (Hz) for the receiver's next start
        self.poll_fn = self._poll
        self.stats = {"frames": 0, "bursts": 0, "overruns": 0, "deferred": 0, "restarts": 0}

    # ---- planning ----

    def _plan_rx(self):
        self.demod = self.sync = self.to_pdu = self.rx = None
        self.rx_chain = []
        if self.source is None:
            return
        nxt = _follow(self.edges, self.source)
        if len(nxt) != 1 or nxt[0][0]._kind != "demod":
            raise ValueError("soapy.source feeds digital.gfsk_demod only in a packet flowgraph")
        self.demod = nxt[0][0]
        nxt = _follow(self.edges, self.demod)
        if len(nxt) != 1 or nxt[0][0]._kind != "sync":
            raise ValueError("gfsk_demod feeds digital.correlate_access_code_bb_ts")
        self.sync = nxt[0][0]
        b = self.sync
        while True:
            nxt = _follow(self.edges, b)
            if len(nxt) != 1:
                raise ValueError("{} feeds one block, towards pdu.tagged_stream_to_pdu".format(
                    b.name()))
            b = nxt[0][0]
            if b._kind == "ts2pdu":
                self.to_pdu = b
                break
            if b._kind != "pkt":
                raise ValueError("{} on the way to tagged_stream_to_pdu".format(b.name()))
            self.rx_chain.append(b)
        if self.rate not in esdr.RATES:
            raise ValueError("a packet receiver runs at one of {}".format(esdr.RATES))
        sps = self.demod.samples_per_symbol()
        self.rx = self.sync._receiver(sps)

    def _plan_tx(self):
        self.mod = None
        self.through = []
        self.from_pdu = []
        if self.sink is None:
            return
        b = self.sink
        while True:
            ins = _inputs(self.edges, b)
            if len(ins) != 1:
                raise ValueError("{} takes one input in a packet flowgraph".format(b.name()))
            b = ins[0]
            if b._kind == "mod":
                self.mod = b
                break
            if b._kind != "through":
                raise ValueError("{} between gfsk_mod and soapy.sink".format(b.name()))
            self.through.append(b)
        # The packet blocks before the modulator, back to the PDU blocks that feed them.
        todo = [self.mod]
        seen = []
        while todo:
            b = todo.pop()
            for src in _inputs(self.edges, b):
                if src in seen:
                    continue
                seen.append(src)
                if src._kind == "pdu2ts":
                    src._link = self
                    self.from_pdu.append(src)
                elif src._kind == "pkt":
                    todo.append(src)
                else:
                    raise ValueError("{} before gfsk_mod".format(src.name()))
        if len(self.from_pdu) != 1:
            raise ValueError("a packet transmitter starts at one pdu.pdu_to_tagged_stream")
        if self.sink.get_sample_rate() != self.rate:
            raise ValueError("soapy.source and soapy.sink at the same sample rate")
        self.sink._check_burst_rate()

    # ---- running ----

    def start(self):
        if not hasattr(esdr, "rx_count"):
            raise OSError("this firmware has no ring capture for packet flowgraphs: update it")
        import array

        if self.source is not None:
            self.source._link = self
            if not self.engine:
                self.ring = array.array("h", bytes(4 * int(RING_S * self.rate)))
            self._rx_start()
        if self.sink is not None:
            self.sink._link = self
            import gnuradio

            self.txbuf = gnuradio._txbuf
        _gui.poll(self.poll_fn)

    def stop(self):
        _gui.poll(self.poll_fn, False)
        if self.tx:
            esdr.tx_stop()
            self.tx = False
        if self.rx_on:
            esdr.rx_stop()
            self.rx_on = False
        self.bursts = []
        for b in (self.source, self.sink):
            if b is not None:
                b._link = None

    def _rx_start(self):
        if self.afc_want is not None:
            self.source._afc = self.afc_want
            self.afc_want = None
        self.rx.reset()
        if self.engine:
            self.source._rx_fsk(self.rx._st, self.rx.slow)
        else:
            self.source._rx_ring(self.ring)
        self.rx_on = True
        self.read = 0
        self.base = 0  # where the receiver's own count started
        self.last_end = -(1 << 30)
        self.defer_t = None
        self.gaps_t = time.ticks_ms()
        self.gaps = 0

    def retune(self):
        """soapy.source's settings changed: the receiver starts again with them."""
        if self.rx_on:
            esdr.rx_stop()
            self._rx_start()

    def _poll(self):
        if self.tx:
            st = esdr.tx_stats()
            if st["played"] < self.tx_n and esdr.tx_busy() and \
                    time.ticks_diff(time.ticks_ms(), self.tx_end) < 0:
                return
            self._tx_done()
        if self.rx_on:
            self._receive()
        gr.dispatch()
        if self.bursts and self._may_send():
            self._send_burst()
        elif self.afc_want is not None and self.rx_on and not self.bursts and \
                not self.rx.busy():
            # A new receive tuning at once; a frame just ended, the guard counts from here.
            self.retune()
            self.last_end = 0
        elif self.ring is not None and self.rx_on and self._losing():
            self.stats["restarts"] += 1
            self.retune()

    def _losing(self):
        """The ring receiver keeps losing input (GAPS_MS) and may start again now."""
        now = time.ticks_ms()
        if time.ticks_diff(now, self.gaps_t) < GAPS_MS:
            return False
        gaps = esdr.rx_stats()["gaps"]
        lost = gaps - self.gaps > GAPS_MAX
        self.gaps_t, self.gaps = now, gaps
        return lost and not self.bursts and not self.rx.busy()

    def _receive(self):
        if self.engine:
            self.read = esdr.rx_count()
            frames = [self.rx.frame(t) for t in esdr.fsk_frames()]
        else:
            frames = self._ring_frames()
        for f in frames:
            self.stats["frames"] += 1
            self.last_end = self.base + f.end
            self._deliver(f)

    def _ring_frames(self):
        total = esdr.rx_count()
        cap = len(self.ring) // 2
        if total - self.read > cap - 4096:
            # The loop fell behind the ring: start the receiver afresh behind the writer.
            self.stats["overruns"] += 1
            self.read = self.base = total - 2048
            self.rx.reset()
        n = min(total - self.read, PASS_SAMPLES)
        frames = []
        while n > 0:
            pos = self.read % cap
            m = min(n, cap - pos)
            frames += self.rx.process(self.ring, m, pos)
            self.read += m
            n -= m
        return frames

    def _deliver(self, f):
        import math

        sps = self.demod.samples_per_symbol()
        fs = esdr.FULL_SCALE
        off = f.offset_hz(self.rate, sps)  # against the receive tuning
        afc = self.source._afc
        if f.errors <= AFC_ERRORS and abs(off) >= AFC_MIN_HZ:
            self.afc_want = max(-AFC_MAX_HZ, min(AFC_MAX_HZ, afc + off))
        meta = {
            "snr": f.snr_db(),
            "rssi": 10 * math.log10(max(f.power, 1) / (fs * fs)),
            "freq_offset": afc + off,
            "errors": f.errors,
        }
        v = gr.Bits(f.data, 8 * len(f.data))
        for b in self.rx_chain:
            v = b._pkt([v])
        self.to_pdu._emit(v, meta)

    def send(self, block, data):
        """pdu_to_tagged_stream block took a PDU: its packet waits for the radio, modulated
        when its burst goes out (one burst buffer: none is prepared while another plays)."""
        vals = {block: data}
        items = self._value(_inputs(self.edges, self.mod)[0], vals)
        amp = 1.0
        for b in self.through:
            amp *= b._scale
        # Newest first: a reply to the frame just heard goes before what waited for it.
        self.bursts.insert(0, (items, amp))
        del self.bursts[8:]

    def _modulate(self, items, amp):
        pad = int(getattr(esdr, "TX_PREBUF_S", PREBUF_S) * self.rate) + PAD_SAMPLES
        try:
            buf, n, used = self.mod._burst(items, amp, pad, self.txbuf)
        except MemoryError:
            # A packet beyond the buffer on a small heap (the ESP32-C5): this burst is lost (the
            # link's peer sends again).
            self.stats["no_memory"] = self.stats.get("no_memory", 0) + 1
            print("link: no memory for a burst of {} bytes".format(len(items)))
            return None, 0
        if isinstance(buf, bytearray):
            if self.txbuf is None or len(buf) > len(self.txbuf):
                import gnuradio

                self.txbuf = gnuradio._txbuf = buf
            buf = memoryview(buf)[:4 * used]
        return buf, n

    def _value(self, b, vals):
        if b in vals:
            return vals[b]
        if b._kind == "pdu2ts":
            raise ValueError("no packet from " + b.name())
        v = b._pkt([self._value(src, vals) for src in _inputs(self.edges, b)])
        vals[b] = v
        return v

    def _may_send(self):
        if not self.rx_on:
            return True
        now = time.ticks_ms()
        if self.rx.busy():
            if self.defer_t is None:
                self.defer_t = now
                self.stats["deferred"] += 1
            if time.ticks_diff(now, self.defer_t) < DEFER_MS:
                return False
        self.defer_t = None
        return self.read - self.last_end >= GUARD_MS * self.rate // 1000

    def _send_burst(self):
        if self.rx_on:
            # Before the modulation: on the ESP32-C5 the receiver takes most of its one CPU.
            esdr.rx_stop()
            self.rx_on = False
        burst, n = self._modulate(*self.bursts.pop(0))
        if burst is None:
            if self.source is not None:
                self._rx_start()
            return
        self.sink._burst(burst)
        self.tx = True
        self.tx_n = n
        self.tx_end = time.ticks_add(time.ticks_ms(), n * 1000 // self.rate + TX_LATE_MS)
        self.stats["bursts"] += 1
        if not esdr.tx_busy():
            # It played inside tx_start (the ESP32-C5): listen again at once, not a pass later.
            self._tx_done()

    def _tx_done(self):
        esdr.tx_stop()
        self.tx = False
        if self.source is not None:
            self._rx_start()
