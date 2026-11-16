# SPDX-License-Identifier: GPL-3.0-or-later
"""Pcap: apps/pcap's packet capture on the board's own radio, its packets in a table.

apps/pcap's channel mode on one channel at 2 MS/s: a power detector cuts bursts, and apps/pcap's
decoders make Bluetooth LE packets, IEEE 802.15.4 frames and, from what neither claimed, records
of the burst's modulation, symbol rate and bits (doc/pcap.md). Where the esdr module has the
engine (esdr.pcap_start(): the S3 in multiapp, esdr.PACKET_RATES, the ring's every 4th pair, two
of them averaged; the S31; the P4), a thread above MicroPython runs the detector where the
samples land (the S3 and the S31: internal RAM, the thread on the other CPU; the P4: a ring in
the heap the link fills, esdr.PCAP_RING_MIN) and a second one the decoders; each pass of the
display loop takes their packets (esdr.pcap_take()). Elsewhere the radio receives into a ring in
the heap (esdr.rx_start(wrap=True)) and each pass detects and decodes what came
(esdr.pcap_feed()). The packets go into the table (pcap_view.py), newest first by default:

  Ch      the channel: Bluetooth LE advertising 37, 38, 39 or 802.15.4 11 to 26
  Show    which packets: decoded ones with a good CRC, all, one protocol, the burst records,
          those with a bad CRC, or those of the selected row's source
  Group   one row per source, type, protocol or channel: count, last time, loudest, bytes
  Sort    newest, oldest, loudest, longest, most packets (per source when not grouped),
          source, type
  Gain    automatic or a fixed receive gain index
  Save    the capture as pcapng on the SD card (/SD:/pcap/esdr-NNN.pcapng), apps/pcap's
          interfaces and records: Wireshark reads it as it reads apps/pcap's stream
  Hold    the table stops changing while the capture goes on

The bar at the table's right edge selects a row (the knob: focus it, press, turn; a finger:
drag); the lines below the table show the selected row in full. The header counts the packets
shown and kept, the packets a second, the capture's share of the CPU and what it lost. On a
short display (the Communicator's) the table and the controls have a tab each."""

import gc
import time

import esdr
from gnuradio import _gui, blocks, qtgui
from PyQt5 import Qt

import pcap_view as pv

# The decoders' rates, the filtered stream's first.
RATES = (2000000, 4000000)
# The ring: 0.5 s at 2 MS/s at most, and no more than this share of the free heap (the packets
# kept need the rest); RING_MIN samples at least. The engine's thread reads it as it fills: 130
# ms (the garbage collector reads every word of it at each collection).
RING_MAX = 1 << 20
RING_ENGINE = 1 << 18
RING_SHARE = 0.45
RING_MIN = 1 << 17
# Samples one pcap_feed() takes at most, and the time a pass of the loop spends feeding or
# decoding (the radio first: the table draws less often when packets come fast).
FEED_SAMPLES = 32768
FEED_MS = 60
# Bursts no decoder claimed that a feed analyses (the rest are recorded unanalysed), while the
# capture is less than BEHIND of the ring behind the radio (else none: the decoders first; the
# engine's decoder thread does the same with a quarter of its pool).
ANALYSE = 2
BEHIND = 0.25
# The engine (False: the loop feeds even where the module has it, for benches), burst buffers
# between its detector and its decoders, packets a take collects.
ENGINE = True
POOL = 48
POOL_MIN = 12
# The time a pass takes packets for at most: in a busy band some 650 records a second come, some
# 0.3 ms each to take on the XIAO (the decoders leave burst records out while their ring is half
# full; the radio first, the table second).
TAKE_MS = 150
# More where the heap leaves POOL_RESERVE besides (the P4's 32 MB of PSRAM: its decoders, on one
# core, fell behind in a busy moment), POOL_MAX at most. The packets kept, the view and the
# display need some 2 to 3 MB on the VIEWE: a pool of the free heap but 40 % ran it out.
POOL_MAX = 128
POOL_RESERVE = 6 << 20
TAKE = 64
# Packets kept, and how many go at once when that is reached.
KEEP = 1500
TRIM = 200
# The view is built again at most this often while packets come.
VIEW_MS = 300
GAINS = ("auto", "20", "30", "40", "50", "60")


def _channels():
    out = [("BLE 37", 2402e6), ("BLE 38", 2426e6), ("BLE 39", 2480e6)]
    out += [("15.4 {}".format(c), (2405 + 5 * (c - 11)) * 1e6) for c in range(11, 27)]
    return out


CHANNELS = _channels()


def stream():
    """The rate to receive at and that stream's full scale: one of RATES (the lower first) the
    radio streams filtered, else one of its packet rates; None without either."""
    pools = ((getattr(esdr, "RATES", ()), getattr(esdr, "FULL_SCALE", 16384)),
             (getattr(esdr, "PACKET_RATES", ()), getattr(esdr, "PACKET_FULL_SCALE", 16384)))
    for rates, fs in pools:
        for r in RATES:
            if r in rates:
                return r, fs
    return None


def _shift(fs):
    """Left shift of the stream's samples to the decoders' full scale (16384)."""
    s = 0
    while fs < 16384 and s < 8:
        fs *= 2
        s += 1
    return s


class Capture:
    """The radio's ring (or the engine), the capture state and the packets kept."""

    def __init__(self):
        got = stream()
        if got is None:
            raise OSError(22, "the radio streams neither 2 nor 4 MS/s here")
        self.rate, fs = got
        self.ch = 0
        self.gain = "auto"
        self.freq = CHANNELS[0][1]
        self.engine = ENGINE and hasattr(esdr, "pcap_start")
        # Samples the engine's ring takes at least where it reads one (the P4), else none.
        ring_min = getattr(esdr, "PCAP_RING_MIN", 0) if self.engine else RING_MIN
        self.pkts = []
        self.count = 0
        self.trimmed = 0
        self.overruns = 0
        self.version = 0  # counts changes of the packets kept
        self.running = False
        self.t_start = time.ticks_ms()
        self.epoch_ns = time.time_ns() if hasattr(time, "time_ns") else 0
        # The stream's index of this run's first sample, samples of this run fed so far, and
        # the indices skipped for the detector at the radio's gaps.
        self.at0 = 0
        self.read = 0
        self.skew = 0
        self.run_t0 = 0.0
        self.gaps = 0
        self.rate_t = [(self.t_start, 0)]
        self.ring = None
        if self.engine:
            # POOL burst buffers (5 ms each) or as many as the free heap holds beyond
            # POOL_RESERVE; fewer while the heap has no room for them in one piece.
            gc.collect()
            pool = min(POOL_MAX, max(POOL, (gc.mem_free() - POOL_RESERVE) // (self.rate // 50)))
            while True:
                gc.collect()
                try:
                    self.state = esdr.pcap_new(self.freq, self.rate, shift=_shift(fs),
                                               pool=pool)
                    break
                except MemoryError:
                    if pool <= POOL_MIN:
                        raise
                    pool //= 2
            if not ring_min:
                return
        else:
            self.state = esdr.pcap_new(self.freq, self.rate, shift=_shift(fs))
        gc.collect()
        n = min(RING_ENGINE if self.engine else RING_MAX, int(gc.mem_free() * RING_SHARE) // 4)
        self.ring = None
        while self.ring is None:
            if n < ring_min:
                raise MemoryError("no room for the receive ring")
            try:
                self.ring = bytearray(4 * n)
            except MemoryError:
                # The free heap is in pieces: a smaller ring.
                n //= 2

    def _gain_index(self):
        return -1 if self.gain == "auto" else int(self.gain)

    def start(self):
        esdr.channel_bw(0)
        esdr.tune(self.freq, offset=True)
        esdr.rx_gain(self._gain_index())
        esdr.rx_bandwidth(0)
        esdr.pcap_tune(self.state, self.freq, 255 if self.gain == "auto" else int(self.gain))
        self.at0 += self.read + self.skew + 1
        self.read = 0
        self.skew = 0
        self.run_t0 = time.ticks_diff(time.ticks_ms(), self.t_start) / 1000
        self.running = True
        if self.engine:
            # Its stage (the P4: its ring) counts from 0 at every start.
            self.at0 = 0
            if self.ring is None:
                esdr.pcap_start(self.state)
            else:
                esdr.pcap_start(self.state, self.ring)
            return
        esdr.rx_start(self.rate, self.ring, 0, wrap=True)
        self.gaps = esdr.rx_stats().get("gaps", 0)

    def stop(self):
        if not self.running:
            return
        self.running = False
        if self.engine:
            esdr.pcap_stop(self.state)
        else:
            esdr.rx_stop()
        self._add(esdr.pcap_flush(self.state, ANALYSE))

    def retune(self, ch=None, gain=None):
        if ch is not None:
            self.ch = ch
            self.freq = CHANNELS[ch][1]
        if gain is not None:
            self.gain = gain
        was = self.running
        self.stop()
        if was:
            self.start()

    def _add(self, raw, made=False):
        """Packets in: the module's tuples, or Pkt objects it made (made)."""
        pkts = self.pkts
        for r in raw:
            self.count += 1
            if made:
                p = r
                t = p.t
            else:
                t = self.run_t0 + (r[1] - self.at0) / self.rate
                p = pv.Pkt(self.count, t, self.freq, r)
            # In time order (the view's Newest needs no sort): a record of a burst's numbers
            # can come before a packet decoded from an earlier burst. The few late ones are
            # moved back into place: MicroPython's sort is quadratic on a list that is nearly
            # in order, and calls the key at every comparison (933 packets: 4.8 s on the S31).
            k = len(pkts)
            while k > 0 and pkts[k - 1].t > t:
                k -= 1
            pkts.insert(k, p)
        if raw:
            self.version += 1
        over = len(pkts) - KEEP
        if over > 0:
            # Down to TRIM below KEEP: a pass may bring hundreds.
            del pkts[:over + TRIM]
            self.trimmed += over + TRIM

    def poll(self):
        """Feed what came since the last pass (FEED_MS at most), or take what the engine's
        decoders found (TAKE_MS at most)."""
        if not self.running:
            return []
        t0 = time.ticks_ms()
        if self.engine:
            return self._take(t0)
        cap = len(self.ring) // 4
        total = esdr.rx_count()
        gaps = esdr.rx_stats().get("gaps", 0)
        if gaps != self.gaps:
            # The radio lost input somewhere since: a gap for the detector.
            self.gaps = gaps
            self.skew += 1
        if total - self.read > cap - FEED_SAMPLES:
            # The loop fell behind the ring: on behind the writer (the index jumps: a gap).
            self.overruns += 1
            self.read = total - FEED_SAMPLES
        new = []
        while self.read < total and time.ticks_diff(time.ticks_ms(), t0) < FEED_MS:
            pos = self.read % cap
            m = min(total - self.read, cap - pos, FEED_SAMPLES)
            at = self.at0 + self.read + self.skew
            analyse = ANALYSE if total - self.read < cap * BEHIND else 0
            raw = esdr.pcap_feed(self.state, self.ring, pos, m, at, analyse)
            self.read += m
            new += raw
        self._add(new)
        return new

    def _take(self, t0):
        # The engine's decoders run on their own thread: their packets, TAKE at a time, made
        # Pkt objects in C where the module can (pcap_take_pkts: dissected, their attributes
        # in one allocation).
        made = getattr(esdr, "pcap_take_pkts", None)
        new = []
        while time.ticks_diff(time.ticks_ms(), t0) < TAKE_MS:
            if made is None:
                got = esdr.pcap_take(self.state, TAKE)
            else:
                got = made(self.state, TAKE, pv.Pkt, self.count + len(new), self.run_t0,
                           self.at0, self.rate, self.freq)
            new += got
            if len(got) < TAKE:
                break
        self._add(new, made is not None)
        return new

    def per_second(self):
        now = time.ticks_ms()
        self.rate_t.append((now, self.count))
        while len(self.rate_t) > 2 and time.ticks_diff(now, self.rate_t[1][0]) > 2000:
            self.rate_t.pop(0)
        t, n = self.rate_t[0]
        span = time.ticks_diff(now, t)
        return (self.count - n) * 1000 / span if span > 0 else 0.0

    def load(self):
        """The capture's share of the time since the app started (of one CPU: on the badge's P4
        the engine's threads run on the other, so it may pass 100 %), and what it lost: the ring
        overrun, or the engine a stage behind the radio or out of burst buffers."""
        st = esdr.pcap_stats(self.state)
        span = time.ticks_diff(time.ticks_ms(), self.t_start) * 1000
        busy = st["busy_us"] + st.get("det_us", 0) + st.get("dec_us", 0)
        lost = st.get("lapped", 0) + st.get("pool_full", 0) + st.get("ring_lost", 0) \
            if self.engine else self.overruns
        return (busy / span if span > 0 else 0.0), lost

    def save(self):
        """The packets kept as pcapng on the SD card; returns the path."""
        import os

        board = os.uname().machine
        path = None
        for k in range(1000):
            p = "/SD:/pcap/esdr-{:03d}.pcapng".format(k)
            try:
                os.stat(p)
            except OSError:
                path = p
                break
        path = blocks.board_path(path or "/SD:/pcap/esdr-999.pcapng")
        vol = blocks._volume_of(path)
        was = self.running
        self.stop()
        esdr.claim(vol)
        try:
            blocks._makedirs(path)
            with open(path, "wb") as f:
                blocks._write(f, esdr.pcapng_head(board, "Zephyr", "esp-sdr MicroPython Pcap"),
                              path)
                for p in self.pkts:
                    t_ns = self.epoch_ns + int(p.t * 1e9)
                    blocks._write(f, esdr.pcapng_epb(p.iface, t_ns, p.rec, p.comment), path)
        finally:
            esdr.release(vol)
            if was:
                self.start()
        return path


class View:
    """What the table shows: the settings and the rows built from the capture."""

    def __init__(self, cap):
        self.cap = cap
        self.filt = pv.FILTERS[0]
        self.group = pv.GROUPS[0]
        self.sort = pv.SORTS[0]
        self.source = None
        self.sel = 0
        self.hold = False
        self._rows = []
        self._built = None
        self._t = 0
        self._wait = VIEW_MS
        # The capture as the rows saw it: its newest packet, its count and its trims.
        self._last = None
        self._count = 0
        self._trimmed = 0
        self.version = 0  # counts changes of the rows or the selection

    def changed(self):
        self._built = None

    def rows(self):
        now = time.ticks_ms()
        key = (self.cap.version, self.filt, self.group, self.sort, self.source)
        settings = self._built is None or key[1:] != self._built[1:]
        fresh = self._built is None or (key != self._built and not self.hold and
                                        time.ticks_diff(now, self._t) >= self._wait)
        if settings or fresh:
            if settings or not self._grow():
                self._rows = pv.view(self.cap.pkts, self.filt, self.group, self.sort,
                                     self.source)
            c = self.cap
            self._last = c.pkts[-1] if c.pkts else None
            self._count = c.count
            self._trimmed = c.trimmed
            self._built = key
            self._t = time.ticks_ms()
            # A view that takes long (grouped, sorted by source) is built less often: the
            # capture shares the loop.
            self._wait = max(VIEW_MS, 4 * time.ticks_diff(self._t, now))
            self.sel = min(self.sel, max(0, len(self._rows) - 1))
            self.version += 1
        return self._rows

    def _grow(self):
        """The newest first, ungrouped: the packets that came since the last build put in front,
        when they all came after its newest (a late one before it, a trim or another view: a
        whole build). Filtering 1300 packets for every frame kept the table at some 2 a second
        on the XIAO."""
        c = self.cap
        pkts = c.pkts
        n = c.count - self._count
        j = len(pkts) - n - 1
        if (self.group != pv.GROUPS[0] or self.sort != "Newest" or self._last is None or
                c.trimmed != self._trimmed or n <= 0 or j < 0 or pkts[j] is not self._last):
            return False
        new = pv._filtered(pkts[j + 1:], self.filt, self.source)
        new.reverse()
        self._rows[0:0] = new
        return True

    def selected(self):
        rows = self._rows
        return rows[self.sel] if 0 <= self.sel < len(rows) else None

    def select(self, k):
        self.sel = max(0, min(int(k), max(0, len(self._rows) - 1)))
        self.version += 1


class _Table(qtgui._sink):
    """The capture view: a header, a line per row, the selected row in full below; a bar at the
    right edge selects the row."""

    _draws = True
    BAR = 14
    LINE = 10

    def __init__(self, view):
        super().__init__("pcap", "c", "Packets")
        self.view = view
        # Text for a table at most 4 times a second: the capture shares the loop.
        self._update = 0.25
        self._bar = None
        self._bar_n = -1
        self._first = 0
        self._shown = None
        self._rates = None
        self.note = ""

    def _make(self, parent, x, y, w, h):
        super()._make(parent, x, y, w, h)
        lv = _gui.lv
        b = lv.slider(parent)
        b.set_size(self.BAR - 6, max(20, h - qtgui.LABEL - 6))
        b.set_pos(x + w - self.BAR + 3, y + qtgui.LABEL + 3)
        b.set_range(0, 1)
        b.set_value(1, False)
        _gui.on(b, lv.EVENT.VALUE_CHANGED, self._moved, b.get_value, latest=True)
        _gui.add_to_group(b)
        self._bar = b
        self._bar_n = -1
        self._shown = None

    def _top(self):
        return max(1, self._bar_n - 1)

    def _moved(self, v):
        # The bar's top is the first row.
        self.view.select(self._top() - int(v))

    def _sync_bar(self, n):
        if self._bar is None:
            return
        if n != self._bar_n:
            self._bar_n = n
            self._bar.set_range(0, self._top())
        self._bar.set_value(self._top() - self.view.sel, False)

    def _label(self):
        v = self.view
        c = v.cap
        # The engine's numbers (a dict of them) once a second, the counts every frame.
        sec = time.ticks_ms() // 1000
        if self._rates is None or self._rates[0] != sec:
            self._rates = (sec, c.load(), c.per_second())
        (load, lost), rate = self._rates[1], self._rates[2]
        text = "{}/{} {:.0f}/s {:.0f}%".format(len(v._rows), len(c.pkts), rate, 100 * load)
        if lost:
            text += " lost {}".format(lost)
        if self.note:
            text += " " + self.note
        return text

    def _draw(self):
        v = self.view
        rows = v.rows()
        # The header's rates change without new rows: once a second at least.
        key = (v.version, len(rows), self.note, time.ticks_ms() // 1000)
        if key == self._shown:
            # Nothing new: the GUI loop leaves the canvas alone (no flush to the panel).
            return False
        self._shown = key
        self._sync_bar(len(rows))
        self._head()
        fb = self._fbo
        x, y, w, h = self._plot()
        w -= self.BAR
        chars = w // 8
        lines = h // self.LINE
        extra = 3 if lines >= 10 else (2 if lines >= 6 else 0)
        body = max(1, lines - 1 - extra)
        fb.fill_rect(x, y, w, h, _gui.BG)
        # From the rows drawn: the settings may have changed since they were built.
        grouped = bool(rows) and isinstance(rows[0], pv.Group)
        cols = pv.columns(chars, grouped)
        fb.text(pv.header(cols), x, y + 1, _gui.MUTED)
        fb.hline(x, y + self.LINE - 1, w, _gui.DIM)
        sel = v.sel
        if sel < self._first:
            self._first = sel
        elif sel >= self._first + body:
            self._first = sel - body + 1
        self._first = max(0, min(self._first, len(rows) - body))
        for k in range(body):
            r = self._first + k
            if r >= len(rows):
                break
            row = rows[r]
            yy = y + self.LINE * (k + 1)
            p = row.latest if isinstance(row, pv.Group) else row
            colour = _gui.TRACE[p.iface % len(_gui.TRACE)] if p is not None and p.ok \
                else _gui.MUTED
            if r == sel:
                fb.fill_rect(x, yy, w, self.LINE, _gui.FAINT)
                colour = _gui.FG
            fb.text(pv.line(row, cols), x, yy + 1, colour)
        if extra:
            yy = y + self.LINE * (body + 1)
            fb.hline(x, yy, w, _gui.DIM)
            for k, text in enumerate(pv.detail(v.selected(), chars, extra)):
                fb.text(text, x, yy + 2 + self.LINE * k, _gui.FG)


def _bar(*parts):
    """A tool bar: (label, widget) pairs."""
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


# Below this many rows for the table under the controls (the Communicator's 142-row panel), the
# table and the controls get a tab each.
TABLE_MIN = 100


def _tabbed():
    return _gui.H - _gui.HEADER - 3 * _gui.ROW < TABLE_MIN


def _gui_up(cap, view):
    root = page_table = page_setup = None
    if _tabbed():
        root = Qt.QWidget()
        tabs = Qt.QTabWidget()
        Qt.QVBoxLayout(root).addWidget(tabs)
        page_table, page_setup = Qt.QWidget(), Qt.QWidget()
        tabs.addTab(page_table, "Packets")
        tabs.addTab(page_setup, "Setup")
        page_table, page_setup = Qt.QVBoxLayout(page_table), Qt.QVBoxLayout(page_setup)
    _gui.reset(root)
    table = _Table(view)
    if page_table is not None:
        page_table.addWidget(table)

    def pick_ch(i):
        cap.retune(ch=i)
        _gui.title("Pcap " + CHANNELS[i][0])
        if _gui._head is not None:
            _gui._header()

    def pick_filter(i):
        f = pv.FILTERS[i]
        # The Source filter keeps the selected row's source, chosen now.
        view.source = pv.row_source(view.selected()) if f == "Source" and view.selected() \
            else None
        view.filt = f
        view.sel = 0

    def pick_group(i):
        view.group = pv.GROUPS[i]
        view.sel = 0

    def pick_sort(i):
        view.sort = pv.SORTS[i]
        view.sel = 0

    def pick_gain(i):
        cap.retune(gain=GAINS[i])

    def save():
        try:
            path = cap.save()
            table.note = "saved " + path.rsplit("/", 1)[-1]
        except OSError as e:
            table.note = "not saved: " + (e.args[-1] if e.args else "error")
            print("Pcap:", e)

    def hold(on):
        view.hold = bool(on)
        view.changed()

    _gui.title("Pcap " + CHANNELS[cap.ch][0])
    names = [c[0] for c in CHANNELS]
    button = Qt.QPushButton("Save")
    button.clicked.connect(save)
    check = Qt.QCheckBox("Hold")
    check.toggled.connect(hold)
    bars = (_bar(("Ch", _chooser(names, cap.ch, pick_ch)),
                 ("Show", _chooser(pv.FILTERS, pv.FILTERS.index(view.filt), pick_filter))),
            _bar(("Group", _chooser(pv.GROUPS, pv.GROUPS.index(view.group), pick_group)),
                 ("Sort", _chooser(pv.SORTS, pv.SORTS.index(view.sort), pick_sort))),
            _bar(("Gain", _chooser(GAINS, GAINS.index(cap.gain), pick_gain)), ("", button),
                 ("", check)))
    if page_setup is not None:
        for bar in bars:
            page_setup.addWidget(bar)
    return table


def _print_new(new):
    """Without a display: each packet as a line on the console."""
    cols = pv.columns(78, False)
    for p in new[-20:]:
        print(pv.line(p, cols))


def main():
    esdr.init()
    cap = Capture()
    view = View(cap)
    headless = _gui.headless()

    def poll():
        new = cap.poll()
        if headless and new:
            _print_new(cap.pkts[-len(new):])

    _gui_up(cap, view)
    cap.start()
    _gui.poll(poll)
    _gui.show()
    try:
        _gui.run()
    finally:
        cap.stop()
