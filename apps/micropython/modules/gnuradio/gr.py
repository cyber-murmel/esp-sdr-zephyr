# SPDX-License-Identifier: GPL-3.0-or-later
"""gr.top_block for the board. A receive flowgraph is one radio source, an optional head, and
sinks; a transmit flowgraph is one chain from a signal source (analog.sig_source_c,
blocks.file_source, blocks.vector_source_c) through throttle, multiply_const or head blocks
to soapy.sink; a packet flowgraph (digital.gfsk_mod, gfsk_demod and the blocks around them)
shares the radio between its receive and transmit chains (gnuradio/_link.py).

A flowgraph runs in esdr's engines, not in Python: start() configures the radio and starts
the capture or the transmission, the REPL stays free (input() and the like), stop() ends it
and writes the file sinks, wait() returns when a bounded run (head, a file played once) has
finished.

Messages pass as in GNU Radio, asynchronously: message_port_pub() queues a message for the
blocks connected to the port, dispatch() hands the queue to their handlers in order. The Qt
GUI loop dispatches (gnuradio/_gui.py), so do packet flowgraphs' radio passes.
"""

import esdr

sizeof_char = 1
sizeof_short = 2
sizeof_int = 4
sizeof_float = 4
sizeof_gr_complex = 8

GR_MSB_FIRST = 0
GR_LSB_FIRST = 1


class types:
    """Item types of the PDU blocks (gr.types.byte_t, ...): their sizes."""

    byte_t = 1
    short_t = 2
    int_t = 4
    float_t = 4
    complex_t = 8


class log_levels:
    trace = 0
    debug = 1
    info = 2
    warn = 3
    error = 4
    critical = 5
    off = 6


def io_signature(min_streams, max_streams, itemsize):
    return (min_streams, max_streams, itemsize)


class Bits:
    """A tagged stream packet of unpacked bits (an item per bit, as correlate_access_code_bb_ts
    gives them), kept packed: nbits bits of data, most significant first."""

    def __init__(self, data, nbits):
        self.data = data
        self.nbits = nbits

    def items(self):
        out = bytearray(self.nbits)
        for k in range(self.nbits):
            out[k] = (self.data[k >> 3] >> (7 - (k & 7))) & 1
        return bytes(out)

    def __len__(self):
        return self.nbits


_queue = []


def _port(port):
    """A message port's name: the pmt symbol's string, or the string itself."""
    return port.value if hasattr(port, "value") else str(port)


def dispatch(limit=64):
    """Hand queued messages to their blocks' handlers, in order, up to limit of them (the
    handlers may queue more)."""
    n = 0
    while _queue and n < limit:
        dst, port, msg = _queue.pop(0)
        dst._deliver(port, msg)
        n += 1
    return n


class basic_block:
    """What connect() needs from a block (its name, item sizes and role) and message ports.
    Embedded Python blocks of GRC (gr.sync_block, gr.basic_block subclasses with message ports
    only) run as they are."""

    _kind = None
    in_itemsize = 0
    out_itemsize = 0

    def __init__(self, name="block", in_sig=None, out_sig=None):
        self._name = name
        self._msg_out = {}
        self._msg_in = {}

    def name(self):
        return self._name

    def to_basic_block(self):
        return self

    def set_block_alias(self, alias):
        pass

    def message_port_register_in(self, port):
        self._msg_in.setdefault(_port(port), None)

    def message_port_register_out(self, port):
        self._msg_out.setdefault(_port(port), [])

    def set_msg_handler(self, port, handler):
        self._msg_in[_port(port)] = handler

    def message_port_pub(self, port, msg):
        for dst, dport in self._msg_out.get(_port(port), ()):
            _queue.append((dst, dport, msg))

    def _post(self, port, msg):
        _queue.append((self, _port(port), msg))

    def _deliver(self, port, msg):
        handler = self._msg_in.get(port)
        if handler is not None:
            handler(msg)

    # Hooks of the runtime, called by top_block.
    def _start(self, tb):
        pass

    def _stop(self, tb):
        pass


sync_block = basic_block


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


