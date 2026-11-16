# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/micropython's Scan app (eggs/scan): the plan (scan_view.py: every panorama bin read once a
plan, within the guard and the half span), the edges' bisection, and the app on a fake radio of
each flavour (the S3's modesdr, the P4's link to its C6, the S31) whose esdr.scan_hop() is the
real C (cmod/esdr_scan.c): tones land in their bins, the holds, a refused or stuck hop, a
clipped one, a gain change, the tabs, headless lines and Save's rtl_power lines."""

import array
import ctypes
import errno
import importlib
import sys
import time
import types

import numpy as np
import pytest

from conftest import REPO

MODULES = REPO / "apps/micropython/modules"
EGG = REPO / "apps/micropython/eggs/scan"
OWN = ("gnuradio", "pmt", "PyQt5", "sip", "packaging", "esdr", "framebuf", "scan", "scan_view",
       "dusk")
FS = 80_000_000


def addr(buf):
    return ctypes.addressof((ctypes.c_char * len(memoryview(buf).cast("B"))).from_buffer(buf))


class FakeRadio(types.ModuleType):
    """esdr of one flavour over a sky of tones (Hz, dBFS) and noise."""

    def __init__(self, lib, flavour, sky=(), noise=300.0, lock=None, workers=True):
        super().__init__("esdr")
        self.lib = lib
        # c6old: a C6 firmware from before ESDR_STATUS_LO_*: it never tells the lock.
        self.flavour = flavour
        self.RADIO = {"s3": "esp32s3", "c6": "esp32c6", "c6old": "esp32c6",
                      "s31": "esp32s31"}[flavour]
        self.sky = list(sky)
        self.noise = noise
        self.lock = lock or {"s3": (1832, 2787), "c6": (1790, 2850), "c6old": (1790, 2850),
                             "s31": (2200, 2875)}[flavour]
        self.lo = 2400_000_000
        self.took = self.lo
        self.calls = []
        self.claimed = []
        self.refuse = set()        # LOs (MHz) refused once more
        self.stuck = set()         # LOs whose capture never ends
        self.lossy = set()         # LOs whose capture loses a block once (the link's gaps)
        self.busy = False
        self.stats = {"gaps": 0, "resyncs": 0}
        self.count = 0
        self.gain = 0
        self.scan = ctypes.create_string_buffer(65536)
        # The app's clock (ms): each call takes the time it takes on the XIAO (scan_hop: its FFT).
        self.clock = [0]
        lib.esdr_scan_init(self.scan)
        self.rng = np.random.default_rng(1)
        # The worker (scan_start()): the buffer it may still read, its result, the hops folded
        # from it and those made in scan_hop().
        self.workers = workers
        self.worker_buf = None
        self.worker_out = None
        self.from_worker = 0
        self.inline = 0
        # scan_wait() calls that found the worker on that buffer.
        self.waits_hit = 0
        if flavour != "s3":
            self.freq_range = lambda: ((2205_000_000, 2900_000_000) if flavour == "s31"
                                       else (100_000_000, 6_000_000_000))

    # ---- radio ----
    def init(self):
        self.calls.append(("init",))

    def tune(self, hz, *args, offset=True):
        if args and self.flavour != "s3":
            raise TypeError("extra positional arguments given")
        assert (args[0] if args else offset) is False
        mhz = int(round(hz / 1e6))
        self.calls.append(("tune", mhz))
        self.clock[0] += 4
        if mhz in self.refuse:
            self.refuse.discard(mhz)
            raise OSError(errno.ERANGE)
        lo, hi = self.lock
        if self.flavour == "s31" and not lo <= mhz <= hi:
            raise OSError(errno.ERANGE)  # refused before moving
        self.lo = mhz * 1_000_000
        if self.flavour == "s3" and not lo <= mhz <= hi:
            raise OSError(errno.ERANGE)  # after moving
        if self.flavour == "c6" and not lo <= mhz <= hi:
            # The C6 tells, the P4 tunes it back (esdr_radio.c).
            self.lo = self.took
            raise OSError(errno.ERANGE)
        self.took = self.lo
        return float(np.float32(hz))

    def radio_state(self):
        assert self.flavour != "s3"
        return {"freq": self.lo, "locked": None if self.flavour == "c6old" else True}

    def _int(self, v, name):
        if self.flavour != "s3" and not isinstance(v, int):
            raise TypeError("can't convert float to int: " + name)

    def channel_bw(self, mode):
        self.calls.append(("cbw", mode))

    def rx_bandwidth(self, hz):
        self._int(hz, "bandwidth")
        self.calls.append(("bw", hz))

    def rx_gain(self, g=None):
        if g is not None:
            assert not self.busy
            self.gain = g
            self.calls.append(("gain", g))
        return self.gain

    def rx_gain_max(self):
        return 79 if self.flavour != "s3" else 82

    def rx_start(self, rate, buf, count, offset=True):
        self._int(rate, "rate")
        assert offset is False and rate == FS
        assert not self.busy
        assert buf is not self.worker_buf, "a capture into the buffer the worker reads"
        n = count
        t = np.arange(n) / FS
        z = self.rng.normal(0, self.noise, n) + 1j * self.rng.normal(0, self.noise, n)
        for hz, db in self.sky:
            off = hz - self.lo
            if abs(off) < FS / 2:
                z += 16384 * 10 ** (db / 20) * np.exp(2j * np.pi * off * t)
        x = np.frombuffer(buf, np.int16, 2 * n).reshape(-1, 2)
        x[:, 0] = np.clip(np.round(z.real), -16384, 16383)
        x[:, 1] = np.clip(np.round(z.imag), -16384, 16383)
        self.count = n
        self.clock[0] += 2
        # Each run counts from 0, as on every port.
        self.stats = {"gaps": 0, "resyncs": 0}
        mhz = self.lo // 1_000_000
        if mhz in self.lossy:
            self.lossy.discard(mhz)
            self.stats["gaps"] = 1
        self.busy = mhz in self.stuck
        self.calls.append(("start", self.lo // 1_000_000))

    def rx_busy(self):
        return self.busy

    def rx_wait(self, ms=-1):
        # A capture that never ends (stuck) takes the whole wait.
        if self.busy and ms >= 0:
            self.clock[0] += ms
        return not self.busy

    def rx_stop(self):
        self.busy = False
        self.calls.append(("stop",))

    def rx_stats(self):
        return dict(self.stats)

    def rx_count(self):
        return self.count

    # ---- the scan's C ----
    def _spectrum(self, src, frames):
        mean = (ctypes.c_float * 256)()
        peak = (ctypes.c_float * 256)()
        clipped = ctypes.c_uint32(0)
        got = self.lib.esdr_scan_spectrum(self.scan, addr(src), len(src) // 4, frames, mean,
                                          peak, ctypes.byref(clipped))
        assert got > 0
        return mean, peak, clipped

    def scan_start(self, src, frames=0):
        if not self.workers:
            # One CPU (scan_start() False): scan_hop() makes the spectrum.
            return False
        # The second CPU's: it costs the app's clock nothing (the next hop tunes meanwhile).
        self.worker_buf = src
        self.worker_out = (src, frames, self._spectrum(src, frames))
        return True

    def scan_wait(self, src=None):
        if self.worker_buf is not None and (src is None or src is self.worker_buf):
            self.worker_buf = None
            self.waits_hit += 1

    def scan_hop(self, src, segs, recent, maxh, avg, floor, alpha, rise=1.0, corr=None, frames=0,
                 clip_max=-1):
        out, self.worker_out, self.worker_buf = self.worker_out, None, None
        if out is not None and out[0] is src and out[1] == frames:
            mean, peak, clipped = out[2]
            self.from_worker += 1
        else:
            # The S3's 16-bit FFT took 2.1 ms a hop, the int32 one 9.
            self.clock[0] += 2 if self.flavour == "s3" and self.lib.kind == "int16" else 9
            mean, peak, clipped = self._spectrum(src, frames)
            self.inline += 1
        if clip_max >= 0 and clipped.value > clip_max:
            alpha = 0.0
        pmax, fmean = ctypes.c_float(0), ctypes.c_float(0)
        ret = self.lib.esdr_scan_fold(mean, peak, addr(corr) if corr is not None else None,
                                      addr(segs), len(segs) // 3, addr(recent), addr(maxh),
                                      addr(avg), addr(floor), len(recent), ctypes.c_float(alpha),
                                      ctypes.c_float(rise), ctypes.byref(pmax),
                                      ctypes.byref(fmean))
        assert ret == 0
        return clipped.value, pmax.value, fmean.value

    def scan_q8(self, src, dst):
        self.lib.esdr_scan_q8(addr(src), addr(dst), len(src))

    def stretch(self, src, dst):
        s = np.frombuffer(src, np.float32) if not isinstance(src, memoryview) else \
            np.frombuffer(src.tobytes(), np.float32)
        n, w = len(s), len(dst)
        for c in range(w):
            a, b = c * n // w, max((c + 1) * n // w, c * n // w + 1)
            dst[c] = float(s[a:b].max())

    def claim(self, vol):
        self.claimed.append(("claim", vol))

    def release(self, vol):
        self.claimed.append(("release", vol))


@pytest.fixture
def sv():
    sys.path.insert(0, str(EGG))
    try:
        import scan_view
        importlib.reload(scan_view)
        yield scan_view
    finally:
        sys.path.remove(str(EGG))
        sys.modules.pop("scan_view", None)


@pytest.fixture
def app(monkeypatch, scan_clib, tmp_path):
    if scan_clib is None:
        pytest.skip("no C compiler")

    def make(flavour, sky=(), display=None, sd=True, **kw):
        radio = FakeRadio(scan_clib, flavour, sky, **kw)
        if display is not None:
            radio.display_size = lambda: display
        saved = {k: v for k, v in sys.modules.items() if k.split(".")[0] in OWN}
        for k in saved:
            del sys.modules[k]
        made.append(saved)
        framebuf = types.ModuleType("framebuf")
        framebuf.RGB565 = 1
        framebuf.FrameBuffer = object
        monkeypatch.setitem(sys.modules, "esdr", radio)
        monkeypatch.setitem(sys.modules, "framebuf", framebuf)
        clock = radio.clock
        clock[0] = 1000
        monkeypatch.setattr(time, "ticks_ms", lambda: clock[0], raising=False)
        monkeypatch.setattr(time, "ticks_add", lambda a, b: a + b, raising=False)
        monkeypatch.setattr(time, "ticks_diff", lambda a, b: a - b, raising=False)
        monkeypatch.setattr(time, "time", lambda: clock[0] / 1000)
        monkeypatch.syspath_prepend(str(EGG))
        monkeypatch.syspath_prepend(str(MODULES))
        gui = importlib.import_module("gnuradio._gui")
        gui._headless = True
        mod = importlib.import_module("scan")
        blocks = importlib.import_module("gnuradio.blocks")
        sdir = tmp_path / "SD"
        (sdir / "scan").mkdir(parents=True, exist_ok=True)
        monkeypatch.setattr(blocks, "_mounted", lambda mnt: sd)
        monkeypatch.setattr(blocks, "board_path", lambda p: p)
        monkeypatch.setattr(mod, "open", lambda p, m: open(str(sdir) + p[len("/SD:"):], m),
                            raising=False)
        return types.SimpleNamespace(radio=radio, gui=gui, mod=mod, clock=clock, sd=sdir)

    made = []
    yield make
    for saved in made:
        for k in [k for k in sys.modules if k.split(".")[0] in OWN]:
            del sys.modules[k]
        sys.modules.update(saved)


def sweep(a, scan, n=1, step=5):
    """GUI passes (step ms apart) until n more sweeps are done."""
    target = scan.sweeps + n
    for _ in range(20000):
        if scan.sweeps >= target:
            return
        a.clock[0] += step
        for fn in a.gui._pollers[:]:
            fn()
    pytest.fail("no sweep")


def run_app(a, monkeypatch, body):
    out = {}

    def run():
        scan = a.gui._sinks[0].scan
        out["v"] = body(scan)
        return 0

    monkeypatch.setattr(a.gui, "run", run)
    a.mod.main()
    return out["v"]


# ---- the plan ----

@pytest.mark.parametrize("radio", ["esp32s3", "esp32c6", "esp32s31"])
@pytest.mark.parametrize("span", [(1840, 2780), (2200, 2870), (1795, 2845), (2400, 2430),
                                  (2400, 2415)])
def test_every_bin_has_one_owner_in_both_plans(sv, radio, span):
    r = sv.RADIOS[radio]
    geo = sv.Geometry(span[0], span[1], r["u"], r["d"])
    assert geo.gb >= 6 and geo.db + geo.gb <= geo.ub or geo.gb == 6
    los = []
    for parity in (0, 1):
        hops = sv.plan(geo, parity)
        own = sv.owners(geo, hops)
        assert all(o is not None for o in own)
        for i, x in own:
            assert geo.gb <= abs(x) <= geo.ub, (i, x)
        lo = [h[0] for h in hops]
        assert lo[0] == span[0] and lo[-1] == span[1]
        assert all(v % sv.GRID == 0 for v in lo)
        assert all(sv.GAP_MIN <= b - a <= r["d"] for a, b in zip(lo, lo[1:])) or len(lo) <= 2
        assert all(len(runs) <= 4 for _, runs in hops)
        los.append(lo)
    if span[1] - span[0] >= 4 * r["d"]:
        assert set(los[0][1:-1]).isdisjoint(los[1][1:-1])


def test_the_core_reads_no_farther_than_the_step(sv):
    r = sv.RADIOS["esp32s3"]
    geo = sv.Geometry(1840, 2780, r["u"], r["d"])
    a, b = geo.core()
    edges = (geo.lo_bin(geo.lo_min), geo.lo_bin(geo.lo_max))
    for parity in (0, 1):
        own = sv.owners(geo, sv.plan(geo, parity))
        for j in range(a, b):
            # The guard of an edge LO has no neighbour outside: the inner one, up to ub.
            top = geo.ub if any(abs(j - e) < geo.gb for e in edges) else geo.db
            assert abs(own[j][1]) <= top, (j, own[j])


def test_edges_by_bisection(sv):
    window = (1832, 2787)
    tried = []

    def try_tune(mhz):
        tried.append(mhz)
        return window[0] <= mhz <= window[1]

    assert sv.edges(try_tune, (1700, 2400, 3000)) == (1840, 2780)
    assert len(tried) < 40
    # One refusal in a hundred at random (the S31): a second try settles it.
    flaky = {2385: 1, 2560: 1}

    def try_flaky(mhz):
        if flaky.get(mhz):
            flaky[mhz] -= 1
            return False
        return 2200 <= mhz <= 2875

    assert sv.edges(try_flaky, (2195, 2400, 2905)) == (2205, 2870)
    with pytest.raises(OSError):
        sv.edges(lambda mhz: False, (2195, 2400, 2905))


def test_text_and_lines(sv):
    geo = sv.Geometry(2400, 2500, 18, 15)
    j = geo.lo_bin(2435) + 3
    assert geo.mhz_text(j) == "2435.9375"
    assert geo.hz(j) == 2435_937_500
    assert sv.known(geo, geo.lo_bin(2440)) == "x40"
    assert sv.known(geo, geo.lo_bin(2440) + 2) == ""
    assert sv.known(sv.Geometry(2200, 2800, 24, 20), sv.Geometry(2200, 2800, 24, 20).lo_bin(2500),
                    (2250, 2500)) == "2500M"
    assert sv.clock(90061) == "1970-01-02, 01:01:01"
    line = sv.rtl_line(5, 2_400_000_000, 2_400_625_000, 32, bytes([0, 255, 135]))
    assert line == "1970-01-01, 00:00:05, 2400000000, 2400625000, 312500.00, 32, -127.50, 0.00, -60.00"
    assert sv.levels([-70, -72, -71], [-30]) == (-85, -25)
    assert sv.levels([-70, -72, -71], [-60]) == (-85, -45)
    assert sv.levels([-71, -73, -72], [-31], (-85, -25)) == (-85, -25)


# ---- the app ----

TONE = 2437_187_500


def test_a_tone_lands_and_holds(app, monkeypatch):
    a = app("s3", sky=[(TONE, -30.0)])

    def body(scan):
        sweep(a, scan, 3)
        geo = scan.geo
        j = (TONE - geo.hz(0)) // 312500
        pano = a.gui._sinks[0]
        pano.peak()
        return geo, j, scan.maxh[j], scan.avg[j], scan.floor[j], pano.marker, pano._label(), \
            list(a.radio.calls)

    geo, j, maxh, avg, floor, marker, label, calls = run_app(a, monkeypatch, body)
    assert (geo.lo_min, geo.lo_max) == (1840, 2780)
    assert avg == pytest.approx(-30.0, abs=1.0)
    # The strongest frame held: the tone plus the noise's crest, a little over the tone.
    assert -30.5 < maxh < -28.0
    assert marker == j
    assert label.startswith("2437.1875M a-30 p-")
    # Settings once, then one tune and one capture per hop.
    assert calls.count(("cbw", 2)) == 1 and calls.count(("bw", 69000000)) == 1
    first = next(i for i, c in enumerate(calls) if c[0] == "start")
    hops = [c for c in calls if c[0] == "start"]
    tunes = [c for c in calls[first - 1:] if c[0] == "tune"]
    assert len(tunes) == len(hops)
    assert all(calls[i - 1][0] == "tune" for i, c in enumerate(calls) if c[0] == "start")
    assert len([c for c in calls if c[0] == "gain"]) == 1


def test_the_floor_and_average_stay_at_the_noise(app, monkeypatch):
    a = app("s3")

    def body(scan):
        sweep(a, scan, 4)
        a_, b_ = scan.geo.core()
        avg = [scan.avg[j] for j in range(a_, b_)]
        floor = [scan.floor[j] for j in range(a_, b_)]
        return avg, floor

    avg, floor = run_app(a, monkeypatch, body)
    # Noise rms 300 a component: per bin (BH, 256) some -63 dBFS; flat across hops and guards.
    assert max(avg) - min(avg) < 3.0
    assert np.median(avg) == pytest.approx(np.median(floor) + 1.5, abs=2.0)
    assert min(avg) > -75


@pytest.mark.parametrize("flavour", ["c6", "c6old", "s31"])
def test_the_link_flavours_take_ints(app, monkeypatch, flavour):
    a = app(flavour, sky=[(TONE, -40.0)])

    def body(scan):
        sweep(a, scan, 2)
        j = (TONE - scan.geo.hz(0)) // 312500
        return scan.geo, scan.edges_from, scan.avg[j], scan.maxh[j]

    geo, src, avg, maxh = run_app(a, monkeypatch, body)
    if flavour == "c6":
        # Bisected where the C6 reports its lock: the table's range, a grid step in.
        assert (geo.lo_min, geo.lo_max, src) == (1795, 2845, "bisect")
        assert a.radio.lo // 1_000_000 in range(1795, 2846)
    elif flavour == "c6old":
        assert (geo.lo_min, geo.lo_max, src) == (1795, 2845, "table")
    else:
        assert (geo.lo_min, geo.lo_max, src) == (2205, 2870, "bisect")
    assert avg == pytest.approx(-40.0, abs=1.0)
    assert maxh >= avg


def test_a_refused_and_a_stuck_hop(app, monkeypatch):
    a = app("c6")

    def body(scan):
        sweep(a, scan, 1)
        lo_a = scan.plans[scan.parity][3][0]
        lo_b = scan.plans[scan.parity][5][0]
        a.radio.refuse |= {lo_a}
        a.radio.stuck |= {lo_b}
        failed0 = scan.failed
        sweep(a, scan, 1)
        return scan.failed - failed0, list(a.radio.calls)

    failed, calls = run_app(a, monkeypatch, body)
    # The refusal is retried once (and passes); the stuck capture times out and stops.
    assert failed == 1
    assert ("stop",) in calls


def test_a_clipped_hop_fills_only_the_history(app, monkeypatch):
    a = app("s3", sky=[(2600_000_000, 0.0)])

    def body(scan):
        sweep(a, scan, 2)
        j = (2600_000_000 - scan.geo.hz(0)) // 312500
        return scan.clipped, scan.maxh[j], scan.last_row[j] if scan.rows else None

    clipped, maxh, row = run_app(a, monkeypatch, body)
    assert clipped >= 2
    assert maxh == -200.0


def test_a_gain_change_starts_anew(app, monkeypatch):
    a = app("s3", sky=[(TONE, -30.0)])

    def body(scan):
        sweep(a, scan, 2)
        bars = [v for v in a.gui._vars if type(v).__name__ == "QToolBar"]
        boxes = [w for b in bars for w in b._widgets if type(w).__name__ == "QComboBox"]
        j = (TONE - scan.geo.hz(0)) // 312500
        before = scan.maxh[j]
        boxes[0]._picked(3)  # gain 24
        a.clock[0] += 5
        for fn in a.gui._pollers[:]:
            fn()
        # The holds emptied, the sweep started over (a few hops at the bottom since).
        seen = (scan.gain, before, scan.maxh[j], scan.parity, scan.hop)
        sweep(a, scan, 1)
        return seen, scan.avg[j], a.radio.gain

    (gain, before, held, parity, hop), after, radio_gain = run_app(a, monkeypatch, body)
    assert gain == radio_gain == 24
    assert before > -35 and held == -200.0
    assert parity == 0 and hop <= 4
    assert after == pytest.approx(-30.0, abs=1.0)


def test_save_writes_rtl_power_lines(app, monkeypatch):
    a = app("s3", sky=[(TONE, -30.0)])

    def body(scan):
        for _ in range(3):
            sweep(a, scan, 1)
            a.clock[0] += 1000
        return scan.save(), scan.geo.n

    path, n = run_app(a, monkeypatch, body)
    assert path == "/SD:/scan/scan-000.csv"
    assert a.radio.claimed == [("claim", "/SD:"), ("release", "/SD:")]
    lines = (a.sd / "scan/scan-000.csv").read_text().splitlines()
    assert lines
    times = []
    for line in lines:
        f = [x.strip() for x in line.split(",")]
        assert f[0].startswith("1970-01-01")
        low, high, step, samples = int(f[2]), int(f[3]), float(f[4]), int(f[5])
        assert step == 312500.0 and samples % 32 == 0
        assert (high - low) // 312500 + 1 == len(f) - 6 == n
        times.append(f[1])
    assert len(set(times)) == len(times)
    summ = (a.sd / "scan/scan-000-sum.csv").read_text().splitlines()
    assert summ[0].startswith("# radio=esp32s3 gain=48")
    assert summ[1] == "hz,avg_db,peak_db,floor_db,flags"
    assert len(summ) == 2 + n


def test_a_short_display_gets_tabs(app, monkeypatch):
    seen = {}
    for size in ((428, 142), (320, 240)):
        a = app("s3", display=size)

        def body(scan):
            return a.gui.root

        seen[size] = run_app(a, monkeypatch, body)
    assert seen[(428, 142)] is not None
    assert seen[(320, 240)] is None


def test_no_sd_no_save_button(app, monkeypatch):
    a = app("s3", sd=False)

    def body(scan):
        bars = [v for v in a.gui._vars if type(v).__name__ == "QToolBar"]
        return [w.text() if hasattr(w, "text") else None for b in bars for w in b._widgets
                if type(w).__name__ == "QPushButton"]

    assert "Save" not in run_app(a, monkeypatch, body)


def test_headless_prints_a_summary(app, monkeypatch, capsys):
    a = app("s3", sky=[(TONE, -30.0)])

    def body(scan):
        sweep(a, scan, 2)
        a.clock[0] += 10000
        for fn in a.gui._pollers[:]:
            fn()

    run_app(a, monkeypatch, body)
    out = [s for s in capsys.readouterr().out.splitlines() if s.startswith("scan ")]
    assert out and "1840-2780 MHz bisect g48" in out[-1]
    assert " | 2437.1875M -29" in out[-1] or " | 2437.1875M -30" in out[-1]


def test_the_radio_without_a_plan(app, monkeypatch):
    a = app("s3")
    a.radio.RADIO = "esp32"
    monkeypatch.setattr(a.gui, "run", lambda: pytest.fail("a GUI without a plan"))
    with pytest.raises(OSError):
        a.mod.main()


class FakeBar:
    """The marker's lv.slider as a knob drives it: a value in its range, an event only when the
    value changes."""

    def __init__(self, pano, w):
        self.pano = pano
        self.top = w + 1
        self.value = 0
        pano._w = w
        pano._bar = self
        pano._canvas = object()

    def set_value(self, v, anim):
        self.value = v

    def turn(self, steps):
        for _ in range(abs(steps)):
            v = max(0, min(self.top, self.value + (1 if steps > 0 else -1)))
            if v != self.value:
                self.value = v
                self.pano._moved(v)


@pytest.mark.parametrize("span", [1, 2])
def test_the_knob_moves_the_marker_its_way_in_a_zoomed_view(app, monkeypatch, span):
    """Span 100 or 25 MHz: each detent moves the marker the way the knob turns, also after the
    view pans at an end, and the view only moves when the marker pushes past it."""
    a = app("s3")

    def body(scan):
        sweep(a, scan, 1)
        pano = a.gui._sinks[0]
        bar = FakeBar(pano, 320)
        pano.set_span(span)
        view0 = pano.view()
        assert view0[0] <= pano.marker < view0[1]
        moves = []
        last = pano.marker
        # Right to the view's end and beyond (panning), then back left, then left past the
        # start of the panorama.
        for steps in [1] * 200 + [-1] * 400 + [-1] * 2000:
            before_view = pano.view()
            bar.turn(steps)
            m = pano.marker
            if pano.view() == before_view:
                # Within a view the marker never moves against the knob.
                assert (m - last) * steps >= 0, (steps, last, m)
            moves.append((steps, m - last))
            last = m
            a_, b_ = pano.view()
            assert a_ <= m < b_
            # The bar's column holds the marker's bin.
            lo, hi = pano._col_bins(bar.value - 1, 320)
            assert lo <= m < hi, (bar.value, lo, hi, m)
        return view0, pano.view(), pano.marker, scan.geo.n

    view0, view1, marker, n = run_app(a, monkeypatch, body)
    # Panned right past the first view, then back left past it (at 100 MHz, a bin a detent, to
    # the panorama's start; at 25 MHz four detents a bin).
    assert view1[0] < view0[0]
    if span == 1:
        assert view1[0] == 0 and marker == 0


def test_a_hop_that_lost_a_block_fails_alone(app, monkeypatch):
    a = app("c6")

    def body(scan):
        sweep(a, scan, 1)
        lo = scan.plans[scan.parity][4][0]
        a.radio.lossy |= {lo}
        failed0 = scan.failed
        sweep(a, scan, 1)
        p = scan.parity ^ 1
        return scan.failed - failed0, [k for k, f in enumerate(scan.flags[p]) if f == 2]

    failed, bad = run_app(a, monkeypatch, body)
    # Only that hop, not the one after it (each run's counters start at 0).
    assert failed == 1 and bad == [4]


@pytest.mark.parametrize("flavour", ["s3", "c6"])
def test_the_worker_makes_each_spectrum(app, monkeypatch, flavour):
    # Every good hop's spectrum from the second CPU (scan_start() as its capture ends), folded
    # after the next hop started; no capture into the buffer the worker reads (FakeRadio
    # asserts it), also across a gain change, a refused and a lossy hop.
    a = app(flavour, sky=[(TONE, -40.0)])

    def body(scan):
        sweep(a, scan, 1)
        a.radio.refuse |= {scan.plans[scan.parity][3][0]}
        a.radio.lossy |= {scan.plans[scan.parity][5][0]}
        # The first hop after the gain change fails while the worker still holds the buffer of
        # the fold the change dropped: the next capture into it waits (scan_wait()).
        a.radio.lossy |= {scan.plans[0][0][0]}
        scan.set_gain(32)
        sweep(a, scan, 2)
        j = (TONE - scan.geo.hz(0)) // 312500
        return a.radio.from_worker, a.radio.inline, a.radio.waits_hit, scan.avg[j]

    from_worker, inline, waits_hit, avg = run_app(a, monkeypatch, body)
    assert from_worker > 100 and inline == 0 and waits_hit >= 1
    assert avg == pytest.approx(-40.0, abs=1.0)


def test_one_cpu_makes_each_spectrum_in_the_fold(app, monkeypatch):
    # Without a worker (scan_start() False: the classic ESP32, a board without SMP) scan_hop()
    # makes each spectrum, the same panorama.
    a = app("s3", sky=[(TONE, -40.0)], workers=False)

    def body(scan):
        sweep(a, scan, 2)
        j = (TONE - scan.geo.hz(0)) // 312500
        return a.radio.from_worker, a.radio.inline, scan.avg[j]

    from_worker, inline, avg = run_app(a, monkeypatch, body)
    assert from_worker == 0 and inline > 100
    assert avg == pytest.approx(-40.0, abs=1.0)


def test_a_sweeps_last_hop_reaches_its_history_row(app, monkeypatch):
    a = app("s3")

    def body(scan):
        while scan.rows == 0:
            sweep(a, scan, 1)
            a.clock[0] += 500
        n = scan.geo.n
        row = bytes(memoryview(scan.ring)[0:n])
        return row[n - 1], row[0], scan.levels

    top, bottom, levels = run_app(a, monkeypatch, body)
    # The top flank comes from the plan's last hop (lo_max): in the row, not empty.
    assert top != 0 and bottom != 0
    assert levels is not None

