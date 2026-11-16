# SPDX-License-Identifier: GPL-3.0-or-later
"""Two-way link: short messages, numbered, acknowledged, sent again until acknowledged.

In: send (the text: a QT GUI Msg Push Button's pair, a symbol, a string), rx (PDUs from the
receiver), tick (a Message Strobe, the block's clock). Out: tx (PDUs for the modulator), and
pairs for QT GUI Labels through Message Pair to Var blocks: rx (the last message heard), tx
(messages sent, acknowledged, sent again, lost), link (what the receiver measured on the last
frame: SNR, level, carrier offset; frames whose CRC failed), peer (the SNR the peer measured,
the round trip time, this end's id).

Frame, the PDU after GNU Radio's default packet header: kind (D data, A acknowledgement), the
sender's id (2 bytes), a sequence number; data carries the text, an acknowledgement the SNR its
sender measured (dB, 255 unknown). Then CRC-32 (little-endian) and two bytes of padding that
push the CRC through gfsk_mod's filter: GNU Radio holds a packet's last symbols until more come.
Stop and wait: one message at a time, sent again after timeout s plus up to half of it at
random, up to retries times. Auto sends a numbered message every period s (+-20 %).
"""

import binascii
import os
import random
import time

import pmt
from gnuradio import gr

DATA = 0x44
ACK = 0x41
PAD = b"\x55\x55"
TEXT_MAX = 40
QUEUE_MAX = 4

try:
    _now = time.ticks_ms
    _diff = time.ticks_diff
except AttributeError:
    def _now():
        return int(time.monotonic() * 1000)

    def _diff(a, b):
        return a - b


def frame(kind, node, seq, body):
    b = bytes([kind, node >> 8, node & 255, seq & 255]) + body
    return b + binascii.crc32(b).to_bytes(4, "little") + PAD


def parse(data):
    """(kind, node, seq, body) of a frame whose CRC holds, else None."""
    n = len(data) - 4 - len(PAD)
    if n < 4 or binascii.crc32(data[:n]) != int.from_bytes(data[n:n + 4], "little"):
        return None
    return data[0], (data[1] << 8) | data[2], data[3], data[4:n]


def _text(b):
    try:
        s = b.decode()
    except Exception:
        s = repr(b)
    return "".join(c if " " <= c <= "~" else "?" for c in s)