# Blocks that make a flowgraph a packet flowgraph (gnuradio/_link.py).
_LINK_KINDS = ("mod", "demod", "sync", "pkt", "pdu2ts", "ts2pdu")


class top_block:
    def __init__(self, name="top_block", catch_exceptions=True):
        self._name = name
        self._edges = []  # (src, src port, dst, dst port)
        self._msg_blocks = []
        self._running = False
        self._source = None
        self._head = None
        self._sinks = []
        self._chain = []
        self._link = None
        _queue.clear()

    def name(self):
        return self._name

    def connect(self, *points):
        if len(points) == 1:
            # A lone block (a source with nothing attached): nothing to run.
            return
        for a, b in zip(points, points[1:]):
            src, dst = _block_of(a), _block_of(b)
            if src.out_itemsize != dst.in_itemsize:
                raise ValueError("itemsize mismatch: {} ({}) -> {} ({})".format(
                    src.name(), src.out_itemsize, dst.name(), dst.in_itemsize))
            self._edges.append((src, _port_of(a), dst, _port_of(b)))

    def disconnect(self, *points):
        for a, b in zip(points, points[1:]):
            self._edges.remove((_block_of(a), _port_of(a), _block_of(b), _port_of(b)))

    def disconnect_all(self):
        self._edges = []

    def msg_connect(self, *args):
        """msg_connect((src, port), (dst, port)) or msg_connect(src, port, dst, port)."""
        if len(args) == 2:
            (src, sport), (dst, dport) = args
        else:
            src, sport, dst, dport = args
        src, dst = src.to_basic_block(), dst.to_basic_block()
        src._msg_out.setdefault(_port(sport), []).append((dst, _port(dport)))
        for b in (src, dst):
            if b not in self._msg_blocks:
                self._msg_blocks.append(b)

    def msg_disconnect(self, *args):
        if len(args) == 2:
            (src, sport), (dst, dport) = args
        else:
            src, sport, dst, dport = args
        routes = src._msg_out.get(_port(sport), [])
        if (dst, _port(dport)) in routes:
            routes.remove((dst, _port(dport)))

    def lock(self):
        pass

    def unlock(self):
        pass

    def _blocks(self):
        blocks = []
        for src, _, dst, _ in self._edges:
            for b in (src, dst):
                if b not in blocks:
                    blocks.append(b)
        return blocks

    def _plan(self):
        # source -> [head] -> sinks, sinks fanned out from the source or the head.
        blocks = self._blocks()
        self._link = None
        if any(b._kind in _LINK_KINDS for b in blocks):
            from gnuradio import _link

            self._link = _link.Link(self, blocks)
            return
        if not blocks and self._msg_blocks:
            return  # messages only
        for src, sp, dst, dp in self._edges:
            if sp != 0 or dp != 0:
                raise ValueError("only port 0 is supported")
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
        for src, _, dst, _ in self._edges:
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
        for src, _, dst, _ in self._edges:
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
        self._running = True
        try:
            if self._link is not None:
                self._link.start()
            elif self._edges:
                self._items = self._head.n if self._head else 0
                for b in self._sinks:
                    b._start(self)
                self._source._start(self)
            for b in self._msg_blocks:
                b._start(self)
            if self._msg_blocks and self._link is None:
                from gnuradio import _gui

                _gui.poll(dispatch)
        except BaseException:
            self._running = False
            if self._link is not None:
                self._link.stop()
            raise

    def stop(self):
        if self._running:
            for b in self._msg_blocks:
                b._stop(self)
            if self._link is not None:
                self._link.stop()
                self._running = False
                return
            if self._msg_blocks:
                from gnuradio import _gui

                _gui.poll(dispatch, False)
            if self._source is None:
                self._running = False
                return
            self._source._stop(self)
            self._finish()

    def wait(self):
        # A packet flowgraph runs until stopped (there is no other thread to stop it).
        if self._running and self._link is None and self._source is not None:
            self._source._wait(self)
            self._finish()

    def run(self, max_noutput_items=10000000):
        self.start(max_noutput_items)
        self.wait()

    def _finish(self):
        self._running = False
        for b in self._sinks:
            b._stop(self)
