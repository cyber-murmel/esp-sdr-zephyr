# SPDX-License-Identifier: GPL-3.0-or-later
"""MCUmgr (Simple Management Protocol) over UDP, the subset `west mcumgr` needs: image upload/list/test/confirm, os echo/reset/
params, file upload/download/status/crc. No dependencies (smpmgr cannot choose the UDP port, and the
dev shell has no cbor2): a small CBOR codec, the 8 byte header, and a client with one datagram per
request and the server's `off` as the resync after a lost one."""

import hashlib
import socket
import struct
import time
import zlib

# Header: op, flags, length, group, sequence, id (big endian)
HDR = struct.Struct(">BBHHBB")
OP_READ, OP_READ_RSP, OP_WRITE, OP_WRITE_RSP = 0, 1, 2, 3
GRP_OS, GRP_IMG, GRP_FS = 0, 1, 8
# os
OS_ECHO, OS_RESET, OS_PARAMS, OS_INFO, OS_BOOTLOADER = 0, 5, 6, 7, 8
# img
IMG_STATE, IMG_UPLOAD, IMG_ERASE = 0, 1, 5
# fs
FS_FILE, FS_STAT, FS_HASH = 0, 1, 2

PORT = 4996
# 1492 (the router's MTU, which the board's stack honours) - 40 (IPv6) - 8 (UDP)
DEFAULT_PAYLOAD = 1444

ERRORS = {1: "unknown error", 2: "out of memory", 3: "invalid argument", 4: "timeout", 5: "not found",
          6: "bad state", 7: "message too long", 8: "not supported", 9: "corrupt", 10: "busy",
          11: "access denied", 12: "too old", 13: "too new",
          256: "streaming: flash access refused (a stream runs and every flash access stalls it: stop the app)"}


class McumgrError(Exception):
    """A refusal or failure, with a hint where one helps."""


def err_text(rc, group=None):
    text = ERRORS.get(rc, f"error {rc}")
    if rc == 11 and group in (None, GRP_OS, GRP_IMG, GRP_FS):
        # The board answers the same for both: no detail comes with the code
        text += " (the gate is closed: `mcumgr arm <minutes>` on the board's shell; or a path outside /FLASH:/)"
    elif rc == 10:
        text += " (the volume is a USB drive, or another file command runs)"
    return text


# ---- CBOR: unsigned and negative integers, byte and text strings, arrays, maps, false/true/null ----

def cbor_head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
        if n < 1 << (8 * struct.calcsize(fmt)):
            return bytes([major << 5 | ai]) + struct.pack(fmt, n)
    raise ValueError("integer too large")


def cbor_dumps(v):
    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if isinstance(v, int):
        return cbor_head(0, v) if v >= 0 else cbor_head(1, -1 - v)
    if isinstance(v, (bytes, bytearray)):
        return cbor_head(2, len(v)) + bytes(v)
    if isinstance(v, str):
        b = v.encode()
        return cbor_head(3, len(b)) + b
    if isinstance(v, (list, tuple)):
        return cbor_head(4, len(v)) + b"".join(cbor_dumps(x) for x in v)
    if isinstance(v, dict):
        return cbor_head(5, len(v)) + b"".join(cbor_dumps(k) + cbor_dumps(x) for k, x in v.items())
    raise TypeError(f"cannot encode {type(v)}")


def cbor_loads(data):
    v, n = _decode(memoryview(data), 0)
    if n != len(data):
        raise ValueError("trailing bytes after the CBOR item")
    return v


def _arg(data, pos, ai):
    if ai < 24:
        return ai, pos
    size = {24: 1, 25: 2, 26: 4, 27: 8}.get(ai)
    if size is None:
        raise ValueError("bad CBOR additional information")
    if pos + size > len(data):
        raise ValueError("truncated CBOR")
    return int.from_bytes(data[pos:pos + size], "big"), pos + size


BREAK = object()


