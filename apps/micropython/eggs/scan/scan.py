# SPDX-License-Identifier: GPL-3.0-or-later
"""Scan: a survey of everything the board's radio tunes, as a panorama.

The radio hops its LO over the whole range its PLL locks in (found by bisection where tune()
refuses an LO out of lock, else from a table of a measured chip: a C6 firmware that does not report
the lock), an 80 MS/s burst at each LO, its spectrum in 312.5 kHz bins (esdr.scan_hop(): 32 frames
of 256, Blackman-Harris, each frame's DC out; on a second CPU, esdr.scan_start(), while the next
hop tunes). The hops' bins go into the panorama (scan_view.py): the nearest hop's,
except near its LO, two plans taking turns. The panorama shows, in dB of a full-scale tone in a bin:

  avg     orange: the power averaged over the sweeps
  peak    purple: the strongest frame (3.2 us) held since the start or Reset
  floor   gray: the quietest readings, rising 1 dB a sweep at most (bursts and what follows the
          LO stay out of it)

and below it a history of the sweeps (a row a second at most, the strongest of its sweeps). The
bar under the panorama is the marker (the knob: focus it, press, turn; a finger: drag), a column
of the plot a step; in a zoomed span a step past either end pans the view a quarter span that
way (the view stays put otherwise); its label reads the frequency and the three levels there,
x40 at a crystal harmonic (and the board's own known lines), flank in the outer bins only one hop
reads. Ticks under the plot: the LOs of
the plan swept, the one being measured lit; marks over it: hops that clipped or failed.

  Gain    the receive gain index (fixed: the AGC would move every hop's level); a change starts
          the holds anew
  Span    the whole range, or 100 or 25 MHz around the marker (the sweep covers the whole range)
  Peak    the marker to the strongest peak in view, again to the next
  Reset   the holds, the floor and the history anew
  Pause   no new hop: everything holds still
  Save    the history as rtl_power CSV (/SD:/scan/scan-NNN.csv, uptime as 1970 dates) and the
          panorama's levels per bin (scan-NNN-sum.csv)

On a short display (the Communicator's) the panorama, the history and the controls have a tab
each. Without a display a summary line every 10 s."""

import array
import time

import dusk
import esdr
from gnuradio import _gui, blocks, qtgui
from PyQt5 import Qt
from PyQt5._nothing import Nothing

import scan_view as sv

# Samples a hop (32 frames: 0.8 dB of scatter a bin, the sweeps' average below that) and its
# rate. A burst takes 16380 at most; the frames past 32 cost more FFT (some 0.2 ms each) and, on
# the P4, link time than they add.
COUNT = 8192
RATE = 80000000
# The weight of a sweep in the average, the floor's rise a sweep (dB).
ALPHA = 0.25
RISE = 1.0
# A pass of the GUI loop starts hops for this long at most (ms), and no more than HOPS: the
# knob and the panel first.
SCAN_MS = 30
HOPS = 4
# Clipped samples a hop may have before it counts as clipped (0.2 % of 8192).
CLIP_MAX = 16
# History rows kept (and saved), a row a second at most.
ROWS = 64
ROW_MS = 1000
GAIN = 48
SPANS = (("Full", 0), ("100M", 100), ("25M", 25))
HEADLESS_MS = 10000
# The LO being measured; a hop that clipped or failed.
ACCENT = dusk.rgb565(dusk.ACCENT)
WARN = dusk.rgb565(dusk.WARN)


def _floats(n, v=sv.EMPTY):
    return array.array("f", [v] * n)


