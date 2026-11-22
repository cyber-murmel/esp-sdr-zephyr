# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/west_commands/mcumgrlib.py: MCUmgr over UDP, the part of `west mcumgr` that needs no board.
The CBOR vectors are RFC 8949 appendix A; the header layout and the group and command numbers are
Zephyr's (include/zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h, subsys/mgmt/mcumgr/grp), not the tool's."""

import hashlib
import socket
import struct
import threading

import pytest
from conftest import load

lib = load("scripts/west_commands/mcumgrlib.py")


@pytest.mark.parametrize("value,hexdump", [
    (0, "00"), (23, "17"), (24, "1818"), (100, "1864"), (1000, "1903e8"), (1000000, "1a000f4240"),
    (-1, "20"), (-100, "3863"), (False, "f4"), (True, "f5"), (None, "f6"),
    (b"", "40"), (b"\x01\x02\x03\x04", "4401020304"), ("", "60"), ("IETF", "6449455446"),
    ([], "80"), ([1, 2, 3], "83010203"), ({}, "a0"), ({1: 2, 3: 4}, "a201020304"),
    ({"a": 1, "b": [2, 3]}, "a26161016162820203"),
])
def test_cbor_vectors(value, hexdump):
    assert lib.cbor_dumps(value).hex() == hexdump
    assert lib.cbor_loads(bytes.fromhex(hexdump)) == value


def test_cbor_rejects_garbage():
    with pytest.raises(ValueError):
        lib.cbor_loads(b"\x83\x01")  # array of 3 with one item
    with pytest.raises(ValueError):
        lib.cbor_loads(b"\x01\x02")  # trailing byte
    with pytest.raises(ValueError):
        lib.cbor_loads(b"\x5f")  # indefinite string without a break
    with pytest.raises(ValueError):
        lib.cbor_loads(b"\xff")  # break outside an item
    with pytest.raises(ValueError):
        lib.cbor_loads(b"\xbf\x61a\xff")  # indefinite map with a key and no value


def test_indefinite_length_items():
    # the board's echo reply (zcbor): header, then bf 61 72 62 "hi" ff
    frame = bytes.fromhex("0300000700000700bf61726268" "69ff")
    assert lib.unpack(frame) == (3, 0, 7, 0, {"r": "hi"})
    # RFC 8949 A: indefinite strings, arrays, maps, and nesting
    assert lib.cbor_loads(bytes.fromhex("7f657374726561646d696e67ff")) == "streaming"
    assert lib.cbor_loads(bytes.fromhex("5f42010243030405ff")) == bytes([1, 2, 3, 4, 5])
    assert lib.cbor_loads(bytes.fromhex("9fff")) == []
    assert lib.cbor_loads(bytes.fromhex("9f018202039f0405ffff")) == [1, [2, 3], [4, 5]]
    assert lib.cbor_loads(bytes.fromhex("bf61610161629f0203ffff")) == {"a": 1, "b": [2, 3]}
    assert lib.cbor_loads(bytes.fromhex("bf6346756ef563416d7421ff")) == {"Fun": True, "Amt": -2}
    assert lib.cbor_loads(bytes.fromhex("826161bf61626163ff")) == ["a", {"b": "c"}]
    # an image list as the board sends it: map in map in array, all indefinite
    body = bytes.fromhex("bf66696d61676573 9f bf 64736c6f7400 6776657273696f6e 65302e322e30 ff ff ff".replace(" ", ""))
    assert lib.cbor_loads(body) == {"images": [{"slot": 0, "version": "0.2.0"}]}


def test_undecodable_reply_is_reported():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))

    def serve():
        data, peer = s.recvfrom(2000)
        # right header, a break where a map should be
        s.sendto(bytes.fromhex("0300000100000700") + b"\xff", peer)

    threading.Thread(target=serve, daemon=True).start()
    c = lib.Client("127.0.0.1", s.getsockname()[1], timeout=0.3, retries=1)
    with pytest.raises(lib.McumgrError, match="undecodable reply"):
        c.echo("x")
    c.close()
    s.close()


def test_header_layout():
    # echo request of the Zephyr docs: op write, group os, id echo, {"d": "hi"}
    frame = lib.pack(lib.OP_WRITE, lib.GRP_OS, lib.OS_ECHO, 7, {"d": "hi"})
    assert frame[:8] == bytes([2, 0, 0, 6, 0, 0, 7, 0])
    assert frame[8:] == lib.cbor_dumps({"d": "hi"})
    op, group, seq, cid, body = lib.unpack(frame)
    assert (op, group, seq, cid, body) == (2, 0, 7, 0, {"d": "hi"})
    # the image group is 1, the file group 8
    assert lib.pack(lib.OP_READ, lib.GRP_IMG, lib.IMG_STATE, 1, {})[:8] == bytes([0, 0, 0, 1, 0, 1, 1, 0])
    assert lib.pack(lib.OP_WRITE, lib.GRP_FS, lib.FS_FILE, 1, {})[4:6] == bytes([0, 8])


def test_unpack_checks_length():
    frame = lib.pack(lib.OP_READ_RSP, 0, 0, 1, {"r": "x"})
    with pytest.raises(ValueError):
        lib.unpack(frame[:-1])
    with pytest.raises(ValueError):
        lib.unpack(frame + b"\x00")
    with pytest.raises(ValueError):
        lib.unpack(b"\x00\x01")


@pytest.mark.parametrize("size", [1, 5, 300, 5000, 4_000_000])
@pytest.mark.parametrize("first", [True, False])
def test_upload_chunks_fit_and_cover(size, first):
    data = bytes(i % 251 for i in range(min(size, 20000)))
    data = data + bytes(size - len(data)) if size > len(data) else data
    fields = {"image": 0, "len": size, "sha": bytes(32)} if first else {}
    off, sent = 0, 0
    while off < len(data) and sent < 6000:
        body, n = lib.upload_chunk(fields, data, off, lib.DEFAULT_PAYLOAD)
        frame = lib.pack(lib.OP_WRITE, lib.GRP_IMG, lib.IMG_UPLOAD, 1, body)
        assert len(frame) <= lib.DEFAULT_PAYLOAD
        assert body["data"] == data[off:off + n] and body["off"] == off
        # a chunk is not left small by the estimate: within a few bytes of the limit unless it is the tail
        if off + n < len(data):
            assert lib.DEFAULT_PAYLOAD - len(frame) < 16
        off += n
        sent += n
        fields = {}
        if off >= len(data) or sent >= 6000:
            break


def signed_image(payload=b"\x11" * 100, hdr=32, protected=False):
    digest = hashlib.sha256(b"fake").digest()
    img = struct.pack("<IIHHIIBBHI", lib.IMAGE_MAGIC, 0, hdr, 0, len(payload), 0, 0, 2, 0, 0)
    img = img.ljust(hdr, b"\0") + payload
    tlvs = struct.pack("<BBH", lib.TLV_SHA256, 0, 32) + digest + struct.pack("<BBH", 0x22, 0, 4) + b"sig!"
    if protected:
        pt = struct.pack("<BBH", 0x50, 0, 4) + b"abcd"
        img += struct.pack("<HH", lib.TLV_PROT_INFO, 4 + len(pt)) + pt
    return img + struct.pack("<HH", lib.TLV_INFO, 4 + len(tlvs)) + tlvs, digest


@pytest.mark.parametrize("protected", [False, True])
def test_image_hash(protected):
    img, digest = signed_image(protected=protected)
    assert lib.image_hash(img) == digest


def test_image_hash_refusals():
    with pytest.raises(lib.McumgrError, match="not an MCUboot image"):
        lib.image_hash(b"\x00" * 64)
    img, _ = signed_image()
    with pytest.raises(lib.McumgrError, match="not signed"):
        lib.image_hash(img[:32 + 100])


def test_image_hash_bounds_a_bad_tlv_area():
    img, _ = signed_image()
    cut = img[:32 + 100]
    for bad in (struct.pack("<HH", lib.TLV_INFO, 0), struct.pack("<HH", lib.TLV_INFO, 3),
                struct.pack("<HHBBH", lib.TLV_INFO, 8, 0x10, 0, 60000)):
        with pytest.raises(lib.McumgrError):
            lib.image_hash(cut + bad)


def test_error_texts():
    assert "mcumgr arm" in lib.err_text(11, 0) and "/FLASH:/" in lib.err_text(11, 8)
    assert "streaming: flash access refused" in lib.err_text(256) and "stream" not in lib.err_text(10)
    assert lib.err_text(99) == "error 99"
    with pytest.raises(lib.McumgrError, match="access denied"):
        lib.Client.check({"rc": 11}, 1)
    with pytest.raises(lib.McumgrError, match="not found"):
        lib.Client.check({"err": {"group": 8, "rc": 5}}, 8)
    assert lib.Client.check({"rc": 0, "off": 3}, 1) == {"rc": 0, "off": 3}


class Board:
    """A UDP server on loopback that answers like Zephyr's groups: echo, image upload and state, files."""

    def __init__(self, armed=True, drop_first=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.armed, self.drop_first = armed, drop_first
        self.image, self.files, self.state = bytearray(), {}, []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        dropped = False
        while True:
            try:
                data, peer = self.sock.recvfrom(65535)
            except OSError:
                return
            op, group, seq, cid, body = lib.unpack(data)
            if self.drop_first and not dropped:
                dropped = True
                continue
            rsp = self.handle(op, group, cid, body)
            self.sock.sendto(lib.pack(op + 1, group, cid, seq, rsp), peer)

    def handle(self, op, group, cid, b):
        if not self.armed:
            return {"rc": 11}
        if (group, cid) == (lib.GRP_OS, lib.OS_ECHO):
            return {"r": b["d"]}
        if (group, cid) == (lib.GRP_IMG, lib.IMG_UPLOAD):
            if b["off"] == 0:
                self.image = bytearray()
                self.total = b["len"]
            if b["off"] != len(self.image):
                return {"rc": 0, "off": len(self.image)}
            self.image += b["data"]
            return {"rc": 0, "off": len(self.image)}
        if (group, cid) == (lib.GRP_IMG, lib.IMG_STATE) and op == lib.OP_WRITE:
            self.state.append(b)
            return {"images": [{"slot": 1, "hash": b.get("hash", b""), "pending": not b["confirm"]}]}
        if (group, cid) == (lib.GRP_IMG, lib.IMG_STATE):
            return {"images": [{"slot": 0, "active": True, "version": "0.2.0"}]}
        if (group, cid) == (lib.GRP_FS, lib.FS_FILE) and op == lib.OP_WRITE:
            buf = self.files.setdefault(b["name"], bytearray()) if b["off"] else bytearray()
            self.files[b["name"]] = buf
            buf += b["data"]
            return {"rc": 0, "off": len(buf)}
        if (group, cid) == (lib.GRP_FS, lib.FS_FILE):
            f = self.files[b["name"]]
            out = {"rc": 0, "off": b["off"], "data": bytes(f[b["off"]:b["off"] + 400])}
            if b["off"] == 0:
                out["len"] = len(f)
            return out
        if (group, cid) == (lib.GRP_FS, lib.FS_STAT):
            return {"rc": 0, "len": len(self.files[b["name"]])}
        if (group, cid) == (lib.GRP_FS, lib.FS_HASH):
            return {"rc": 0, "type": "crc32", "output": lib.crc32(bytes(self.files[b["name"]]))}
        return {"rc": 8}

    def client(self, **kw):
        return lib.Client("127.0.0.1", self.port, **kw)

    def close(self):
        self.sock.close()


@pytest.fixture
def board():
    b = Board()
    yield b
    b.close()


def test_echo_and_refusal():
    for armed in (True, False):
        b = Board(armed)
        c = b.client(timeout=0.5)
        try:
            if armed:
                assert c.echo("hello") == "hello"
            else:
                with pytest.raises(lib.McumgrError, match="mcumgr arm"):
                    c.echo("hello")
        finally:
            c.close()
            b.close()


def test_upload_and_mark(board):
    img, digest = signed_image(payload=bytes(range(256)) * 40)
    c = board.client(timeout=0.5)
    seen = []
    sha = c.upload_image(img, lambda d, t: seen.append(d))
    assert bytes(board.image) == img and sha == hashlib.sha256(img).digest()
    assert seen[-1] == len(img) and seen == sorted(seen)
    c.mark(lib.image_hash(img), confirm=False)
    c.mark(None, confirm=True)
    assert board.state == [{"confirm": False, "hash": digest}, {"confirm": True}]
    assert c.images()[0]["version"] == "0.2.0"
    c.close()


def test_a_lost_request_is_sent_again():
    b = Board(drop_first=True)
    c = b.client(timeout=0.2)
    assert c.echo("again") == "again"
    c.close()
    b.close()


def test_no_answer_says_so():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    c = lib.Client("127.0.0.1", s.getsockname()[1], timeout=0.1, retries=2)
    with pytest.raises(lib.McumgrError, match="no answer"):
        c.echo("x")
    c.close()
    s.close()


def test_files_roundtrip(board):
    data = bytes((i * 7) % 256 for i in range(5000))
    c = board.client(timeout=0.5)
    c.fs_put("/FLASH:/a.llext", data)
    assert bytes(board.files["/FLASH:/a.llext"]) == data
    assert c.fs_stat("/FLASH:/a.llext") == len(data)
    assert c.fs_crc("/FLASH:/a.llext") == lib.crc32(data)
    assert c.fs_get("/FLASH:/a.llext") == data
    c.fs_put("/FLASH:/empty", b"")
    c.close()


def test_resolve_literals():
    assert lib.resolve("127.0.0.1")[0][1] == ("127.0.0.1", 4996)
    assert lib.resolve("::1")[0][1][:2] == ("::1", 4996)
    with pytest.raises(lib.McumgrError, match="mDNS"):
        lib.resolve("no-such-board.invalid.local")


@pytest.mark.parametrize("version,text", [
    ("0.2.0+0", "0.2.0"), ("0.2.0", "0.2.0"), ("0.2.0.0", "0.2.0"),
    ("0.2.0+" + str(0x1A2B3C4), "0.2.0 (g01a2b3c4)"),
    ("0.2.%d.%d" % (0x8000, 0x1A2B3C4), "0.2.0 (g01a2b3c4-dirty)"),
    ("1.0.%d+%d" % (3 + 0x8000, 0xDEADBEEF), "1.0.3 (gdeadbeef-dirty)"),
    ("1.0.3.%d" % 0x0000abc, "1.0.3 (g00000abc)"),
    ("garbage", "garbage"),
])
def test_version_text(version, text):
    assert lib.version_text(version) == text


def test_version_text_takes_an_image_entry():
    assert lib.version_text({"slot": 0, "version": "0.2.32768.1234"}) == lib.version_text("0.2.32768.1234")
    assert lib.version_text({"slot": 0}) == "None"


def _src(path):
    import pathlib
    return (pathlib.Path(__file__).parents[2] / path).read_text()


def _body(src, head):
    b = src[src.index(head):]
    return b[:b.index("\n}\n")]


def test_netsvc_holds_a_transfer_and_frees_the_volume_at_its_end():
    """fs_mgmt keeps its file open between chunks: the claim of a transfer outlives one command (board,
    2026-11-26); a short command, or a transfer's end, gives the volume back at once (the loader that follows
    a `fs put`, which ends with a crc request, must find it free)."""
    src = _src("apps/multiapp/src/netsvc.c")
    cmd_done = _body(src, "static enum mgmt_cb_return cmd_done")
    assert "multiapp_drive_fs_end" not in cmd_done  # the thread that took the claim gives it
    assert "early = xfer_ended || (!cmd_transfer && !xfer_open());" in cmd_done
    assert "hold_release_soon();" in cmd_done
    assert "MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE" in src and "xfer_ended = true;" in _body(src, "static enum mgmt_cb_return file_done")
    assert src.count("multiapp_drive_fs_begin(") == 2 and src.count("multiapp_drive_fs_end(") == 2
    assert "hold_release_soon();" in _body(src, "void multiapp_stream_hold")
    conf = _src("apps/common/net/mcumgr.conf")
    idle = int(conf.split("FILE_AUTOMATIC_IDLE_CLOSE_TIME=")[1].split()[0])
    hold_ms = int(src.split("#define HOLD_IDLE_MS ")[1].split()[0])
    open_ms = int(src.split("#define XFER_OPEN_MS ")[1].split()[0])
    assert idle < open_ms < hold_ms  # fs_mgmt closes its file while the volume is still mounted
    # the loader tries its claim more than once
    assert "tries < 3" in _body(_src("apps/multiapp/src/drive.c"), "static ssize_t read_path")


def test_netsvc_claims_follow_the_command():
    """A put took a read-only mount and failed with -EROFS on the board; a transfer hides the drive (the PC's
    wait for a read claim ends after 10 s with I/O errors), a short command takes a read claim."""
    src = _src("apps/multiapp/src/netsvc.c")
    lock = _body(src, "static enum mgmt_cb_return fs_lock(int32_t *rc, uint8_t id, uint8_t op)\n{")
    assert "cmd_transfer = id == FS_MGMT_ID_FILE;" in lock
    assert "fs_claim(rc, cmd_transfer && op == MGMT_OP_WRITE, cmd_transfer, false)" in lock  # write: only a put
    assert "MGMT_OP_READ" not in lock
    drive = _src("apps/multiapp/src/drive.c")
    begin = _body(drive, "int multiapp_drive_fs_begin")
    assert "hide ? multiapp_drive_write_begin(timeout) : multiapp_drive_read_begin(timeout)" in begin
    assert "fs_mount(write ? &fat_mount_wr : &fat_mount)" in begin
    mounts = drive[:drive.index("/* What the boot's scan found")]
    ro = mounts[mounts.index("fat_mount = {"):]
    wr = mounts[mounts.index("fat_mount_wr = {"):mounts.index("fat_mount = {")]
    assert "READ_ONLY" in ro and "READ_ONLY" not in wr


def test_netsvc_remounts_for_a_new_transfer_but_not_under_an_open_one():
    """A get right after a put, and a get of another file right after a get, failed (rc 1) on a held mount;
    unmounting while fs_mgmt has an abandoned file open loses clusters."""
    src = _src("apps/multiapp/src/netsvc.c")
    claim = _body(src, "static enum mgmt_cb_return fs_claim(int32_t")
    assert "held && (!covers || fresh)" in claim
    assert claim.index("xfer_open()") < claim.index("drop_work")  # no drop while a transfer may be open
    assert claim.index("drop_work") < claim.index("acquire_work")  # dropped first, then claimed afresh
    assert "K_FOREVER" in claim  # the MCUmgr thread waits for each step: fs_mgmt opens on a settled mount
    assert "MGMT_ERR_EBUSY" in claim
    fresh = _body(src, "static enum mgmt_cb_return fs_fresh(int32_t *rc, enum fs_mgmt_file_access_types access)\n{")
    assert "fs_claim(rc, access == FS_MGMT_FILE_ACCESS_WRITE, true, true)" in fresh
    # reads and writes reach it; a stat or a hash keeps what fs_lock() gave it
    assert "access != FS_MGMT_FILE_ACCESS_READ && access != FS_MGMT_FILE_ACCESS_WRITE" in fresh
    hook = _body(src, "static enum mgmt_cb_return file_access(uint32_t")
    assert "return fs_fresh(rc, fa->access);" in hook
    drop = _body(src, "static void drop_fn")
    assert "multiapp_drive_fs_end(held_write, held_hide)" in drop and "held = false" in drop


@pytest.fixture(scope="module")
def netpath(tmp_path_factory):
    import ctypes
    import shutil
    import subprocess
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    tmp = tmp_path_factory.mktemp("np")
    (tmp / "np.c").write_text(
        '#include "netpath.h"\n'
        "int np_ok(const char *p){return netpath_ok(p);}\n"
        "int np_ext(const char *p){return netpath_is_extension(p);}\n")
    so = tmp / "np.so"
    inc = __import__("pathlib").Path(__file__).parents[2] / "apps/multiapp/src"
    r = subprocess.run([cc, "-shared", "-fPIC", "-O1", "-Wall", "-o", str(so), str(tmp / "np.c"), f"-I{inc}"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip(r.stderr[-300:])
    c = ctypes.CDLL(str(so))
    c.np_ok.argtypes = c.np_ext.argtypes = [ctypes.c_char_p]
    return c


@pytest.mark.parametrize("path,ok", [
    ("/FLASH:/hello.llext", True), ("/FLASH:/a/b.bin", True), ("/FLASH:/..x", True), ("/FLASH:/a..b", True),
    ("/FLASH:/x..", True), ("/FLASH:/", True),
    ("/FLASH:/..", False), ("/FLASH:/../x", False), ("/FLASH:/a/../b", False), ("/FLASH:/a/..", False),
    ("/FLASH:a", False), ("/FLASH:", False), ("/flash:/a", False), ("/etc/passwd", False), ("", False),
    ("/FLASH:/a\\b", False), ("FLASH:/a", False), ("/SD:/a", False),
])
def test_netpath_ok(netpath, path, ok):
    assert bool(netpath.np_ok(path.encode())) is ok


@pytest.mark.parametrize("path,ext", [
    ("/FLASH:/a.llext", True), ("/FLASH:/A.LLEXT", True), ("/FLASH:/a.LlExT", True), ("/FLASH:/a.llext.", True),
    ("/FLASH:/a.llext ", True), ("/FLASH:/a.llext. . ", True), ("/FLASH:/HELLO~1.LLE", True), ("/FLASH:/a~b", True),
    ("/FLASH:/.llext", True),
    ("/FLASH:/a.llex", False), ("/FLASH:/a.llextx", False), ("/FLASH:/llext", False), ("/FLASH:/a.bin", False),
    ("/FLASH:/a.llext.bin", False), ("", False),
])
def test_netpath_is_extension(netpath, path, ext):
    assert bool(netpath.np_ext(path.encode())) is ext