class blk(gr.basic_block):
    """Two-way link (stop and wait)"""

    def __init__(self, auto=False, period=2.0, timeout=0.4, retries=5):
        gr.basic_block.__init__(self, name="Two-way link", in_sig=None, out_sig=None)
        self.auto = auto
        self.period = period
        self.timeout = timeout
        self.retries = retries
        for port, handler in (("send", self._send), ("rx", self._rx), ("tick", self._tick)):
            self.message_port_register_in(pmt.intern(port))
            self.set_msg_handler(pmt.intern(port), handler)
        for port in ("tx", "rx_text", "tx_text", "link_text", "peer_text"):
            self.message_port_register_out(pmt.intern(port))
        seed = os.urandom(4)
        random.seed(int.from_bytes(seed, "little"))
        self.node = int.from_bytes(seed[:2], "little")
        self.seq = 0
        self.queue = []
        self.out = None  # [seq, frame, sends, last send, first send, wait]
        self.count = 0
        self.next_auto = None
        self.last = {}
        self.n = {"msgs": 0, "acked": 0, "retries": 0, "lost": 0, "heard": 0, "dup": 0,
                  "bad": 0}
        self.heard = "-"
        self.snr = self.rssi = self.offset = self.peer_snr = self.rtt = None
        self.shown = {}
        self.show_t = None

    def _pub(self, data):
        self.message_port_pub(pmt.intern("tx"), pmt.cons(pmt.make_dict(),
                                                         pmt.init_u8vector(len(data), list(data))))

    def _send(self, msg):
        if pmt.is_pair(msg):
            msg = pmt.cdr(msg)
        if pmt.is_symbol(msg):
            text = pmt.symbol_to_string(msg)
        else:
            text = str(pmt.to_python(msg))
        if len(self.queue) < QUEUE_MAX:
            self.queue.append(text)
        self._next(_now())

    def _next(self, now):
        if self.out is not None or not self.queue:
            return
        self.seq = (self.seq + 1) & 255
        f = frame(DATA, self.node, self.seq, self.queue.pop(0).encode()[:TEXT_MAX])
        self.out = [self.seq, f, 1, now, now, self._wait()]
        self.n["msgs"] += 1
        self._pub(f)
        self._show(now, True)

    def _wait(self):
        return int(1000 * self.timeout * (1 + random.random() / 2))

    def _tick(self, msg):
        now = _now()
        o = self.out
        if o is not None and _diff(now, o[3]) >= o[5]:
            if o[2] <= self.retries:
                o[2] += 1
                o[3] = now
                o[5] = self._wait()
                self.n["retries"] += 1
                self._pub(o[1])
            else:
                self.n["lost"] += 1
                self.out = None
        if self.auto and self.period > 0:
            if self.next_auto is None or _diff(now, self.next_auto) >= 0:
                self.next_auto = now + int(1000 * self.period * (0.8 + 0.4 * random.random()))
                if not self.queue and self.out is None:
                    self.count += 1
                    self.queue.append("auto {}".format(self.count))
        else:
            self.next_auto = None
        self._next(now)
        self._show(now)

    def _rx(self, msg):
        now = _now()
        meta = pmt.to_python(pmt.car(msg)) if pmt.is_dict(pmt.car(msg)) else None
        if not isinstance(meta, dict):  # GNU Radio gives None for an empty one
            meta = {}
        f = parse(bytes(pmt.u8vector_elements(pmt.cdr(msg))))
        if f is None:
            self.n["bad"] += 1
            self._show(now, True)
            return
        kind, node, seq, body = f
        if node == self.node:
            return
        for key in ("snr", "rssi"):
            if key in meta:
                setattr(self, key, meta[key])
        if "freq_offset" in meta:
            self.offset = meta["freq_offset"]
        if kind == DATA:
            snr = 255 if self.snr is None else max(0, min(254, int(self.snr + 0.5)))
            self._pub(frame(ACK, self.node, seq, bytes([snr])))
            if self.last.get(node) == seq:
                self.n["dup"] += 1
            else:
                self.last[node] = seq
                self.n["heard"] += 1
                self.heard = "{:04x} #{}: {}".format(node, seq, _text(body))
        elif kind == ACK and self.out is not None and seq == self.out[0]:
            self.n["acked"] += 1
            self.rtt = _diff(now, self.out[3])
            self.peer_snr = body[0] if body and body[0] != 255 else None
            self.out = None
            self._next(now)
        self._show(now, True)

    def _show(self, now, force=False):
        if not force and self.show_t is not None and _diff(now, self.show_t) < 500:
            return
        self.show_t = now
        n = self.n
        texts = {
            "rx_text": self.heard,
            "tx_text": "{} sent, {} acked, {} again, {} lost{}".format(
                n["msgs"], n["acked"], n["retries"], n["lost"],
                " (#{})".format(self.out[0]) if self.out else ""),
            "link_text": "{}, {}, {}, {} bad".format(
                "- dB" if self.snr is None else "{:.0f} dB".format(self.snr),
                "-" if self.rssi is None else "{:.0f} dBFS".format(self.rssi),
                "-" if self.offset is None else "{:+.1f} kHz".format(self.offset / 1000),
                n["bad"]),
            "peer_text": "{}, rtt {}, me {:04x}".format(
                "- dB" if self.peer_snr is None else "{} dB".format(self.peer_snr),
                "-" if self.rtt is None else "{} ms".format(self.rtt), self.node),
        }
        for port, text in texts.items():
            if self.shown.get(port) != text:
                self.shown[port] = text
                self.message_port_pub(pmt.intern(port), pmt.cons(pmt.intern(port),
                                                                 pmt.intern(text)))
