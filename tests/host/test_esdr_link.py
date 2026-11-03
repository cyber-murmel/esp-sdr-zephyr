# SPDX-License-Identifier: GPL-3.0-or-later
"""The framed link's Python twin (apps/osmosdr_serial/tools/esdr_serial.py), held to the vectors
tests/unit/esdr_link holds the C codec to, and its host side against a fake device."""

import re
import struct
import zlib
from pathlib import Path

import pytest
from conftest import REPO, load

es = load("apps/osmosdr_serial/tools/esdr_serial.py")


def cobs_by_the_rules(d):
    """COBS as the paper defines it, written apart from the tool's."""
    out = bytearray()
    blk = bytearray()
    for b in d:
        if b == 0:
            out.append(len(blk) + 1)
            out += blk
            blk = bytearray()
        else:
            blk.append(b)
            if len(blk) == 254:
                out.append(255)
                out += blk
                blk = bytearray()
    out.append(len(blk) + 1)
    out += blk
    return bytes(out)


@pytest.mark.parametrize("ftype, seq, payload, wire", [
    (1, 0, b"", "020101010579b8f89900"),
    (2, 0x1234, b"\x00\x01\x00\x00\xff", "020203341202010106ff1196734f00"),
    (6, 7, bytes(10), "020602070101010101010101010105fedbe15d00"),
])
def test_vectors(ftype, seq, payload, wire):
    assert es.link_encode(ftype, seq, payload).hex() == wire
    assert es.link_decode(bytes.fromhex(wire)[:-1]) == (ftype, seq, payload)


def test_long_vector():
    f = es.link_encode(4, 0xFFFF, bytes(range(1, 250)))
    assert len(f) == 260
    assert zlib.crc32(f) == 0xF2C42674


@pytest.mark.parametrize("n", [0, 1, 2, 253, 254, 255, 256, 507, 508, 509, 4096])
@pytest.mark.parametrize("fill", [0x00, 0xFF, 0x55])
def test_cobs_against_the_rules(n, fill):
    d = bytes([fill]) * n
    assert es.cobs_encode(d) == cobs_by_the_rules(d)
    assert es.cobs_decode(es.cobs_encode(d)) == d


def test_cobs_counter_across_blocks():
    d = bytes(k % 255 + 1 for k in range(1000))
    assert es.cobs_encode(d) == cobs_by_the_rules(d)
    assert es.cobs_decode(es.cobs_encode(d)) == d


def test_no_zero_inside_a_frame():
    f = es.link_encode(4, 0, bytes(5000))
    assert f.index(0) == len(f) - 1


@pytest.mark.parametrize("k", [0, 1, 5, 20])
def test_a_flipped_byte_fails(k):
    f = bytearray(es.link_encode(4, 3, bytes(range(1, 30)))[:-1])
    f[k] ^= 0x10
    with pytest.raises(ValueError):
        es.link_decode(bytes(f))


def test_constants_match_the_header():
    text = (Path(REPO) / "include/esp_sdr/esdr_link.h").read_text()
    for name, value in (("HELLO", es.LINK_HELLO), ("REQUEST", es.LINK_REQUEST),
                        ("REPLY", es.LINK_REPLY), ("BLOCK", es.LINK_BLOCK), ("TEXT", es.LINK_TEXT),
                        ("CREDIT", es.LINK_CREDIT), ("BYE", es.LINK_BYE)):
        assert re.search(rf"ESDR_LINK_{name} = {value},", text), name
    assert f"ESDR_LINK_VERSION {es.LINK_VERSION}U" in text
    coproc = (Path(REPO) / "include/esp_sdr/mch2022_coproc.h").read_text()
    for name, value in (("SET_BAUD", es.COPROC_SET_BAUD), ("ESP32_RESET", es.COPROC_ESP32_RESET),
                        ("LINK_OPEN", es.COPROC_LINK_OPEN), ("LINK_CLOSE", es.COPROC_LINK_CLOSE)):
        assert re.search(rf"COPROC_REQ_{name} = {value:#04x},", coproc), name
    proto = (Path(REPO) / "apps/osmosdr/src/esdr_proto.h").read_text()
    assert f"ESDR_REQ_SET_LINK_BAUD = {es.REQ_SET_LINK_BAUD:#04x}," in proto


class FakeBadge:
    """interface 6 and an osmosdr_serial behind it, as far as frames go."""

    def __init__(self, answers_at=2000000, boots_at=None):
        self.baud = 2000000
        self.esp_baud = boots_at or 2000000
        self.answers = answers_at
        self.link_open = False
        self.to_host = bytearray()
        self.writes = []
        self.requests = []
        self.credit = 0
        self.seq = 0
        self.resets = 0
        self.closed = False

    def ctrl_transfer(self, rt, req, value, index, data):
        assert (rt, index) == (0x41, es.COPROC_IF_LINK)
        if req == es.COPROC_SET_BAUD:
            self.baud = struct.unpack("<I", data)[0]
        elif req == es.COPROC_LINK_OPEN:
            self.link_open = True
        elif req == es.COPROC_LINK_CLOSE:
            self.closed = True
        elif req == es.COPROC_ESP32_RESET:
            self.resets += 1
            self.esp_baud = 2000000

    def _say(self, ftype, payload):
        self.to_host += es.link_encode(ftype, self.seq, payload)
        self.seq += 1

    def write(self, ep, data, timeout=0):
        assert ep == es.COPROC_EP_OUT
        data = bytes(data)
        self.writes.append(data)
        if self.baud != self.esp_baud:
            return len(data)
        for wire in data.split(b"\0"):
            if not wire:
                continue
            ftype, _, payload = es.link_decode(wire)
            if ftype == es.LINK_HELLO:
                self.credit = es.LINK_HELLO_FMT.unpack(payload)[3]
                self._say(es.LINK_HELLO, es.LINK_HELLO_FMT.pack(1, 0, 600, self.credit, 5000000))
            elif ftype == es.LINK_CREDIT:
                self.credit += struct.unpack("<I", payload)[0]
            elif ftype == es.LINK_REQUEST:
                h = es.HDR.unpack_from(payload)
                self.requests.append(h[1])
                if h[1] == es.REQ_SET_LINK_BAUD:
                    self._say(es.LINK_REPLY, es.HDR.pack(es.SER_MAGIC, h[1], 0, 0, 0, 0))
                    self.esp_baud = struct.unpack_from("<I", payload, es.HDR.size)[0]
                else:
                    body = b"\x2a" * (h[4] if h[2] & es.SER_IN else 0)
                    self._say(es.LINK_REPLY,
                              es.HDR.pack(es.SER_MAGIC, h[1], 0, 0, len(body), 0) + body)
        return len(data)

    def read(self, ep, n, timeout=0):
        assert ep == es.COPROC_EP_IN
        if not self.to_host:
            raise OSError("timeout")
        out = bytes(self.to_host[:n])
        del self.to_host[:n]
        return out


def framed(badge, baud=2000000):
    return es.EsdrFramed(badge, baud, sleep=lambda s: None)


def test_opening_says_hello_with_the_credit():
    b = FakeBadge()
    d = framed(b)
    assert b.link_open
    assert b.credit == es.LINK_WINDOW
    assert d.hello[2] == 600
    assert b.resets == 0


def test_a_badge_at_another_rate_is_reset():
    b = FakeBadge(boots_at=5000000)
    framed(b)
    assert b.resets == 1


def test_a_silent_badge_gets_the_uart_back():
    b = FakeBadge()
    b.esp_baud = 1
    b.ctrl_reset = b.ctrl_transfer

    def ctrl(rt, req, value, index, data):
        b.ctrl_reset(rt, req, value, index, data)
        b.esp_baud = 1
    b.ctrl_transfer = ctrl
    with pytest.raises(TimeoutError):
        framed(b)
    assert b.closed


def test_requests_and_status():
    b = FakeBadge()
    d = framed(b)
    assert d.request("GET_INFO", in_len=4) == b"\x2a" * 4
    assert b.requests == [es.REQ["GET_INFO"]]


def test_a_failed_request_raises():
    b = FakeBadge()
    d = framed(b)
    b.to_host += es.link_encode(es.LINK_REPLY, 1,
                                es.HDR.pack(es.SER_MAGIC, es.REQ["SET_MODE"], 0, 0, 0, -22))
    b.write = lambda ep, data, timeout=0: len(data)
    with pytest.raises(OSError):
        d.request("SET_MODE", value=1)


def test_going_up_in_rate_changes_both_ends():
    b = FakeBadge()
    framed(b, 5000000)
    assert b.baud == 5000000
    assert b.esp_baud == 5000000


def test_closing_goes_back_for_the_next_host():
    b = FakeBadge()
    d = framed(b, 5000000)
    d.close()
    assert b.baud == es.LINK_BOOT_BAUD
    assert b.esp_baud == es.LINK_BOOT_BAUD
    assert b.closed


def test_blocks_come_as_they_are():
    b = FakeBadge()
    d = framed(b)
    body = es.BLK.pack(es.BLOCK_MAGIC, 0, 3, 77) + bytes(6)
    b._say(es.LINK_BLOCK, body)
    kind, h, payload = d._next(0.5)
    assert kind == "block"
    assert h[2:] == (3, 77)
    assert payload == bytes(6)


def test_credit_comes_back_as_blocks_are_read():
    b = FakeBadge()
    d = framed(b)
    start = b.credit
    body = es.BLK.pack(es.BLOCK_MAGIC, 0, 500, 0) + bytes(1000)
    total = 0
    for _ in range(10):
        b._say(es.LINK_BLOCK, body)
        total += len(es.link_encode(es.LINK_BLOCK, 0, body))
        d._next(0.5)
    granted = b.credit - start
    assert total - es.LINK_CREDIT_STEP < granted <= total + 16


def test_a_broken_frame_costs_that_frame():
    b = FakeBadge()
    d = framed(b)
    body = es.BLK.pack(es.BLOCK_MAGIC, 0, 1, 5) + bytes(2)
    good = es.link_encode(es.LINK_BLOCK, b.seq + 1, body)
    b.to_host += good[:3] + good[4:]
    b.seq += 1
    b._say(es.LINK_BLOCK, body)
    kind, h, _ = d._next(0.5)
    assert kind == "block"
    assert d.bad_frames == 1
    assert d.lost_frames == 1


def test_whole_packet_writes_end_with_a_short_packet():
    b = FakeBadge()
    d = framed(b)
    b.writes.clear()
    d._write(bytes(64))
    assert [len(w) for w in b.writes] == [64, 0]
    b.writes.clear()
    d._write(bytes(65))
    assert [len(w) for w in b.writes] == [65]
