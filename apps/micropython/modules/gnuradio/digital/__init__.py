# SPDX-License-Identifier: GPL-3.0-or-later
"""digital: the (G)FSK packet blocks for packet flowgraphs on the board (gnuradio/_link.py),
with GNU Radio's names and parameters. The modem is _fsk.py, run by esdr in C (cmod/esdr_fsk.c)
when the firmware has it.

gfsk_mod modulates as GNU Radio's does. gfsk_demod with correlate_access_code_bb_ts is a burst
receiver that also takes a carrier offset of tens of kHz (the deviation's order) and needs no
time to lock: the access code gives the offset and the sampling point (see _fsk.py); with
GNU Radio's own gfsk_demod the offset must stay well inside the deviation. The receiver
delivers what correlate_access_code_bb_ts does, the payload after the access code and the
two lengths, of up to 255 bytes."""

import array

import esdr
from gnuradio import gr
from gnuradio.digital import _fsk


class packet_utils:
    default_access_code = "1010110011011101101001001110001011110010100011000010000011111100"


class header_format_default:
    """The access code, then the payload length in bytes twice (16 bits each)."""

    def __init__(self, access_code, threshold, bps=1):
        if isinstance(access_code, int):
            access_code = packet_utils.default_access_code
        self._code, self._code_len = _fsk.code_bits(access_code)
        if self._code_len % 8:
            raise ValueError("header_format_default: an access code of whole bytes")
        self._threshold = int(threshold)

    def header_nbits(self):
        return self._code_len + 32

    def header_nbytes(self):
        return self.header_nbits() // 8

    def access_code(self):
        return self._code

    def threshold(self):
        return self._threshold

    def format(self, nbytes):
        return _fsk.header(nbytes, self._code, self._code_len)

    def base(self):
        return self


header_format_base = header_format_default


class protocol_formatter_bb(gr.basic_block):
    """The header of each packet (the payload goes on beside it to a tagged_stream_mux)."""

    _kind = "pkt"

    def __init__(self, format, len_tag_key="packet_len"):
        super().__init__("protocol_formatter_bb")
        self.in_itemsize = self.out_itemsize = gr.sizeof_char
        self.set_header_format(format)

    def set_header_format(self, format):
        self._format = format

    def _pkt(self, inputs):
        return self._format.format(len(inputs[0]))


class gfsk_mod(gr.basic_block):
    _kind = "mod"

    def __init__(self, samples_per_symbol=2, sensitivity=1.0, bt=0.35, verbose=False, log=False,
                 do_unpack=True):
        super().__init__("gfsk_mod")
        sps = int(samples_per_symbol)
        if not 2 <= sps <= _fsk.SPS_MAX:
            raise ValueError("gfsk_mod: samples per symbol 2 .. {}".format(_fsk.SPS_MAX))
        self.in_itemsize = gr.sizeof_char
        self.out_itemsize = gr.sizeof_gr_complex
        self._sps = sps
        self._sens = float(sensitivity)
        self._taps = array.array("f", _fsk.gfsk_taps(sps, float(bt)))
        self._unpack = bool(do_unpack)

    def samples_per_symbol(self):
        return self._sps

    @staticmethod
    def bits_per_symbol(self=None):
        return 1

    def _burst(self, items, amp, pad, buf=None):
        """A packet's samples (int16 I, Q) at amp (the multiply_const blocks after this one),
        then silence up to pad samples: in buf when it holds that many (the rest of it zeroed),
        else in a new buffer. Returns (buffer, samples, samples with the silence)."""
        if isinstance(items, gr.Bits):
            data, nbits = items.data, items.nbits
        elif self._unpack:
            data, nbits = bytes(items), 8 * len(items)
        else:
            data, nbits = _pack(items), len(items)
        n = _fsk.mod_samples(nbits, self._sps, len(self._taps))
        # gfsk_mod's own 0.999 keeps the samples off full scale.
        a = 0.999 * amp
        need = max(n, pad)
        if hasattr(esdr, "fsk_mod"):
            if buf is None or len(buf) < 4 * need:
                buf = bytearray(4 * need)
            esdr.fsk_mod(data, nbits, self._sps, self._taps, self._sens, a, buf, 0.0)
        else:
            buf = array.array("h", bytes(4 * need))
            _fsk.modulate(data, nbits, self._sps, self._taps, self._sens, a, buf)
        return buf, n, need


def _pack(bits):
    out = bytearray((len(bits) + 7) // 8)
    for k, b in enumerate(bits):
        if b & 1:
            out[k >> 3] |= 0x80 >> (k & 7)
    return bytes(out)


class gfsk_demod(gr.basic_block):
    _kind = "demod"

    def __init__(self, samples_per_symbol=2, sensitivity=1.0, gain_mu=0.175, mu=0.5,
                 omega_relative_limit=0.005, freq_error=0.0, verbose=False, log=False):
        super().__init__("gfsk_demod")
        sps = int(samples_per_symbol)
        if not 2 <= sps <= _fsk.SPS_MAX:
            raise ValueError("gfsk_demod: samples per symbol 2 .. {}".format(_fsk.SPS_MAX))
        self.in_itemsize = gr.sizeof_gr_complex
        self.out_itemsize = gr.sizeof_char
        self._sps = sps

    def samples_per_symbol(self):
        return self._sps

    @staticmethod
    def bits_per_symbol(self=None):
        return 1


class correlate_access_code_bb_ts(gr.basic_block):
    _kind = "sync"

    def __init__(self, access_code, threshold, tag_name="packet_len"):
        super().__init__("correlate_access_code_bb_ts")
        self.in_itemsize = self.out_itemsize = gr.sizeof_char
        self._code, self._code_len = _fsk.code_bits(access_code)
        if self._code_len < 8:
            raise ValueError("correlate_access_code_bb_ts on the board: 8 to 64 bits of code")
        self._threshold = int(threshold)
        self._tag = tag_name

    def set_access_code(self, access_code):
        self._code, self._code_len = _fsk.code_bits(access_code)
        return True

    def access_code(self):
        return self._code

    def _receiver(self, sps):
        if hasattr(esdr, "fsk_rx_init"):
            return _CReceiver(sps, self._code, self._code_len, self._threshold)
        return _PyReceiver(sps, self._code, self._code_len, self._threshold)


class _PyReceiver(_fsk.Receiver):
    def busy(self):
        return self.state != _fsk.SEARCH


class _CReceiver:
    """_fsk.Receiver in esdr (C). Where the radio engine runs it at a fraction of the rate
    (esdr.RX_FSK_SLOW, the ESP32-C5), it is made for that fraction of the samples per symbol, if
    at least 4 are left."""

    def __init__(self, sps, code, code_len, threshold, max_len=_fsk.LEN_MAX):
        import gc

        import gnuradio

        slow = getattr(esdr, "RX_FSK_SLOW", 1)
        self.slow = slow if sps % slow == 0 and sps // slow >= 4 else 1
        code = code.to_bytes(8, "big")
        if gnuradio._rxroom is not None:
            # The room kept for the state since the first import.
            gnuradio._rxroom = None
            gc.collect()
        self._st = esdr.fsk_rx_init(sps // self.slow, code, code_len, threshold, max_len)

    def reset(self):
        esdr.fsk_rx_reset(self._st)

    def busy(self):
        return esdr.fsk_rx_state(self._st) != _fsk.SEARCH

    def process(self, iq, n, start=0):
        return [_fsk.Frame(*t) for t in esdr.fsk_rx(self._st, iq, start, n)]

    @staticmethod
    def frame(t):
        return _fsk.Frame(*t)