def _decode(data, pos, allow_break=False):
    if pos >= len(data):
        raise ValueError("truncated CBOR")
    major, ai = data[pos] >> 5, data[pos] & 31
    pos += 1
    if major == 7:
        if ai == 31:
            if not allow_break:
                raise ValueError("unexpected CBOR break")
            return BREAK, pos
        if ai in (20, 21, 22, 23):
            return {20: False, 21: True, 22: None, 23: None}[ai], pos
        if ai in (25, 26, 27):
            size = {25: 2, 26: 4, 27: 8}[ai]
            raw = bytes(data[pos:pos + size])
            if len(raw) != size:
                raise ValueError("truncated CBOR")
            fmt = {25: "e", 26: "f", 27: "d"}[ai]
            return struct.unpack(">" + fmt, raw)[0], pos + size
        raise ValueError("unsupported CBOR simple value")
    if ai == 31:
        # Indefinite length (zcbor writes maps like this): items up to a break
        if major in (2, 3):
            out = bytearray()
            while True:
                chunk, pos = _decode(data, pos, True)
                if chunk is BREAK:
                    break
                if not isinstance(chunk, (bytes, str)) or (isinstance(chunk, str) != (major == 3)):
                    raise ValueError("bad chunk in an indefinite CBOR string")
                out += chunk.encode() if major == 3 else chunk
            return (bytes(out) if major == 2 else out.decode()), pos
        if major in (4, 5):
            out = []
            while True:
                x, pos = _decode(data, pos, True)
                if x is BREAK:
                    break
                out.append(x)
            if major == 4:
                return out, pos
            if len(out) % 2:
                raise ValueError("indefinite CBOR map with a key and no value")
            return dict(zip(out[::2], out[1::2])), pos
        raise ValueError("bad CBOR additional information")
    n, pos = _arg(data, pos, ai)
    if major == 0:
        return n, pos
    if major == 1:
        return -1 - n, pos
    if major in (2, 3):
        if pos + n > len(data):
            raise ValueError("truncated CBOR")
        raw = bytes(data[pos:pos + n])
        return (raw if major == 2 else raw.decode()), pos + n
    if major == 4:
        out = []
        for _ in range(n):
            x, pos = _decode(data, pos)
            out.append(x)
        return out, pos
    if major == 5:
        out = {}
        for _ in range(n):
            k, pos = _decode(data, pos)
            out[k], pos = _decode(data, pos)
        return out, pos
    raise ValueError("unsupported CBOR type")


# ---- framing ----

def pack(op, group, cid, seq, body):
    payload = cbor_dumps(body)
    return HDR.pack(op, 0, len(payload), group, seq & 0xFF, cid) + payload


def unpack(frame):
    """(op, group, seq, id, body) of one datagram; ValueError when it is not a whole frame."""
    if len(frame) < HDR.size:
        raise ValueError("short frame")
    op, _flags, length, group, seq, cid = HDR.unpack_from(frame)
    if len(frame) != HDR.size + length:
        raise ValueError(f"frame of {len(frame)} bytes, header says {HDR.size + length}")
    return op, group, seq, cid, cbor_loads(frame[HDR.size:])


def upload_chunk(name_fields, data, off, limit):
    """The body of one upload request, with as much of data[off:] as fits a datagram of `limit` bytes.

    name_fields: the dict of the fields besides off and data (what the first request adds). Returns
    (body, bytes carried)."""
    room = limit - HDR.size - len(cbor_dumps({**name_fields, "off": off, "data": b""}))
    n = max(1, min(len(data) - off, room - 8))
    while True:
        body = {**name_fields, "off": off, "data": data[off:off + n]}
        if HDR.size + len(cbor_dumps(body)) <= limit or n == 1:
            return body, n
        n -= 8 if n > 8 else 1


# ---- an MCUboot image ----

IMAGE_MAGIC = 0x96F3B83D
TLV_INFO, TLV_PROT_INFO = 0x6907, 0x6908
TLV_SHA256 = 0x10


