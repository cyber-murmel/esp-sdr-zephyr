# SPDX-License-Identifier: GPL-3.0-or-later
"""gr.top_block for the board: one radio source or file source, an optional head, and sinks.

A flowgraph runs in esdr's engine on CPU 1, not in Python: start() configures the radio and
starts the capture, the REPL stays free (input() and the like), stop() ends it and writes
the file sinks, wait() returns when a bounded capture (head) has finished.
"""

sizeof_char = 1
sizeof_short = 2
sizeof_int = 4
sizeof_float = 4
sizeof_gr_complex = 8


class basic_block:
    """What connect() needs from a block: its name, item sizes and role."""

    _kind = None
    in_itemsize = 0
    out_itemsize = 0

    def __init__(self, name):
        self._name = name

    def name(self):
        return self._name

    # Hooks of the runtime, called by top_block.
    def _start(self, tb):
        pass

    def _stop(self, tb):
        pass


def _block_of(point):
    return point[0] if isinstance(point, tuple) else point


def _port_of(point):
    return point[1] if isinstance(point, tuple) else 0


class top_block:
    def __init__(self, name="top_block", catch_exceptions=True):
        self._name = name
        self._edges = []
        self._running = False
        self._source = None
        self._head = None
        self._sinks = []

    def name(self):
        return self._name

    def connect(self, *points):
        if len(points) == 1:
            # A lone block (a source with nothing attached): nothing to run.
            return
        for a, b in zip(points, points[1:]):
            src, dst = _block_of(a), _block_of(b)
            if _port_of(a) != 0 or _port_of(b) != 0:
                raise ValueError("only port 0 is supported")
            if src.out_itemsize != dst.in_itemsize:
                raise ValueError("itemsize mismatch: {} ({}) -> {} ({})".format(
                    src.name(), src.out_itemsize, dst.name(), dst.in_itemsize))
            self._edges.append((src, dst))

    def disconnect(self, *points):
        for a, b in zip(points, points[1:]):
            self._edges.remove((_block_of(a), _block_of(b)))

    def disconnect_all(self):
        self._edges = []

    def lock(self):
        pass

    def unlock(self):
        pass

    def _plan(self):
        # source -> [head] -> sinks, sinks fanned out from the source or the head.
        blocks = []
        for e in self._edges:
            for b in e:
                if b not in blocks:
                    blocks.append(b)
        sources = [b for b in blocks if b._kind == "source"]
        if len(sources) != 1:
            raise ValueError("a flowgraph needs exactly one source (has {})".format(len(sources)))
        heads = [b for b in blocks if b._kind == "head"]
        if len(heads) > 1:
            raise ValueError("at most one head")
        others = [b for b in blocks if b._kind not in ("source", "head", "sink")]
        if others:
            raise ValueError("block not supported on the board: " + others[0].name())
        self._source = sources[0]
        self._head = heads[0] if heads else None
        self._sinks = [b for b in blocks if b._kind == "sink"]
        for src, dst in self._edges:
            if dst._kind == "head" and src is not self._source:
                raise ValueError("head must follow the source")
            if dst._kind == "sink" and src is not self._source and src is not self._head:
                raise ValueError("sink {} must follow the source or the head".format(dst.name()))
        if not self._sinks:
            raise ValueError("a flowgraph needs a sink")

    def start(self, max_noutput_items=10000000):
        if self._running:
            raise RuntimeError("already running")
        self._plan()
        self._items = self._head.n if self._head else 0
        self._running = True
        try:
            for b in self._sinks:
                b._start(self)
            self._source._start(self)
        except BaseException:
            self._running = False
            raise

    def stop(self):
        if self._running:
            self._source._stop(self)
            self._finish()

    def wait(self):
        if self._running:
            self._source._wait(self)
            self._finish()

    def run(self, max_noutput_items=10000000):
        self.start(max_noutput_items)
        self.wait()

    def _finish(self):
        self._running = False
        for b in self._sinks:
            b._stop(self)
