# SPDX-License-Identifier: GPL-3.0-or-later
"""blocks: the sinks, head and file source that run on the board.

file_sink buffers the capture in the Python heap (PSRAM) and writes it when the flowgraph
stops: a flash write during the capture would stall the receive ring. The file holds the
items as GNU Radio writes them (fc32 for gr_complex, sc16 for short*2), readable on a host
with numpy.fromfile().

Recordings go to the SD card only: a "/SD:..." path is used as it is, any other path, such as
the host path an exported flowgraph carries, is placed under /SD: ("/tmp/rx.fc32" ->
"/SD:/tmp/rx.fc32"). /FLASH: holds the scripts; writing captures there would stall the
receive ring (flash writes park its CPU) and fill the 4 MB volume.
"""

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