def image_hash(image):
    """The SHA-256 TLV of a signed MCUboot image (what `image list` shows as its hash)."""
    if len(image) < 32 or struct.unpack_from("<I", image)[0] != IMAGE_MAGIC:
        raise McumgrError("not an MCUboot image (the header magic is missing): sign it with imgtool, or use "
                       "the build's signed image (zephyr.signed.bin)")
    hdr_size, prot = struct.unpack_from("<HH", image, 8)
    img_size = struct.unpack_from("<I", image, 12)[0]
    pos = hdr_size + img_size
    end = pos
    while pos + 4 <= len(image):
        magic, total = struct.unpack_from("<HH", image, pos)
        if magic not in (TLV_INFO, TLV_PROT_INFO) or total < 4:
            break
        end = min(pos + total, len(image))
        p = pos + 4
        while p + 4 <= end:
            typ, _, ln = struct.unpack_from("<BBH", image, p)
            if typ == TLV_SHA256 and ln == 32:
                return bytes(image[p + 4:p + 4 + ln])
            p += 4 + ln
        pos = end
    if end == hdr_size + img_size:
        raise McumgrError("the image has no TLV area: it is not signed")
    raise McumgrError("the image's TLV area has no SHA-256")


# ---- the client ----

def resolve(host, port=PORT):
    """[(family, sockaddr)] of an IPv4 or IPv6 literal (a scope with %iface) or a name (mDNS: *.local)."""
    try:
        infos = socket.getaddrinfo(host, port, type=socket.SOCK_DGRAM)
    except socket.gaierror as e:
        hint = " (mDNS needs avahi or systemd-resolved with MulticastDNS; or give the address)" \
            if host.endswith(".local") else ""
        raise McumgrError(f"cannot resolve {host}: {e}{hint}") from e
    return [(i[0], i[4]) for i in infos]