class Scan:
    """The sweep: plans, the hop in flight, the panorama's arrays, the history ring."""

    def __init__(self, radio):
        self.r = sv.RADIOS[radio]
        self.radio = radio
        self.gain = GAIN
        self.edges_from = "table"
        self.geo = None
        self.plans = None
        self.corr = None
        # Two capture buffers: a hop done is folded while the next one fills the other.
        self.buf = (bytearray(4 * COUNT), bytearray(4 * COUNT))
        self.next_buf = 0
        self.parity = 0
        self.hop = 0
        # The hop in flight: (plan, hop, buffer, deadline, stats before) or None; the one done
        # and not yet folded likewise.
        self.inflight = None
        self.done = None
        self.flags = None
        self.recent = self.maxh = self.avg = self.floor = None
        # All empty (slice copies empty an array in C), and the last history row's levels.
        self.empty = None
        self.last_row = None
        self.ring = None
        self.ring_t = array.array("I", [0] * ROWS)
        self.ring_n = array.array("H", [0] * ROWS)
        self.rows = 0
        self.row_t0 = time.ticks_ms()
        self.row_sweeps = 0
        self.sweeps = 0
        self.sweep_t0 = time.ticks_ms()
        self.sweep_ms = 0
        self.hop_floors = []
        self.hop_peaks = []
        self.levels = None
        self.clipped = 0
        self.failed = 0
        self.paused = False
        self.pending_gain = None
        self.version = 0
        self.resets = 0
        self.new_row = False
        # Seconds since boot for the history's times (time.time() on the boards: no wrap, as the
        # ticks have after 6 days).
        self.t_start = time.time()
        # Whether the hop waiting for its fold ended a sweep.
        self.done_ends = False
        # Wait for a capture inside poll(); not from a scheduled callback (headless: a Ctrl-C
        # raised in the wait would be lost there).
        self.wait_capture = True

    # ---- start ----

    def _try(self, mhz):
        try:
            esdr.tune(mhz * 1000000, offset=False)
            return True
        except (OSError, ValueError):
            return False

    def _bisect(self):
        """Whether tune() refuses an LO out of lock: a radio on the chip does, one behind a link
        where it reports the lock (radio_state()["locked"] not None)."""
        r = self.r
        if r["edges"] is None:
            return False
        if r["table"] is None:
            return True
        state = getattr(esdr, "radio_state", None)
        return state is not None and state().get("locked") is not None

    def setup(self):
        r = self.r
        if r["cbw"] is not None:
            esdr.channel_bw(r["cbw"])
        esdr.rx_bandwidth(r["bw"])
        esdr.rx_gain(self.gain)
        if self._bisect():
            lo, hi = sv.edges(self._try, r["edges"])
            self.edges_from = "bisect"
        else:
            lo, hi = r["table"]
            self.edges_from = "table"
        self.geo = sv.Geometry(lo, hi, r["u"], r["d"])
        self.plans = tuple(self._segs(sv.plan(self.geo, p)) for p in (0, 1))
        flat = sv.correction(r["flat"])
        self.corr = array.array("f", flat) if flat else None
        n = self.geo.n
        self.recent = _floats(n)
        self.maxh = _floats(n)
        self.avg = _floats(n)
        self.floor = _floats(n)
        self.empty = _floats(n)
        self.last_row = _floats(n)
        self.ring = bytearray(ROWS * n)
        self.flags = (bytearray(len(self.plans[0])), bytearray(len(self.plans[1])))

    @staticmethod
    def _segs(hops):
        out = []
        for lo, runs in hops:
            a = array.array("H")
            for t in runs:
                a.extend(t)
            out.append((lo, a))
        return out

    # ---- the sweep ----

    def reset(self):
        for arr in (self.recent, self.maxh, self.avg, self.floor):
            arr[:] = self.empty
        for row in range(ROWS):
            self.ring_n[row] = 0
        self.rows = 0
        self.row_sweeps = 0
        self.row_t0 = time.ticks_ms()
        self.levels = None
        self.clipped = 0
        self.failed = 0
        self.version += 1
        self.resets += 1

    def set_gain(self, g):
        self.pending_gain = g

    def _start_next(self):
        """Tune and start the next hop's capture (the radio idle). False: it failed."""
        p, k = self.parity, self.hop
        lo = self.plans[p][k][0]
        buf = self.buf[self.next_buf]
        for attempt in range(2):
            try:
                esdr.tune(lo * 1000000, offset=False)
                # The worker may still read it (a hop that ended as it started).
                esdr.scan_wait(buf)
                esdr.rx_start(RATE, buf, COUNT, offset=False)
                self.inflight = (p, k, buf, time.ticks_add(time.ticks_ms(), self.r["deadline"]))
                self.next_buf ^= 1
                return True
            except OSError:
                pass
        return False

    @staticmethod
    def _bad():
        """The capture lost blocks (the link's gaps or resyncs: each run counts from 0) or stopped
        short."""
        st = esdr.rx_stats()
        return st.get("gaps", 0) != 0 or st.get("resyncs", 0) != 0 or esdr.rx_count() < COUNT

    def _fold(self, done):
        p, k, buf = done[0], done[1], done[2]
        segs = self.plans[p][k][1]
        clipped, peak, floor = esdr.scan_hop(buf, segs, self.recent, self.maxh, self.avg,
                                             self.floor, ALPHA, RISE, self.corr,
                                             clip_max=CLIP_MAX)
        clip = clipped > CLIP_MAX
        if clip:
            # Clipping spreads over the whole hop: only the history row takes it.
            self.clipped += 1
            self.flags[p][k] = 1
        else:
            self.flags[p][k] = 0
            self.hop_floors.append(floor)
            self.hop_peaks.append(peak)
        self.version += 1

    def _fail(self, p, k):
        self.flags[p][k] = 2
        self.failed += 1

    def _advance(self):
        """On to the next hop; at the end of a plan the other plan next. True: a sweep ended (its
        bookkeeping, _end_sweep(), after its last hop's fold)."""
        self.hop += 1
        if self.hop < len(self.plans[self.parity]):
            return False
        self.parity ^= 1
        self.hop = 0
        return True

    def _end_sweep(self):
        now = time.ticks_ms()
        self.sweep_ms = time.ticks_diff(now, self.sweep_t0)
        self.sweep_t0 = now
        self.sweeps += 1
        self.row_sweeps += 1
        # The top from the peaks held (they only grow until Reset): a level that stays put.
        self.levels = sv.levels(self.hop_floors, (max(self.maxh),), self.levels)
        self.hop_floors = []
        self.hop_peaks = []
        if time.ticks_diff(now, self.row_t0) >= ROW_MS:
            self._close_row(now)

    def _close_row(self, now):
        row = self.rows % ROWS
        n = self.geo.n
        esdr.scan_q8(self.recent, memoryview(self.ring)[row * n:(row + 1) * n])
        self.ring_t[row] = int(time.time() - self.t_start)
        self.ring_n[row] = self.row_sweeps
        self.last_row[:] = self.recent
        self.recent[:] = self.empty
        self.rows += 1
        self.row_sweeps = 0
        self.row_t0 = now
        self.new_row = True

    def take_row(self):
        """The levels of the history row just closed, or None."""
        if not self.new_row:
            return None
        self.new_row = False
        return self.last_row

    def clear_recent(self):
        self.recent[:] = self.empty

    def poll(self):
        """Hops for SCAN_MS at most: a hop done is folded while the next one captures."""
        t0 = time.ticks_ms()
        for _ in range(HOPS):
            if self.inflight is not None:
                p, k, buf, deadline = self.inflight
                # The capture ends within milliseconds; with the spectrum on the second CPU
                # the fold no longer covers it: waited for here, within this pass's time.
                left = min(time.ticks_diff(deadline, time.ticks_ms()),
                           SCAN_MS - time.ticks_diff(time.ticks_ms(), t0))
                if self.wait_capture and left > 0 and esdr.rx_busy():
                    esdr.rx_wait(left)
                if esdr.rx_busy():
                    if time.ticks_diff(time.ticks_ms(), deadline) < 0:
                        break
                    esdr.rx_stop()
                    self.inflight = None
                    self._fail(p, k)
                    if self._advance():
                        self._end_sweep()
                else:
                    self.inflight = None
                    if self._bad():
                        self._fail(p, k)
                        if self._advance():
                            self._end_sweep()
                    else:
                        # Its spectrum on the second CPU while this one tunes on.
                        esdr.scan_start(buf)
                        self.done = (p, k, buf)
                        self.done_ends = self._advance()
            if self.pending_gain is not None and self.inflight is None:
                try:
                    esdr.rx_gain(self.pending_gain)
                except OSError:
                    # The link timed out (P4) or a stopped capture winds down (S3): the next
                    # pass tries again.
                    break
                self.gain = self.pending_gain
                self.pending_gain = None
                self.done = None
                self.done_ends = False
                self.parity = 0
                self.hop = 0
                self.hop_floors = []
                self.hop_peaks = []
                self.reset()
                self.clear_recent()
            started = False
            if not self.paused and self.inflight is None:
                started = self._start_next()
                if not started:
                    self._fail(self.parity, self.hop)
                    if self._advance():
                        self._end_sweep()
            if self.done is not None:
                done, self.done = self.done, None
                self._fold(done)
                if self.done_ends:
                    self.done_ends = False
                    self._end_sweep()
            if self.paused or time.ticks_diff(time.ticks_ms(), t0) >= SCAN_MS:
                break
            if not started and self.inflight is None:
                break

    def stop(self):
        if self.inflight is not None or esdr.rx_busy():
            esdr.rx_stop()
            self.inflight = None
        esdr.scan_wait()

    def lo_now(self):
        if self.inflight is None:
            return None
        return self.plans[self.inflight[0]][self.inflight[1]][0]

    # ---- output ----

    def summary(self):
        geo = self.geo
        floors = sorted(v for v in self.floor if v > sv.EMPTY)
        med = floors[len(floors) // 2] if floors else sv.EMPTY
        taken = []
        for _ in range(5):
            j = sv.next_peak(self.maxh, 0, geo.n, taken)
            if j is None:
                break
            taken.append(j)
        peaks = " ".join("{}M {:.0f}{}".format(geo.mhz_text(j), self.maxh[j],
                                                (" " + sv.known(geo, j, self.r["lines"]))
                                                if sv.known(geo, j, self.r["lines"]) else "")
                         for j in taken)
        return "scan {}-{} MHz {} g{} sweeps {} {:.2f}s floor {:.0f} | {}".format(
            geo.lo_min, geo.lo_max, self.edges_from, self.gain, self.sweeps,
            self.sweep_ms / 1000, med, peaks)

    def save(self):
        """The history as rtl_power lines and the panorama per bin, on the SD card; the path."""
        import os

        path = None
        for k in range(1000):
            p = "/SD:/scan/scan-{:03d}.csv".format(k)
            try:
                os.stat(p)
            except OSError:
                path = p
                break
        path = blocks.board_path(path or "/SD:/scan/scan-999.csv")
        vol = blocks._volume_of(path)
        was = self.paused
        self.paused = True
        self.stop()
        esdr.claim(vol)
        try:
            blocks._makedirs(path)
            geo = self.geo
            n = geo.n
            low, high = geo.hz(0), geo.hz(n - 1)
            frames = COUNT // sv.N
            with open(path, "w") as f:
                first = max(0, self.rows - ROWS)
                for r in range(first, self.rows):
                    row = r % ROWS
                    line = sv.rtl_line(self.ring_t[row], low, high, frames * self.ring_n[row],
                                       memoryview(self.ring)[row * n:(row + 1) * n])
                    blocks._write(f, line + "\n", path)
            with open(path[:-4] + "-sum.csv", "w") as f:
                r = self.r
                head = ("# radio={} gain={} rate={} nfft={} window=blackman-harris enbw_bins=2.0"
                        " u_mhz={} d_mhz={} guard_bins={} lo_mhz={}-{} edges={} flat={}"
                        " sweeps={} uptime_s={} units=dB of a full-scale tone per 312.5 kHz bin\n"
                        "hz,avg_db,peak_db,floor_db,flags\n").format(
                    self.radio, self.gain, RATE, sv.N, r["u"], r["d"], geo.gb, geo.lo_min,
                    geo.lo_max, self.edges_from, "measured" if r["flat"] else "none",
                    self.sweeps, int(time.time() - self.t_start))
                blocks._write(f, head, path)
                for j in range(n):
                    blocks._write(f, "{},{:.1f},{:.1f},{:.1f},{}\n".format(
                        geo.hz(j), self.avg[j], self.maxh[j], self.floor[j],
                        sv.known(geo, j, r["lines"])), path)
        finally:
            esdr.release(vol)
            self.paused = was
        return path


def _span_bins(span_mhz, n):
    """Bins a span of span_mhz shows (0: the whole panorama of n)."""
    return n if not span_mhz else min(n, span_mhz * 16 // 5)


class _Panorama(qtgui._sink):
    _draws = True
    BAR = 10

    def __init__(self, scan):
        super().__init__("scan", "c", "Scan")
        self.scan = scan
        self._update = 0.2
        self.span = 0
        self.marker = scan.geo.n // 2
        # The view's first bin: it stays while the marker moves in it (a view that followed the
        # marker moved under the bar, whose next step then counted from the new view).
        self.view_a = 0
        self.taken = []
        self.note = ""
        self._bar = None
        self._cols = None
        self._shown = None
        self._slider_cols = 0

    def _need(self):
        return (0, 2)

    def _bar_h(self):
        return 24 if _gui.TOUCH else self.BAR

    def _make(self, parent, x, y, w, h):
        super()._make(parent, x, y, w, h)
        lv = _gui.lv
        self._cols = array.array("f", [0.0] * w)
        b = lv.slider(parent)
        b.set_size(w - 8, self._bar_h() - 4)
        b.set_pos(x + 4, y + h - self._bar_h() + 2)
        # A step past each end: 1 to w are the columns, 0 and w + 1 pan a zoomed view (a slider
        # at its end sends no event for a push further).
        b.set_range(0, w + 1)
        _gui.on(b, lv.EVENT.VALUE_CHANGED, self._moved, b.get_value, latest=True)
        _gui.add_to_group(b)
        self._bar = b
        self._slider_cols = w
        self._sync_bar()
        self._shown = None

    def view(self):
        """The bins [a, b) the plot shows."""
        n = self.scan.geo.n
        m = _span_bins(self.span, n)
        a = max(0, min(self.view_a, n - m))
        return a, a + m

    def _centre(self):
        m = _span_bins(self.span, self.scan.geo.n)
        self.view_a = self.marker - m // 2
        self.view_a, _ = self.view()

    def _col_bins(self, c, w):
        a, b = self.view()
        n = b - a
        return a + c * n // w, a + max((c + 1) * n // w, c * n // w + 1)

    def _moved(self, v):
        w = self._w
        v = int(v)
        self.taken = []
        self._shown = None
        if v <= 0 or v > w:
            # Pushed past an end: a zoomed view pans a quarter of its span that way, the marker
            # stays on its bin (at the edge if the view left it), the bar goes to its column.
            a, b = self.view()
            step = max(1, (b - a) // 4)
            self.view_a = a - step if v <= 0 else a + step
            a, b = self.view()
            self.marker = max(a, min(b - 1, self.marker))
            self._sync_bar()
            return
        lo, hi = self._col_bins(v - 1, w)
        self.marker = sv.strongest(self.scan.maxh, lo, hi)

    def _sync_bar(self):
        # Another tab is showing (the Communicator): _gui's _clear() deleted the slider and left a
        # Nothing for the canvas; _make() makes a new one and syncs it when the tab comes back.
        if self._bar is None or isinstance(self._canvas, Nothing):
            return
        a, b = self.view()
        col = (self.marker - a) * self._w // (b - a)
        self._bar.set_value(1 + max(0, min(self._w - 1, col)), False)

    def peak(self):
        a, b = self.view()
        j = sv.next_peak(self.scan.maxh, a, b, self.taken)
        if j is None:
            self.taken = []
            j = sv.next_peak(self.scan.maxh, a, b, self.taken)
        if j is not None:
            self.taken.append(j)
            self.marker = j
            self._sync_bar()
        self._shown = None

    def set_span(self, i):
        self.span = SPANS[i][1]
        self._centre()
        self._sync_bar()
        self._shown = None

    def _label(self):
        s = self.scan
        if s.geo is None:
            return "Scan"
        return sv.marker_text(s.geo, self.marker, s.avg, s.maxh, s.floor, s.r["lines"])

    def _draw(self):
        s = self.scan
        key = (s.version, self.marker, self.view(), s.levels, s.lo_now(), self.note)
        if key == self._shown:
            return False
        self._shown = key
        self._head()
        fb = self._fbo
        buf = self._buf
        x, y, w, h = self._plot()
        h -= self._bar_h()
        fb.fill_rect(x, y, w, h, _gui.BG)
        geo = s.geo
        a, b = self.view()
        lo_db, hi_db = s.levels if s.levels is not None else (-100, -20)
        # The flanks (one hop reads them) shaded.
        core_a, core_b = geo.core()
        for fa, fb_ in ((a, min(b, core_a)), (max(a, core_b), b)):
            if fb_ > fa:
                xa = (fa - a) * w // (b - a)
                xb = (fb_ - a) * w // (b - a)
                fb.fill_rect(x + xa, y, max(1, xb - xa), h, _gui.FAINT)
        # Grid: 10 dB steps, and frequency lines.
        step = 10
        level = lo_db - lo_db % step + step
        while level < hi_db:
            fb.hline(x, y + h - 1 - (level - lo_db) * (h - 1) // (hi_db - lo_db), w, _gui.FAINT)
            level += step
        for line in _freq_lines(geo, a, b, w):
            fb.vline(x + line, y, h, _gui.FAINT)
        cols = self._cols
        if s.sweeps:
            for arr, colour in ((s.floor, _gui.TRACE[2]), (s.avg, _gui.TRACE[0]),
                                (s.maxh, _gui.TRACE[1])):
                esdr.stretch(memoryview(arr)[a:b], cols)
                esdr.fb_trace(buf, self._w, x, y, w, h, cols, lo_db, hi_db, colour)
        # The marker.
        if a <= self.marker < b:
            fb.vline(x + (self.marker - a) * w // (b - a), y, h, _gui.TRACE[3])
        # LO ticks of the plan swept, the hop being measured lit; clipped or failed hops marked.
        now = s.lo_now()
        p = s.parity
        for k, (lo, _) in enumerate(s.plans[p]):
            j = geo.lo_bin(lo)
            if a <= j < b:
                xx = x + (j - a) * w // (b - a)
                fb.vline(xx, y + h - 3, 3, ACCENT if lo == now else _gui.MUTED)
                if s.flags[p][k]:
                    fb.hline(max(x, xx - 3), y, 7, WARN)
        # Known lines at the top.
        for line in _known_lines(geo, a, b, w, s.r["lines"]):
            fb.vline(x + line, y, 3, _gui.MUTED)
        # The view's edges, the levels' range.
        fb.text(geo.mhz_text(a).split(".")[0], x + 1, y + h - 12, _gui.MUTED)
        right = geo.mhz_text(b - 1).split(".")[0]
        fb.text(right, x + w - 8 * len(right) - 1, y + h - 12, _gui.MUTED)
        top = "{}dB".format(hi_db)
        fb.text(top, x + w - 8 * len(top) - 1, y + 2, _gui.MUTED)
        fb.text("{}dB".format(lo_db), x + w - 8 * len("{}dB".format(lo_db)) - 1, y + h - 24,
                _gui.MUTED)
        if self.note:
            fb.text(self.note[: w // 8 - 6], x + 1, y + 2, _gui.FG)


def _freq_lines(geo, a, b, w):
    """Columns of 100 MHz lines (25 MHz in a narrow view) over bins [a, b)."""
    span_mhz = (b - a) * 5 // 16
    step = 100 if span_mhz > 300 else (25 if span_mhz > 60 else 5)
    f0 = geo.f16(a)
    f1 = geo.f16(b)
    out = []
    m = (f0 // (16 * step) + 1) * step
    while m * 16 < f1:
        out.append((m * 16 - f0) * w // (f1 - f0))
        m += step
    return out


def _known_lines(geo, a, b, w, lines):
    f0 = geo.f16(a)
    f1 = geo.f16(b)
    out = []
    m = (f0 // (16 * sv.XTAL_MHZ) + 1) * sv.XTAL_MHZ
    while m * 16 < f1:
        out.append((m * 16 - f0) * w // (f1 - f0))
        m += sv.XTAL_MHZ
    for m in lines:
        if f0 <= m * 16 < f1:
            out.append((m * 16 - f0) * w // (f1 - f0))
    return out


class _History(qtgui._sink):
    _draws = True

    def __init__(self, scan, pano):
        super().__init__("history", "c", "History")
        self.scan = scan
        self.pano = pano
        self._update = 0.2
        self._cols = None
        # What the rows drawn were drawn for: (span, resets), and their levels.
        self._view = None
        self._levels = None

    def _make(self, parent, x, y, w, h):
        super()._make(parent, x, y, w, h)
        self._cols = array.array("f", [0.0] * w)
        self._view = None

    def _label(self):
        s = self.scan
        text = "{:.2f}s g{}".format(s.sweep_ms / 1000, s.gain)
        if s.clipped:
            text += " clip{}".format(s.clipped)
        if s.failed:
            text += " fail{}".format(s.failed)
        return text + " " + s.edges_from

    def _draw(self):
        s = self.scan
        row = s.take_row()
        view = (self.pano.view(), s.resets)
        if row is None and view == self._view:
            return False
        self._head()
        x, y, w, h = self._plot()
        if view != self._view:
            # A new span or a reset: the rows drawn before no longer line up. The rows keep the
            # levels of the first one after it.
            self._fbo.fill_rect(x, y, w, h, _gui.BG)
            self._view = view
            self._levels = None
        if row is not None:
            a, b = view[0]
            if self._levels is None and s.levels is not None:
                self._levels = s.levels
            lo_db, hi_db = self._levels if self._levels is not None else (-100, -20)
            esdr.stretch(memoryview(row)[a:b], self._cols)
            esdr.fb_scroll(self._buf, self._w, x, y, w, h)
            esdr.fb_wf_row(self._buf, self._w, x, y, w, 1, self._cols, lo_db, hi_db,
                           _gui.PALETTE)
        return True


def _bar(*parts):
    bar = Qt.QToolBar()
    for label, widget in parts:
        if label:
            bar.addWidget(Qt.QLabel(label))
        bar.addWidget(widget)
    _gui.variable(bar)
    return bar


def _chooser(items, at, slot):
    box = Qt.QComboBox()
    box.setSizeAdjustPolicy(Qt.QComboBox.AdjustToContents)
    for it in items:
        box.addItem(it)
    box.setCurrentIndex(at)
    box.currentIndexChanged.connect(slot)
    return box


# Below this many rows for the plots under the controls (the Communicator's 142-row panel), the
# panorama, the history and the controls get a tab each.
PLOTS_MIN = 120


def _tabbed():
    return _gui.H - _gui.HEADER - 2 * _gui.ROW < PLOTS_MIN


def _gains():
    top = esdr.rx_gain_max()
    out = list(range(0, top + 1, 8))
    if out[-1] != top:
        out.append(top)
    return out


def _gui_up(scan):
    root = page_scan = page_hist = page_setup = None
    if _tabbed():
        root = Qt.QWidget()
        tabs = Qt.QTabWidget()
        Qt.QVBoxLayout(root).addWidget(tabs)
        pages = [Qt.QWidget() for _ in range(3)]
        for page, name in zip(pages, ("Scan", "History", "Setup")):
            tabs.addTab(page, name)
        page_scan, page_hist, page_setup = (Qt.QVBoxLayout(p) for p in pages)
    _gui.reset(root)
    pano = _Panorama(scan)
    hist = _History(scan, pano)
    if page_scan is not None:
        page_scan.addWidget(pano)
        page_hist.addWidget(hist)
    gains = _gains()

    def pick_gain(i):
        scan.set_gain(gains[i])

    def save():
        try:
            path = scan.save()
            pano.note = "saved " + path.rsplit("/", 1)[-1]
        except OSError as e:
            pano.note = "not saved: " + str(e.args[-1] if e.args else "error")
            print("Scan:", e)

    def reset():
        scan.reset()
        scan.clear_recent()

    def pause(on):
        scan.paused = bool(on)

    geo = scan.geo
    _gui.title("Scan {}-{} {}".format(geo.f16(0) // 16, geo.f16(geo.n - 1) // 16,
                                      scan.edges_from))
    at = min(range(len(gains)), key=lambda i: abs(gains[i] - scan.gain))
    peak = Qt.QPushButton("Peak")
    peak.clicked.connect(pano.peak)
    clear = Qt.QPushButton("Reset")
    clear.clicked.connect(reset)
    hold = Qt.QCheckBox("Pause")
    hold.toggled.connect(pause)
    tools = [("", peak), ("", clear), ("", hold)]
    if blocks._mounted("/SD:"):
        button = Qt.QPushButton("Save")
        button.clicked.connect(save)
        tools.append(("", button))
    bars = (_bar(("Gain", _chooser([str(g) for g in gains], at, pick_gain)),
                 ("Span", _chooser([s[0] for s in SPANS], 0, pano.set_span))),
            _bar(*tools))
    if page_setup is not None:
        for bar in bars:
            page_setup.addWidget(bar)
    return pano, hist


def main():
    esdr.init()
    radio = getattr(esdr, "RADIO", "esp32s3")
    if radio not in sv.RADIOS:
        raise OSError("Scan: no plan for the radio " + radio)
    scan = Scan(radio)
    scan.setup()
    headless = _gui.headless()
    scan.wait_capture = not headless
    last = [time.ticks_ms()]

    def poll():
        scan.poll()
        if headless and time.ticks_diff(time.ticks_ms(), last[0]) >= HEADLESS_MS:
            last[0] = time.ticks_ms()
            print(scan.summary())

    _gui_up(scan)
    _gui.poll(poll)
    _gui.show()
    try:
        _gui.run()
    finally:
        scan.stop()
