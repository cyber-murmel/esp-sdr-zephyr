# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython/eggs/datalink: the unmodified GRC export with the board's gnuradio stand-ins,
the packet link (gnuradio/_link.py) and the flowgraph's MAC (its embedded Python block), on a
simulated half-duplex radio (a fake esdr module) against a simulated peer: the same MAC on
the Python modem. The fake esdr's FSK functions are the C modem (cmod/esdr_fsk.c) when the host
has a compiler, as on the board; otherwise the stand-ins fall back to the Python modem. The
"engine" radio runs the receiver itself and needs no burst padding, as the ESP32-C5's does;
"headless" runs the Qt GUI on the console (gnuradio/_console.py). Also the C5 image's RF switch
module on a fake machine.Pin."""

import array
import ctypes
import importlib
import math
import os
import random
import shutil
import subprocess
import sys
import time
import types

import pytest

from conftest import REPO, load

MODULES = REPO / "apps/micropython/modules"
EGG = REPO / "apps/micropython/eggs/datalink"
RATE = 250000


class Clock:
    def __init__(self):
        self.ms = 1000

    def ticks_ms(self):
        return self.ms


class Peer:
    """The other station: the MAC block on the Python modem, its radio idealised."""

    def __init__(self, sim):
        self.sim = sim
        gr = sys.modules["gnuradio.gr"]
        pmt = sys.modules["pmt"]
        fsk = sys.modules["gnuradio.digital._fsk"]
        self.pmt = pmt
        self.fsk = fsk
        self.mac = sys.modules["datalink_epy_block_0"].blk(auto=False, timeout=0.4, retries=5)
        self.radio = gr.basic_block("peer radio")
        self.radio.message_port_register_in("pdus")
        self.radio.set_msg_handler("pdus", self._tx)
        self.mac._msg_out["tx"] = [(self.radio, "pdus")]
        self.code, self.code_len = fsk.code_bits("01" * 16 + "11100100001011000110011001011101")
        self.taps = fsk.gfsk_taps(10, 0.5)
        self.sens = math.tau * 25e3 / RATE
        self.rx = fsk.Receiver(10, self.code, self.code_len, 8)
        self.heard = []
        self.drop_tx = 0  # frames of its own to lose
        self.next_tick = 0

    def _tx(self, msg):
        data = bytes(self.pmt.u8vector_elements(self.pmt.cdr(msg)))
        if self.drop_tx:
            self.drop_tx -= 1
            return
        frame = self.fsk.header(len(data), self.code, self.code_len) + data
        n = self.fsk.mod_samples(8 * len(frame), 10, len(self.taps))
        out = array.array("h", bytes(4 * n))
        self.fsk.modulate(frame, 8 * len(frame), 10, self.taps, self.sens, 0.5, out)
        self.sim.peer_burst(out)

    def hear(self, iq):
        """A burst of the board's: the peer's receiver and MAC get it."""
        sig = array.array("h", bytes(4 * 3000)) + iq + array.array("h", bytes(4 * 500))
        for f in self.rx.process(sig):
            meta = self.pmt.make_dict()
            meta = self.pmt.dict_add(meta, self.pmt.intern("snr"), self.pmt.from_double(30.0))
            self.mac._post("rx", self.pmt.cons(meta, self.pmt.init_u8vector(len(f.data), f.data)))
        self.rx.reset()

    def tick(self, now):
        if now >= self.next_tick:
            self.next_tick = now + 100
            self.mac._post("tick", self.pmt.PMT_NIL)


class FakeRadio(types.ModuleType):
    """esdr as the stand-ins use it in a packet flowgraph, half duplex as the board."""

    RATES = (250000, 125000, 62500, 31250, 15625)
    BURST_RATES = (80000000, 40000000, 16000000)
    FULL_SCALE = 16384

    def __init__(self, clock, clib, engine=False):
        super().__init__("esdr")
        self.clock = clock
        self.engine = engine
        self.frames = []
        if engine:
            # The C5: the engine runs the receiver at half the rate, bursts need no padding.
            self.fsk_frames = self._fsk_frames
            self.TX_PREBUF_S = 0.0
            self.RX_FSK_SLOW = 2
        self.rx_on = False
        self.tx_on = False
        self.count = 0
        self.air = []  # (first sample on the board's receive count, samples) from the peer
        self.bursts = []  # the board's bursts: (samples)
        self.peer = None
        self.lose_board = 0  # board bursts to lose
        # The peer's carrier against the frequency set (ref_hz), Hz: the board's receiver
        # sees it less its tuning's correction, the peer sees the board's bursts at minus it.
        self.ref_hz = None
        self.peer_offset = 0.0
        self.tuned = None
        self.tx_hz = []  # the board's transmit frequencies
        self.gaps = 0  # the receive run's lost stretches (rx_stats())
        self.starts = 0
        rnd = random.Random(3)
        self.noise = array.array("h", [int(rnd.gauss(0, 60)) for _ in range(8192)])
        if clib is not None:
            self._c(clib)

    # -- radio --
    def init(self):
        pass

    def tune(self, hz, offset=True):
        self.tuned = float(hz)
        return float(hz)

    def rx_gain(self, *a):
        return 30

    def rx_gain_max(self):
        return 82

    def tx_gain_max(self):
        return 17

    def rx_bandwidth(self, hz):
        pass

    def channel_bw(self, mode):
        if self.rx_on or self.tx_on:
            raise OSError(16, "busy")

    def rx_start(self, rate, buf, count=0, offset=True, monitor=False, wrap=False, fsk=None,
                 slow=1):
        if self.tx_on or self.rx_on:
            raise OSError(16, "busy")
        assert count == 0 and rate == RATE
        if self.engine:
            assert buf is None and fsk is not None and slow == 2
            self.fsk_state = fsk
            self.slow = slow
        else:
            assert wrap and fsk is None
            self.ring = buf
            self.cap = len(buf) // 2
        self.count = 0
        self.rx_on = True
        self.rx_t = self.clock.ms
        self.gaps = 0
        self.starts += 1

    def rx_stop(self):
        self.rx_on = False

    def rx_count(self):
        return self.count

    def rx_stats(self):
        return {"samples": self.count, "gaps": self.gaps}

    def tx_start(self, rate, hz, gain, buf=None, repeat=False, count=0, scale=None, gen=None):
        if self.rx_on or self.tx_on:
            raise OSError(16, "busy")
        assert rate == RATE and buf is not None
        self.tx_on = True
        # Any buffer of int16 I/Q, as the board's binding takes it.
        self.tx_buf = array.array("h", bytes(memoryview(buf)))
        self.tx_t = self.clock.ms + 13  # start latency
        self.tx_hz.append(float(hz))
        return float(hz)

    def tx_busy(self):
        return self.tx_on

    def tx_stats(self):
        played = max(0, (self.clock.ms - self.tx_t) * RATE // 1000)
        return {"played": min(played, len(self.tx_buf) // 2)}

    def tx_stop(self):
        if self.tx_on:
            self.tx_on = False
            n = self.tx_stats()["played"]
            iq = self.tx_buf[:2 * n]
            self.bursts.append(iq)
            if self.lose_board:
                self.lose_board -= 1
            elif self.peer is not None:
                self.peer.hear(shift(iq, -self.peer_offset))

    # -- simulation --
    def peer_burst(self, iq):
        """The peer transmits once the board's receiver has run for 25 ms more (its own
        turnaround); a burst while the board transmits is lost to it."""
        start = self.count + 25 * RATE // 1000 if self.rx_on else None
        if start is not None:
            residual = self.peer_offset
            if self.ref_hz is not None:
                residual -= self.tuned - self.ref_hz
            self.air.append((start, shift(iq, residual)))

    def advance(self, ms):
        self.clock.ms += ms
        if not self.rx_on:
            self.air = []
            return
        n = ms * RATE // 1000
        block = array.array("h", bytes(4 * n))
        for k in range(n):
            s = self.count + k
            j = 2 * (s % 4096)
            i, q = self.noise[j], self.noise[j + 1]
            for start, iq in self.air:
                if start <= s < start + len(iq) // 2:
                    i += iq[2 * (s - start)] // 2
                    q += iq[2 * (s - start) + 1] // 2
            block[2 * k] = i
            block[2 * k + 1] = q
        if self.engine:
            # The engine's receiver takes the samples as they come, pairs averaged; its frame
            # ends count at the full rate.
            half = array.array("h", bytes(2 * n))
            for k in range(n // 2):
                half[2 * k] = (block[4 * k] + block[4 * k + 2]) // 2
                half[2 * k + 1] = (block[4 * k + 1] + block[4 * k + 3]) // 2
            for t in self.fsk_rx(self.fsk_state, half, 0, n // 2):
                self.frames.append(t[:2] + (t[2] * 2,) + t[3:])
        else:
            for k in range(n):
                pos = 2 * ((self.count + k) % self.cap)
                self.ring[pos] = block[2 * k]
                self.ring[pos + 1] = block[2 * k + 1]
        self.count += n
        self.air = [(st, iq) for st, iq in self.air if st + len(iq) // 2 > self.count]

    def _fsk_frames(self):
        out, self.frames = self.frames, []
        return out

    # -- the C modem, as esdr's bindings --
    def _c(self, lib):
        frame_t = _cframe()
        emit_t = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(frame_t))
        lib.esdr_fsk_rx_size.restype = ctypes.c_size_t
        lib.esdr_fsk_rx_size.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        lib.esdr_fsk_rx_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
                                         ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32,
                                         ctypes.c_uint32]
        lib.esdr_fsk_rx_process.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                            emit_t, ctypes.c_void_p]
        lib.esdr_fsk_rx_state.restype = ctypes.c_uint32
        lib.esdr_fsk_rx_state.argtypes = [ctypes.c_void_p]
        lib.esdr_fsk_rx_reset.argtypes = [ctypes.c_void_p]
        lib.esdr_fsk_mod.restype = ctypes.c_size_t
        lib.esdr_fsk_mod.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32,
                                     ctypes.c_void_p, ctypes.c_size_t, ctypes.c_float,
                                     ctypes.c_float, ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.c_float)]

        def fsk_rx_init(sps, code, code_len, threshold, max_len):
            size = lib.esdr_fsk_rx_size(sps, code_len)
            st = ctypes.create_string_buffer(size)
            assert lib.esdr_fsk_rx_init(st, size, sps, int.from_bytes(code, "big"), code_len,
                                        threshold, max_len) == 0
            return st

        def fsk_rx(st, buf, start, n):
            out = []

            def emit(user, f):
                f = f.contents
                out.append((bytes(f.data[:f.len]), f.errors, f.end, f.phase, f.dc, f.amp,
                            f.var, f.power, f.floor))

            cb = emit_t(emit)
            view = (ctypes.c_int16 * (2 * n)).from_buffer(buf, 4 * start)
            lib.esdr_fsk_rx_process(st, view, n, cb, None)
            return out

        def fsk_mod(data, nbits, sps, taps, sens, amp, out, phase):
            ph = ctypes.c_float(phase)
            t = (ctypes.c_float * len(taps)).from_buffer(taps)
            nbytes = memoryview(out).nbytes
            o = (ctypes.c_int16 * (nbytes // 2)).from_buffer(out)
            n = lib.esdr_fsk_mod(bytes(data), nbits, sps, t, len(taps), sens, amp, o,
                                 nbytes // 4, ctypes.byref(ph))
            # As the binding: the rest of out zeroed.
            ctypes.memset(ctypes.addressof(o) + 4 * n, 0, nbytes - 4 * n)
            return n, ph.value

        self.fsk_rx_init = fsk_rx_init
        self.fsk_rx = fsk_rx
        self.fsk_rx_reset = lib.esdr_fsk_rx_reset
        self.fsk_rx_state = lib.esdr_fsk_rx_state
        self.fsk_mod = fsk_mod


def shift(iq, hz):
    """int16 I/Q moved by hz."""
    if not hz:
        return iq
    out = array.array("h", bytes(2 * len(iq)))
    w = math.tau * hz / RATE
    for k in range(len(iq) // 2):
        c, s = math.cos(w * k), math.sin(w * k)
        i, q = iq[2 * k], iq[2 * k + 1]
        out[2 * k] = int(round(i * c - q * s))
        out[2 * k + 1] = int(round(i * s + q * c))
    return out


def _cframe():
    class CFrame(ctypes.Structure):
        _fields_ = [("len", ctypes.c_uint32), ("errors", ctypes.c_uint32),
                    ("end", ctypes.c_uint32), ("phase", ctypes.c_uint32),
                    ("dc", ctypes.c_double), ("amp", ctypes.c_double), ("var", ctypes.c_double),
                    ("power", ctypes.c_double), ("floor", ctypes.c_uint32),
                    ("data", ctypes.c_uint8 * 255)]
    return CFrame


@pytest.fixture(scope="module")
def clib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        return None
    out = tmp_path_factory.mktemp("fsk") / "libesdr_fsk.so"
    subprocess.run([cc, "-O2", "-shared", "-fPIC", "-o", str(out),
                    str(REPO / "apps/micropython/cmod/esdr_fsk.c"), "-lm"], check=True)
    return ctypes.CDLL(str(out))


OWN = ("gnuradio", "pmt", "PyQt5", "sip", "packaging", "esdr", "framebuf", "datalink",
       "datalink_epy_block_0")


@pytest.fixture(params=["c", "python", "engine", "headless"])
def sim(request, monkeypatch, clib):
    mode = request.param
    if mode != "python" and clib is None:
        pytest.skip("no host C compiler")
    clock = Clock()
    radio = FakeRadio(clock, clib if mode != "python" else None, engine=mode == "engine")
    saved = {k: v for k, v in sys.modules.items() if k.split(".")[0] in OWN}
    for k in saved:
        del sys.modules[k]
    framebuf = types.ModuleType("framebuf")
    framebuf.RGB565 = 1
    framebuf.FrameBuffer = object
    monkeypatch.setitem(sys.modules, "esdr", radio)
    monkeypatch.setitem(sys.modules, "framebuf", framebuf)
    # The MACs seed their randomness (and ids) from os.urandom: the same run every time.
    counter = iter(range(1, 1 << 30))
    monkeypatch.setattr(os, "urandom", lambda n: bytes((next(counter) * 37 + k) & 255
                                                       for k in range(n)))
    monkeypatch.setattr(time, "ticks_ms", clock.ticks_ms, raising=False)
    monkeypatch.setattr(time, "ticks_add", lambda a, b: a + b, raising=False)
    monkeypatch.setattr(time, "ticks_diff", lambda a, b: a - b, raising=False)
    monkeypatch.syspath_prepend(str(EGG))
    monkeypatch.syspath_prepend(str(MODULES))
    if mode == "headless":
        importlib.import_module("gnuradio._gui")._headless = True
    export = importlib.import_module("datalink")
    tb = export.datalink()
    radio.peer = Peer(radio)
    yield types.SimpleNamespace(tb=tb, radio=radio, peer=radio.peer, clock=clock, mode=mode,
                                gui=sys.modules["gnuradio._gui"], gr=sys.modules["gnuradio.gr"])
    tb.stop()
    for k in [k for k in sys.modules if k.split(".")[0] in OWN]:
        del sys.modules[k]
    sys.modules.update(saved)


def run(s, ms, step=10):
    """The Qt GUI loop's passes (timers, pollers) for ms of simulated time; headless, the
    console's timer callback."""
    for _ in range(ms // step):
        s.radio.advance(step)
        s.peer.tick(s.clock.ms)
        if s.mode == "headless":
            importlib.import_module("gnuradio._console").tick()
            continue
        for t in s.gui._timers:
            if t._ms and s.clock.ms - t._next >= 0:
                t._next = s.clock.ms + t._ms
                t.timeout.emit()
        for fn in s.gui._pollers[:]:
            fn()
        s.gr.dispatch()


def test_plan(sim):
    tb = sim.tb
    tb.start()
    link = tb._link
    assert link is not None and link.mod is tb.digital_gfsk_mod_0
    assert link.through == [tb.blocks_multiply_const_vxx_0]
    assert link.rx_chain == [tb.blocks_repack_bits_bb_0]
    assert link.to_pdu is tb.pdu_tagged_stream_to_pdu_0
    assert sim.radio.rx_on and not sim.radio.tx_on


def test_send_and_acknowledge(sim):
    tb = sim.tb
    if sim.mode == "headless":
        # What exec_() does first: the controls on the console, its timer's passes from then on.
        console = importlib.import_module("gnuradio._console")
        console._on = True
        console.show()
    tb.start()
    run(sim, 300)
    tb.set_msg("Ping")
    tb.send._clicked()
    run(sim, 600)
    assert sim.peer.mac.heard.endswith("#1: Ping")
    assert tb.get_tx_text().startswith("1 sent, 1 acked, 0 again, 0 lost")
    assert tb.get_peer_text().startswith("30 dB, rtt ")
    # The board's bursts: the data frame, nothing else (the peer acknowledged at once).
    assert len(sim.radio.bursts) == 1
    # The peer's message comes the other way.
    sim.peer.mac._post("send", sim.peer.pmt.intern("hi there"))
    run(sim, 600)
    assert tb.get_rx_text().endswith("#1: hi there")
    assert tb.get_link_text().endswith(", 0 bad")
    assert " dB, " in tb.get_link_text() and "kHz" in tb.get_link_text()
    assert sim.peer.mac.n["acked"] == 1
    assert len(sim.radio.bursts) == 2  # its acknowledgement


def test_lost_frames_are_sent_again(sim):
    tb = sim.tb
    tb.start()
    run(sim, 200)
    sim.radio.lose_board = 2  # the first two copies of the board's frame
    tb.send._clicked()
    run(sim, 3000)
    assert tb.get_tx_text().startswith("1 sent, 1 acked, 2 again, 0 lost")
    assert sim.peer.mac.n["heard"] == 1
    # A lost acknowledgement: the data comes again, it is acknowledged again, shown once.
    sim.peer.drop_tx = 1
    tb.send._clicked()
    run(sim, 3000)
    assert tb.get_tx_text().startswith("2 sent, 2 acked, 3 again, 0 lost")
    assert sim.peer.mac.n["heard"] == 2 and sim.peer.mac.n["dup"] == 1


def test_auto_both_ways(sim):
    tb = sim.tb
    tb.set_auto(True)
    sim.peer.mac.auto = True
    tb.start()
    run(sim, 12000)
    mine, peers = sim.tb.epy_block_0.n, sim.peer.mac.n
    assert mine["msgs"] >= 4 and peers["msgs"] >= 4
    # Both send at random times: a message may collide often enough to be given up.
    assert mine["lost"] + peers["lost"] <= 1
    assert mine["acked"] >= mine["msgs"] - 2 and peers["acked"] >= peers["msgs"] - 2
    assert mine["heard"] >= peers["msgs"] - 2
    assert tb.get_rx_text().split(": ")[-1].startswith("auto ")


def test_afc_follows_the_peer(sim):
    """The peer's carrier 24 kHz up (two boards' crystals): the receive tuning follows it after
    the first frame, the transmitter stays on the frequency set, the link line shows the
    peer's offset; a new frequency starts the correction again."""
    tb = sim.tb
    sim.radio.ref_hz = tb.get_freq()
    sim.radio.peer_offset = 24e3
    tb.start()
    run(sim, 200)
    for k in range(3):
        sim.peer.mac._post("send", sim.peer.pmt.intern("afc {}".format(k)))
        run(sim, 800)
    assert tb.get_rx_text().endswith(": afc 2")
    assert sim.peer.mac.n["acked"] == 3
    assert sim.radio.tuned - sim.radio.ref_hz == pytest.approx(24e3, abs=1.5e3)
    assert sim.radio.tx_hz and set(sim.radio.tx_hz) == {sim.radio.ref_hz}
    offset = float(tb.get_link_text().split(", ")[2].split()[0])
    assert offset == pytest.approx(24.0, abs=1.5)
    tb.set_freq(2.45e9)
    assert sim.radio.tuned == pytest.approx(2.45e9)


def test_lossy_ring_run_starts_again(sim):
    """A ring receive run that keeps losing input starts again when idle, once; one that loses
    a little does not."""
    if sim.mode == "engine":
        pytest.skip("the ring radio")
    tb = sim.tb
    tb.start()
    run(sim, 600)
    starts = sim.radio.starts
    sim.radio.gaps = 5
    run(sim, 1000)
    assert sim.radio.starts == starts and tb._link.stats["restarts"] == 0
    sim.radio.gaps = 500
    run(sim, 1000)
    assert sim.radio.starts == starts + 1 and tb._link.stats["restarts"] == 1
    run(sim, 1000)
    assert sim.radio.starts == starts + 1
    # Still receiving.
    sim.peer.mac._post("send", sim.peer.pmt.intern("after"))
    run(sim, 600)
    assert tb.get_rx_text().endswith(": after")


def test_engine_mode_plan(sim):
    if sim.mode != "engine":
        pytest.skip("the engine radio")
    sim.tb.start()
    link = sim.tb._link
    assert link.engine and link.ring is None
    assert sim.radio.fsk_state is link.rx._st


def test_headless_console(sim, capsys):
    if sim.mode != "headless":
        pytest.skip("the console")
    console = sys.modules.get("gnuradio._console") or importlib.import_module("gnuradio._console")
    tb = sim.tb
    console._on = True
    console.show()
    out = capsys.readouterr().out
    assert "== Data link ==" in out and "-- Link --" in out and "-- Radio --" in out
    names = [console._describe(w, c) for w, c in console._controls]
    text = [k for k, d in enumerate(names) if d.startswith("Text: Hello")][0] + 1
    send = [k for k, d in enumerate(names) if d.startswith("<Send>")][0] + 1
    auto = [k for k, d in enumerate(names) if "Auto" in d][0] + 1
    freq = [k for k, d in enumerate(names) if d.startswith("Frequency")][0] + 1
    tb.start()
    run(sim, 200)
    console.command("{} ping".format(text))
    assert tb.get_msg() == "Ping"
    console.command(str(send))
    run(sim, 600)
    assert sim.peer.mac.heard.endswith(": Ping")
    console.command("{} hello from the console".format(send))
    run(sim, 600)
    assert sim.peer.mac.heard.endswith(": hello from the console")
    out = capsys.readouterr().out
    assert "TX: 2 sent, 2 acked" in out
    console.command(str(auto))
    assert tb.get_auto() is True and tb.epy_block_0.auto is True
    console.command("{} 2.45G".format(freq))
    assert tb.get_freq() == pytest.approx(2.45e9)
    assert tb.soapy_custom_sink_0.get_frequency() == pytest.approx(2.45e9)
    console.command("!print('ns', 6 * 7)")
    console.command("99")
    out = capsys.readouterr().out
    assert "ns 42" in out and "controls 1 .. " in out


def test_rfswitch(monkeypatch, tmp_path):
    levels = {}

    class Pin:
        OUT = 3

        def __init__(self, id, mode):
            assert id[0] == "gpio@60091000" and mode == Pin.OUT
            self.n = id[1]
            levels.setdefault(self.n, 0)

        def value(self, v=None):
            if v is None:
                return levels[self.n]
            levels[self.n] = v

    machine = types.ModuleType("machine")
    machine.Pin = Pin
    monkeypatch.setitem(sys.modules, "machine", machine)
    rf = load("apps/micropython/modules_c5/rfswitch.py", "rfswitch_test")
    assert rf.select(a=4, b=1) == (4, 1)
    # Port n is n - 1 on bits A, B, C.
    assert [levels[n] for n in (0, 1, 2)] == [1, 1, 0]
    assert [levels[n] for n in (8, 9, 10)] == [0, 0, 0]
    assert rf.select(b=7) == (4, 7)
    assert [levels[n] for n in (8, 9, 10)] == [0, 1, 1]
    with pytest.raises(ValueError):
        rf.select(a=9)
    assert rf.boot() == (4, 1)


def test_mac_metadata_none(sim, monkeypatch):
    # GNU Radio's pmt.to_python() gives None for an empty metadata dictionary.
    if sim.mode != "c":
        pytest.skip("one mode")
    pmt = sys.modules["pmt"]
    real = pmt.to_python
    monkeypatch.setattr(pmt, "to_python", lambda p: None if pmt.is_dict(p) and not p.value
                        else real(p))
    mac = sim.tb.epy_block_0
    mod = sys.modules["datalink_epy_block_0"]
    data = mod.frame(mod.DATA, 0x1234, 9, b"meta")
    mac._rx(pmt.cons(pmt.make_dict(), pmt.init_u8vector(len(data), data)))
    assert mac.n["heard"] == 1 and mac.heard.endswith("#9: meta")
