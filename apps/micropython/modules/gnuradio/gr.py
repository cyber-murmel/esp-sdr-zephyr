# SPDX-License-Identifier: GPL-3.0-or-later
"""gr.top_block for the board. A receive flowgraph is one radio source, an optional head, and
sinks; a transmit flowgraph is one chain from a signal source (analog.sig_source_c,
blocks.file_source, blocks.vector_source_c) through throttle, multiply_const or head blocks
to soapy.sink.

A flowgraph runs in esdr's engines, not in Python: start() configures the radio and starts
the capture or the transmission, the REPL stays free (input() and the like), stop() ends it
and writes the file sinks, wait() returns when a bounded run (head, a file played once) has
finished.
"""

import esdr

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


class tx_source(basic_block):
    """The signal of a transmit flowgraph: its _tx_args() tell esdr's transmit engine what to
    play, {"gen": f(scale) -> (wave, cycles, amp, dc_i, dc_q)} or {"buf": int16 I/Q,
    "repeat": bool}. soapy.sink starts the engine; stop() and wait() end up here."""

    _kind = "txsource"
    in_itemsize = 0

    def __init__(self, name):
        super().__init__(name)
        self._tb = None

    def _tx_args(self):
        raise NotImplementedError

    def _stop(self, tb):
        esdr.tx_stop()

    def _wait(self, tb):
        esdr.tx_wait()


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
        self._chain = []

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
        if any(b._kind == "txsink" for b in blocks):
            self._plan_tx(blocks)
            return
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

    def _plan_tx(self, blocks):
        sinks = [b for b in blocks if b._kind == "txsink"]
        sources = [b for b in blocks if b._kind == "txsource"]
        if len(sinks) != 1 or len(sources) != 1:
            raise ValueError("a transmit flowgraph needs one signal source and one soapy.sink")
        others = [b for b in blocks if b._kind not in ("txsource", "txsink", "through", "head")]
        if others:
            raise ValueError("block not supported in a transmit flowgraph on the board: " +
                             others[0].name())
        nxt = {}
        for src, dst in self._edges:
            if src in nxt:
                raise ValueError("a transmit flowgraph on the board is one chain: " + src.name())
            nxt[src] = dst
        chain = [sources[0]]
        while chain[-1] in nxt and len(chain) <= len(blocks):
            chain.append(nxt[chain[-1]])
        if chain[-1] is not sinks[0] or len(chain) != len(blocks):
            raise ValueError("a transmit flowgraph on the board is one chain from the source to "
                             "soapy.sink")
        heads = [b for b in chain if b._kind == "head"]
        self._source = sources[0]
        self._head = heads[0] if heads else None
        self._sinks = sinks
        self._chain = chain

    def _tx_scale(self):
        """The product of the chain's multiply_const blocks."""
        s = 1.0
        for b in self._chain:
            s *= getattr(b, "_scale", 1.0)
        return s

    def _tx_live(self):
        # A source or multiply_const setting changed while transmitting.
        if self._running and self._chain:
            self._sinks[0]._live()

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
