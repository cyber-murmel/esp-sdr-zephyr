# SPDX-License-Identifier: GPL-3.0-or-later
"""blocks: the sinks, head, sources and pass-through blocks that run on the board.

file_sink buffers the capture in the Python heap (PSRAM) and writes it when the flowgraph
stops: a flash write during the capture would stall the receive ring. The file holds the
items as GNU Radio writes them (fc32 for gr_complex, sc16 for short*2), readable on a host
with numpy.fromfile().

Recordings go to the SD card only: a "/SD:..." path is used as it is, any other path, such as
the host path an exported flowgraph carries, is placed under /SD: ("/tmp/rx.fc32" ->
"/SD:/tmp/rx.fc32"). /FLASH: holds the scripts; writing captures there would stall the
receive ring (flash writes park its CPU) and fill the 4 MB volume.

file_source (transmit) reads the whole file into the heap before the transmission starts,
from the path as given if it names a volume, else from under /SD: or, failing that, /FLASH:;
fc32 samples become 16-bit, so 1.0 is full scale. throttle passes samples through (the radio
sets the pace) and multiply_const_cc scales them in the transmit engine.
"""

import gc
import os

import esdr
from gnuradio import gr

# Sink chunks converted per file write (samples).
_CHUNK = 4096
_VOLUMES = ("/SD:", "/FLASH:")


def _mounted(mnt):
    try:
        os.statvfs(mnt)
        return True
    except OSError:
        return False


def _volume_of(path):
    for v in _VOLUMES:
        if path.startswith(v):
            return v
    return None


def board_path(path):
    """Where a flowgraph's recording goes on the board (see the module docstring)."""
    if path.startswith("/FLASH:"):
        raise ValueError("recordings go to the SD card, not /FLASH:: " + path)
    if not _mounted("/SD:"):
        raise OSError(19, "no SD card mounted for " + path)
    if path.startswith("/SD:"):
        return path
    return "/SD:" + ("" if path.startswith("/") else "/") + path


def _write(f, data, path):
    # A full volume writes short without an error.
    if f.write(data) != len(data):
        raise OSError(28, "no space left on " + _volume_of(path))


def _makedirs(path):
    vol = _volume_of(path)
    parts = path[len(vol):].split("/")[:-1]
    d = vol
    for p in parts:
        if not p:
            continue
        d += "/" + p
        try:
            os.mkdir(d)
        except OSError:
            pass


class null_sink(gr.basic_block):
    _kind = "sink"

    def __init__(self, itemsize, vlen=1):
        super().__init__("null_sink")
        self.in_itemsize = itemsize * vlen
        self.out_itemsize = 0

    def _want_buffer(self):
        return False


class head(gr.basic_block):
    _kind = "head"

    def __init__(self, itemsize, n, vlen=1):
        super().__init__("head")
        self.in_itemsize = self.out_itemsize = itemsize * vlen
        self.n = int(n)

    def set_length(self, n):
        self.n = int(n)

    def reset(self):
        pass


class file_sink(gr.basic_block):
    _kind = "sink"

    def __init__(self, itemsize, filename, append=False):
        super().__init__("file_sink")
        if itemsize not in (gr.sizeof_gr_complex, 2 * gr.sizeof_short):
            raise ValueError("file_sink on the board: gr_complex or short*2 items")
        self.in_itemsize = itemsize
        self.out_itemsize = 0
        self._filename = filename
        self._append = append
        self.written = 0
        self.path = None

    def _want_buffer(self):
        return True

    def _start(self, tb):
        # Before the capture: no card, no recording.
        self.path = board_path(self._filename)

    def set_unbuffered(self, unbuffered):
        pass

    def open(self, filename):
        self._filename = filename
        return True

    def close(self):
        pass

    def _stop(self, tb):
        buf, n = tb._source._captured()
        if buf is None or self.path is None:
            return
        path = self.path
        vol = _volume_of(path)
        mv = memoryview(buf)
        # The volume leaves the USB host while the board writes it (esdr.claim()).
        esdr.claim(vol)
        try:
            _makedirs(path)
            with open(path, "ab" if self._append else "wb") as f:
                if self.in_itemsize == 2 * gr.sizeof_short:
                    _write(f, mv[:4 * n], path)
                else:
                    out = bytearray(8 * _CHUNK)
                    scale = 1.0 / esdr.FULL_SCALE
                    for k in range(0, n, _CHUNK):
                        m = min(_CHUNK, n - k)
                        esdr.sc16_to_fc32(mv[4 * k:4 * (k + m)], out, scale)
                        _write(f, memoryview(out)[:8 * m], path)
        finally:
            esdr.release(vol)
        self.written = n
        if path != self._filename:
            print("file_sink: {} -> {}".format(self._filename, path))


# ---- transmit ----


def source_path(path):
    """Where a flowgraph's input file is on the board (see the module docstring)."""
    if _volume_of(path) is not None:
        return path
    rel = path if path.startswith("/") else "/" + path
    for vol in _VOLUMES:
        try:
            os.stat(vol + rel)
            return vol + rel
        except OSError:
            pass
    raise OSError(2, "no such file on /SD: or /FLASH:: " + path)


def _sc16_room():
    gc.collect()
    return (gc.mem_free() * 3 // 4) // 4


class file_source(gr.tx_source):
    def __init__(self, itemsize, filename, repeat=False, offset=0, len=0):
        super().__init__("file_source")
        if itemsize not in (gr.sizeof_gr_complex, 2 * gr.sizeof_short):
            raise ValueError("file_source on the board: gr_complex or short*2 items")
        self.out_itemsize = itemsize
        self.open(filename, repeat, offset, len)

    def open(self, filename, repeat=False, offset=0, len=0):
        self._filename = filename
        self._repeat = bool(repeat)
        self._offset = int(offset)
        self._len = int(len)
        self._buf = None
        self.path = None

    def close(self):
        self._buf = None

    def set_begin_tag(self, tag):
        pass

    def seek(self, seek_point, whence):
        return False

    def _load(self):
        path = source_path(self._filename)
        isz = self.out_itemsize
        items = max(0, os.stat(path)[6] // isz - self._offset)
        if self._len:
            items = min(items, self._len)
        room = _sc16_room()
        if items > room:
            print("file_source: {} has {} samples, {} fit in memory".format(path, items, room))
            items = room
        if items == 0:
            raise ValueError("file_source: no samples in " + path)
        buf = bytearray(4 * items)
        out = memoryview(buf)
        chunk = bytearray(isz * 4096)
        raw = memoryview(chunk)
        with open(path, "rb") as f:
            f.seek(self._offset * isz)
            k = 0
            while k < items:
                n = min(4096, items - k)
                got = f.readinto(raw[:n * isz]) // isz
                if got <= 0:
                    break
                if isz == gr.sizeof_gr_complex:
                    esdr.fc32_to_sc16(raw[:got * isz], out[4 * k:4 * (k + got)], 32767.0)
                else:
                    out[4 * k:4 * (k + got)] = raw[:got * isz]
                k += got
        self._buf = buf if k == items else buf[:4 * k]
        self.path = path
        if path != self._filename:
            print("file_source: {} -> {}".format(self._filename, path))

    def _tx_args(self):
        if self._buf is None:
            self._load()
        return {"buf": self._buf, "repeat": self._repeat}


class vector_source_c(gr.tx_source):
    def __init__(self, data, repeat=False, vlen=1, tags=()):
        super().__init__("vector_source_c")
        if vlen != 1:
            raise ValueError("vector_source_c on the board: vlen 1")
        self.out_itemsize = gr.sizeof_gr_complex
        self._repeat = bool(repeat)
        self.set_data(data)

    def set_data(self, data, tags=()):
        import array

        a = array.array("h", [0] * (2 * len(data)))
        for k, v in enumerate(data):
            v = complex(v)
            a[2 * k] = max(-32767, min(32767, int(v.real * 32767)))
            a[2 * k + 1] = max(-32767, min(32767, int(v.imag * 32767)))
        self._buf = bytearray(a)
        if self._tb is not None and self._tb._running:
            self._tb._sinks[0]._restart()

    def set_repeat(self, repeat):
        self._repeat = bool(repeat)

    def rewind(self):
        pass

    def _tx_args(self):
        return {"buf": self._buf, "repeat": self._repeat}


class throttle(gr.basic_block):
    """Passes samples through: the radio sets the pace."""

    _kind = "through"

    def __init__(self, itemsize, samples_per_sec, ignore_tags=True, maximum_items_per_chunk=0):
        super().__init__("throttle")
        self.in_itemsize = self.out_itemsize = itemsize
        self._rate = float(samples_per_sec)
        self._tb = None

    def set_sample_rate(self, rate):
        self._rate = float(rate)

    def sample_rate(self):
        return self._rate

    def set_maximum_items_per_chunk(self, n):
        pass


class multiply_const_cc(gr.basic_block):
    """Scales the transmitted samples by a real constant (in the transmit engine)."""

    _kind = "through"

    def __init__(self, k, vlen=1):
        super().__init__("multiply_const_cc")
        if vlen != 1:
            raise ValueError("multiply_const_cc on the board: vlen 1")
        self.in_itemsize = self.out_itemsize = gr.sizeof_gr_complex
        self._tb = None
        self.set_k(k)

    def set_k(self, k):
        if isinstance(k, (list, tuple)):
            k = k[0]
        k = complex(k)
        if k.imag:
            raise ValueError("multiply_const_cc on the board: a real constant")
        self._scale = k.real
        if self._tb is not None:
            self._tb._tx_live()

    def k(self):
        return self._scale


def multiply_const_vcc(k):
    return multiply_const_cc(k[0] if isinstance(k, (list, tuple)) else k)


# ---- packets (tagged streams in packet flowgraphs, gnuradio/_link.py) and messages ----


def _items(v):
    """A packet's items: gr.Bits unpacked, anything else as bytes."""
    return v.items() if isinstance(v, gr.Bits) else bytes(v)


class repack_bits_bb(gr.basic_block):
    """k bits per input byte to l bits per output byte, a packet at a time; correlate_access_code
    _bb_ts's bits to bytes (k 1, l 8) take no unpacking."""

    _kind = "pkt"

    def __init__(self, k, l, len_tag_key="", align_output=False, endianness=gr.GR_LSB_FIRST):
        super().__init__("repack_bits_bb")
        self.in_itemsize = self.out_itemsize = gr.sizeof_char
        self.set_k_and_l(k, l)
        self._msb = endianness == gr.GR_MSB_FIRST

    def set_k_and_l(self, k, l):
        if not (1 <= int(k) <= 8 and 1 <= int(l) <= 8):
            raise ValueError("repack_bits_bb: 1 to 8 bits")
        self._k = int(k)
        self._l = int(l)

    def _pkt(self, inputs):
        v = inputs[0]
        if isinstance(v, gr.Bits) and self._k == 1 and self._l == 8 and self._msb:
            return bytes(v.data[:v.nbits // 8])
        bits = []
        for b in _items(v):
            for j in range(self._k):
                bits.append((b >> (self._k - 1 - j if self._msb else j)) & 1)
        out = bytearray(len(bits) // self._l)
        for i in range(len(out)):
            x = 0
            for j in range(self._l):
                if bits[i * self._l + j]:
                    x |= 1 << (self._l - 1 - j if self._msb else j)
            out[i] = x
        return bytes(out)


class tagged_stream_mux(gr.basic_block):
    """Its inputs' packets one after the other (a header before its payload)."""

    _kind = "pkt"

    def __init__(self, itemsize, lengthtagname, tag_preserve_head_pos=0):
        super().__init__("tagged_stream_mux")
        if itemsize != gr.sizeof_char:
            raise ValueError("tagged_stream_mux on the board: byte items")
        self.in_itemsize = self.out_itemsize = itemsize

    def _pkt(self, inputs):
        return b"".join(_items(v) for v in inputs)


class message_strobe(gr.basic_block):
    """Publishes its message every period ms while the flowgraph runs (from the Qt GUI loop)."""

    def __init__(self, msg, period_ms):
        super().__init__("message_strobe")
        self.message_port_register_out("strobe")
        self.message_port_register_in("set_msg")
        self.set_msg_handler("set_msg", self.set_msg)
        self._msg = msg
        self._ms = max(1, int(period_ms))
        self.timeout = self

    def set_msg(self, msg):
        self._msg = msg

    def msg(self):
        return self._msg

    def set_period(self, period_ms):
        self._ms = max(1, int(period_ms))

    def period(self):
        return self._ms

    # The Qt GUI loop takes it for a QTimer (_ms, timeout.emit()).
    def emit(self):
        self.message_port_pub("strobe", self._msg)

    def _start(self, tb):
        from gnuradio import _gui

        _gui.timer(self)

    def _stop(self, tb):
        from gnuradio import _gui

        _gui.timer(self, False)


class message_debug(gr.basic_block):
    """Prints messages on the console (print, print_pdu) or keeps them (store)."""

    def __init__(self, en_uvec=True, log_level=gr.log_levels.info):
        super().__init__("message_debug")
        self._store = []
        for port, h in (("print", self._print), ("store", self._store.append),
                        ("print_pdu", self._print)):
            self.message_port_register_in(port)
            self.set_msg_handler(port, h)

    def _print(self, msg):
        import pmt

        print("message_debug:", pmt.to_python(msg))

    def num_messages(self):
        return len(self._store)

    def get_message(self, i):
        return self._store[i]


class msg_pair_to_var(gr.basic_block):
    """Sets a flowgraph variable (its setter, callback) to the value of each message pair."""

    def __init__(self, callback):
        super().__init__("msg_pair_to_var")
        self._cb = callback
        self.message_port_register_in("inpair")
        self.set_msg_handler("inpair", self._pair)

    def _pair(self, msg):
        import pmt

        if pmt.is_pair(msg):
            self._cb(pmt.to_python(pmt.cdr(msg)))

    def set_variable(self, callback):
        self._cb = callback
