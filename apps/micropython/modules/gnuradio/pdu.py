# SPDX-License-Identifier: GPL-3.0-or-later
"""pdu: PDUs (pairs of a metadata dictionary and a u8vector) into and out of packet
flowgraphs on the board (gnuradio/_link.py). pdu_to_tagged_stream hands a PDU's bytes to the
transmit chain as a burst, tagged_stream_to_pdu publishes a received frame with what the
receiver measured in the metadata."""

import pmt
from gnuradio import gr


class pdu_to_tagged_stream(gr.basic_block):
    _kind = "pdu2ts"

    def __init__(self, type, tag="packet_len"):
        super().__init__("pdu_to_tagged_stream")
        if type != gr.types.byte_t:
            raise ValueError("pdu_to_tagged_stream on the board: byte items")
        self.in_itemsize = 0
        self.out_itemsize = gr.sizeof_char
        self._tag = tag
        self._link = None
        self.dropped = 0
        self.message_port_register_in("pdus")
        self.set_msg_handler("pdus", self._pdu)

    def _pdu(self, msg):
        if not pmt.is_pair(msg) or not pmt.is_u8vector(pmt.cdr(msg)):
            raise ValueError("pdu_to_tagged_stream: a PDU (metadata, u8vector)")
        if self._link is None:
            self.dropped += 1
            return
        self._link.send(self, bytes(pmt.u8vector_elements(pmt.cdr(msg))))


class tagged_stream_to_pdu(gr.basic_block):
    _kind = "ts2pdu"

    def __init__(self, type, tag="packet_len"):
        super().__init__("tagged_stream_to_pdu")
        if type != gr.types.byte_t:
            raise ValueError("tagged_stream_to_pdu on the board: byte items")
        self.in_itemsize = gr.sizeof_char
        self.out_itemsize = 0
        self._tag = tag
        self.message_port_register_out("pdus")

    def _emit(self, items, meta):
        data = items.items() if isinstance(items, gr.Bits) else bytes(items)
        d = pmt.make_dict()
        for k, v in meta.items():
            d = pmt.dict_add(d, pmt.intern(k), pmt.to_pmt(v))
        self.message_port_pub("pdus", pmt.cons(d, pmt.init_u8vector(len(data), data)))