class Client:
    def __init__(self, host, port=PORT, timeout=2.0, retries=3, payload=DEFAULT_PAYLOAD, sock=None):
        self.timeout, self.retries, self.payload = timeout, retries, payload
        self.seq = 0
        if sock is not None:
            self.sock = sock
            return
        last = None
        for family, addr in resolve(host, port):
            s = socket.socket(family, socket.SOCK_DGRAM)
            s.settimeout(timeout)
            try:
                s.connect(addr)
            except OSError as e:
                s.close()
                last = e
                continue
            self.sock = s
            return
        raise McumgrError(f"cannot reach {host}: {last}")

    def close(self):
        self.sock.close()

    def request(self, op, group, cid, body=None):
        """Send one request, return the response's body; refusals and errors raise McumgrError."""
        self.seq = (self.seq + 1) & 0xFF
        frame = pack(op, group, cid, self.seq, body or {})
        if len(frame) > self.payload:
            raise McumgrError(f"request of {len(frame)} bytes exceeds the datagram limit {self.payload}")
        bad = None
        for attempt in range(self.retries):
            self.sock.send(frame)
            deadline = time.monotonic() + self.timeout
            while True:
                try:
                    self.sock.settimeout(max(0.01, deadline - time.monotonic()))
                    rsp = self.sock.recv(65535)
                except (socket.timeout, TimeoutError):
                    break
                except ConnectionRefusedError:
                    raise McumgrError("connection refused (ICMP port unreachable): is the board's MCUmgr up?")
                try:
                    rop, rgrp, rseq, rid, rbody = unpack(rsp)
                except ValueError as e:
                    bad = e
                    continue
                if rseq != self.seq or rgrp != group or rid != cid:
                    continue
                return self.check(rbody, group)
        if bad is not None:
            raise McumgrError(f"undecodable reply: {bad}")
        raise McumgrError(f"no answer from the board after {self.retries} tries (is it up and `mcumgr arm`ed?)")

    @staticmethod
    def check(body, group):
        if isinstance(body, dict):
            if "err" in body:
                e = body["err"]
                raise McumgrError(err_text(e.get("rc", 1), e.get("group", group)))
            if body.get("rc", 0) != 0:
                raise McumgrError(err_text(body["rc"], group))
        return body

    # os
    def echo(self, text):
        return self.request(OP_WRITE, GRP_OS, OS_ECHO, {"d": text}).get("r")

    def reset(self):
        self.request(OP_WRITE, GRP_OS, OS_RESET, {})

    def params(self):
        return self.request(OP_READ, GRP_OS, OS_PARAMS)

    def bootloader(self, query=None):
        """No query: the bootloader's name; "mode": its mode (the board answers no other query)."""
        return self.request(OP_READ, GRP_OS, OS_BOOTLOADER, {"query": query} if query else {})

    # img
    def images(self):
        return self.request(OP_READ, GRP_IMG, IMG_STATE).get("images", [])

    def mark(self, hash_=None, confirm=False):
        body = {"confirm": confirm}
        if hash_ is not None:
            body["hash"] = hash_
        return self.request(OP_WRITE, GRP_IMG, IMG_STATE, body).get("images", [])

    def upload_image(self, image, progress=None, image_no=0):
        sha = hashlib.sha256(image).digest()
        off = 0
        first = {"image": image_no, "len": len(image), "sha": sha}
        stalled = 0
        while off < len(image):
            fields = first if off == 0 else {}
            body, n = upload_chunk(fields, image, off, self.payload)
            rsp = self.request(OP_WRITE, GRP_IMG, IMG_UPLOAD, body)
            # The board's offset: the next chunk after a lost response or a resumed upload
            new = rsp.get("off", off + n)
            stalled = stalled + 1 if new <= off else 0
            if stalled >= 3:
                raise McumgrError("the board does not advance (an upload of its own is open: reset it)")
            off = new
            if progress:
                progress(off, len(image))
        return sha

    # fs
    def fs_put(self, path, data, progress=None):
        off = 0
        first = {"name": path, "len": len(data)}
        if not data:
            self.request(OP_WRITE, GRP_FS, FS_FILE, {**first, "off": 0, "data": b""})
            return
        stalled = 0
        while off < len(data):
            fields = first if off == 0 else {"name": path}
            body, n = upload_chunk(fields, data, off, self.payload)
            rsp = self.request(OP_WRITE, GRP_FS, FS_FILE, body)
            new = rsp.get("off", off + n)
            stalled = stalled + 1 if new <= off else 0
            if stalled >= 3:
                raise McumgrError("the board does not advance (a file transfer of its own is open)")
            off = new
            if progress:
                progress(off, len(data))

    def fs_get(self, path, progress=None):
        out = bytearray()
        total = None
        while total is None or len(out) < total:
            rsp = self.request(OP_READ, GRP_FS, FS_FILE, {"name": path, "off": len(out)})
            if rsp.get("off") != len(out):
                raise McumgrError("file download out of step")
            if total is None:
                total = rsp.get("len", 0)
            chunk = rsp.get("data", b"")
            if not chunk and len(out) < total:
                raise McumgrError("the board returned no data")
            out += chunk
            if progress:
                progress(len(out), total)
        return bytes(out)

    def fs_stat(self, path):
        return self.request(OP_READ, GRP_FS, FS_STAT, {"name": path}).get("len")

    def fs_crc(self, path):
        rsp = self.request(OP_READ, GRP_FS, FS_HASH, {"name": path, "type": "crc32"})
        return rsp.get("output")


DIRTY = 0x8000


def version_text(version):
    """'0.2.0+1234' or '0.2.0.1234' (MCUboot's build number) as git shows it: '0.2.0 (g1a2b3c4-dirty)'.

    The build number is the git short hash (8 hex digits), bit 15 of the
    patch field says the tree had uncommitted changes. An entry of images() (a dict with a
    "version" key) works too."""
    if isinstance(version, dict):
        version = version.get("version")
    parts = str(version).replace("+", ".").split(".")
    if len(parts) != 4 or not all(p.isdigit() for p in parts):
        return str(version)
    patch = int(parts[2])
    base = f"{parts[0]}.{parts[1]}.{patch & (DIRTY - 1)}"
    build = int(parts[3])
    if build == 0:
        return base
    return f"{base} (g{build:08x}{'-dirty' if patch & DIRTY else ''})"


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF
