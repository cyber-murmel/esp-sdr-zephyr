# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/wpan/tools/wpan_sniffer.py (Wireshark extcap of the wpan app's sniffer mode).

The FCS against a frame with its real checksum, the TAP header's layout, the parser on shell
output (prompt and VT100 codes ahead of the line), the extcap listing on a faked port list (CI
has no boards, and the tool imports without pyserial), the raw port class on a pty pair, and a
whole capture against a fake board on a pty (it answers the sniffer commands like the firmware,
prompt noise included): the pcap it writes is read back by a reader written here, and by tshark
where that is installed."""

import os
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
import types

import pytest

from conftest import REPO, load

TOOL = REPO / "apps/wpan/tools/wpan_sniffer.py"
ws = load("apps/wpan/tools/wpan_sniffer.py")

linux_only = pytest.mark.skipif(not sys.platform.startswith("linux"), reason="pty and termios")


def kermit(data):
    """CRC-16/KERMIT, 802.15.4's FCS, the other way round than the tool: polynomial 0x1021
    shifted left over bit-reversed bytes, the result bit-reversed."""
    def rev(value, bits):
        return int(format(value, f"0{bits}b")[::-1], 2)

    crc = 0
    for byte in data:
        crc ^= rev(byte, 8) << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return struct.pack("<H", rev(crc, 16))


# An Imm-ACK: frame control 0x0002 (type ack), sequence number 0x5a.
ACK = bytes([0x02, 0x00, 0x5A])


def test_fcs_known_frames():
    # The catalogue's check value of CRC-16/KERMIT.
    assert ws.fcs(b"123456789") == bytes([0x89, 0x21])
    assert kermit(b"123456789") == bytes([0x89, 0x21])
    assert ws.fcs(b"") == b"\0\0"
    assert ws.fcs(ACK) == kermit(ACK)
    # The ACK's two FCS bytes, as they go on the air, and the whole frame checks to zero.
    assert ws.fcs(ACK) == bytes([0x67, 0x48])
    assert ws.fcs(ACK + ws.fcs(ACK)) == b"\0\0"
    data = bytes(range(100))
    assert ws.fcs(data) == kermit(data)


def parse_tap(blob):
    """(version, length, {type: value bytes}, rest) of an IEEE 802.15.4 TAP header."""
    version, _, length = struct.unpack_from("<BBH", blob)
    tlvs, off = {}, 4
    while off < length:
        typ, ln = struct.unpack_from("<HH", blob, off)
        tlvs[typ] = blob[off + 4:off + 4 + ln]
        off += 4 + ((ln + 3) & ~3)
    assert off == length
    return version, length, tlvs, blob[length:]


def test_tap_header_layout():
    hdr = ws.tap_header(15, -61, 200)
    version, length, tlvs, rest = parse_tap(hdr)
    assert (version, length, rest) == (0, len(hdr), b"")
    assert length % 4 == 0
    assert list(tlvs) == [0, 1, 3, 10]  # the order the spec's dissector likes: ascending
    assert tlvs[0] == b"\x01"  # FCS type: CRC-16
    assert struct.unpack("<f", tlvs[1]) == (-61.0,)
    assert struct.unpack("<HBB", tlvs[3] + b"\0")[0] == 15 and len(tlvs[3]) == 3
    assert tlvs[10] == bytes([200])
    # FCS type TLV bytes, exactly: type 0, length 1, value padded to 4.
    assert hdr[4:12] == struct.pack("<HHI", 0, 1, 1)
    # RSS: type 1, length 4, a float.
    assert hdr[12:16] == struct.pack("<HH", 1, 4)
    # Header length counts the 4 byte header and every TLV with its padding: 4+8+8+8+8.
    assert length == 36


def test_tap_header_without_rss():
    hdr = ws.tap_header(25, ws.RSSI_UNDEFINED, 7)
    _, length, tlvs, _ = parse_tap(hdr)
    assert list(tlvs) == [0, 3, 10]
    assert length == 28 == len(hdr)
    assert ws.RSSI_UNDEFINED == -32768


@pytest.mark.parametrize("prefix", [
    "",
    "uart:~$ ",
    "\x1b[1;32muart:~$ \x1b[m",
    "\x1b[2K\r\x1b[1;32muart:~$ \x1b[m\x1b[8D",
    "\x1b[J\x1b[1;32muart:~$ \x1b[m ",
])
def test_frame_re_on_shell_output(prefix):
    line = prefix + "psdu: 0200ab power: -47 lqi: 255 time: 123456789012"
    m = ws.FRAME_RE.search(line)
    assert m is not None
    assert m.groups() == ("0200ab", "-47", "255", "123456789012")
    m = ws.FRAME_RE.search(prefix + "psdu: 02 power: -32768 lqi: 0 time: 0")
    assert m.group(2) == "-32768"


@pytest.mark.parametrize("line", ["", "uart:~$ ", "started", "enq=3 drop=0 tx_rec=3 q_hwm=1",
                                  "psdu:  power: 1 lqi: 2 time: 3"])
def test_frame_re_ignores_other_lines(line):
    assert ws.FRAME_RE.search(line) is None


def test_record_has_tap_and_correct_fcs():
    cap = ws.Capture("dev", 115200, 0, 15, True, ws.Log(None))
    m = ws.FRAME_RE.search("uart:~$ psdu: 02005a power: -32768 lqi: 9 time: 5000000")
    rec = cap._record(m)
    ts_s, ts_us, incl, orig = struct.unpack_from("<IIII", rec)
    assert incl == orig == len(rec) - 16
    body = rec[16:]
    _, _, tlvs, frame = parse_tap(body)
    assert 1 not in tlvs and tlvs[10][0] == 9
    assert frame == ACK + bytes([0x67, 0x48])
    assert ts_s * 1_000_000 + ts_us == pytest.approx(time.time() * 1e6, abs=30e6)  # wall clock


def port(device, vid, pid, product, serial):
    return types.SimpleNamespace(device=device, vid=vid, pid=pid, product=product,
                                 serial_number=serial, description="x")


def fake_ports(monkeypatch, tmp_path, ports):
    monkeypatch.setattr(ws, "comports", lambda: ports)
    byid = tmp_path / "by-id"
    byid.mkdir()
    monkeypatch.setattr(ws, "BY_ID_DIR", str(byid))
    return byid


def test_extcap_interfaces(monkeypatch, tmp_path, run_main, capsys):
    node = tmp_path / "ttyACM1"
    node.write_text("")
    byid = fake_ports(monkeypatch, tmp_path, [
        port("/dev/ttyS0", None, None, None, None),
        port(str(node), 0x2FE3, 0x0005, "ESP-SDR 802.15.4", "A1B2"),
        port("/dev/ttyACM7", 0x303A, 0x1001, "USB JTAG/serial debug unit", "40:4C:CA:00:11:22"),
        port("/dev/ttyACM8", 0x2FE3, 0x0001, "USBD sample", "X"),  # a Zephyr port of something else
        port("/dev/ttyUSB0", 0x10C4, 0xEA60, "CP2102", "0001"),
    ])
    (byid / "usb-Zephyr_Project_ESP-SDR_802.15.4_A1B2-if00").symlink_to(node)
    assert run_main(ws, "--extcap-interfaces") == 0
    out = capsys.readouterr().out.splitlines()
    assert out[0].startswith("extcap {version=")
    assert out[1:] == [
        "interface {value=/dev/ttyACM7}"
        "{display=wpan sniffer, USB-Serial-JTAG (40:4C:CA:00:11:22)}",
        "interface {value=" + str(byid / "usb-Zephyr_Project_ESP-SDR_802.15.4_A1B2-if00") + "}"
        "{display=wpan sniffer, ESP32-S3 software radio (A1B2)}",
    ]


def test_extcap_dlts_and_config(run_main, capsys):
    assert run_main(ws, "--extcap-dlts") == 0
    assert "{number=283}" in capsys.readouterr().out
    assert run_main(ws, "--extcap-config") == 0
    cfg = capsys.readouterr().out.splitlines()
    chan = next(line for line in cfg if "{call=--channel}" in line)
    assert "{default=11}" in chan and "{range=11,26}" in chan
    speed = next(line for line in cfg if "{call=--capture-baudrate}" in line)
    assert "{default=0}" in speed
    assert any("{call=--log-file}" in line for line in cfg)


def test_imports_without_pyserial():
    code = ("import sys; sys.modules['serial'] = None; sys.modules['serial.tools'] = None; "
            "sys.modules['serial.tools.list_ports'] = None; sys.argv = ['x', '--extcap-dlts']; "
            "import runpy; runpy.run_path(%r, run_name='__main__')" % str(TOOL))
    res = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert "{number=283}" in res.stdout, res.stderr


@linux_only
def test_raw_serial_on_a_pty():
    import termios
    master, slave = os.openpty()
    name = os.ttyname(slave)
    p = ws.RawSerial(name, 115200, timeout=0.05)
    try:
        attrs = termios.tcgetattr(p.fd)
        assert attrs[0] == attrs[1] == attrs[3] == 0
        assert attrs[2] & termios.CLOCAL and attrs[2] & termios.CREAD
        assert not attrs[2] & termios.HUPCL
        assert p.baudrate == 115200 and attrs[4] == termios.B115200

        t0 = time.monotonic()
        assert p.read(10) == b"" and 0.03 < time.monotonic() - t0 < 1  # the timeout
        assert p.in_waiting == 0

        os.write(master, b"hello\r\n")
        deadline = time.monotonic() + 2
        while p.in_waiting < 7 and time.monotonic() < deadline:
            time.sleep(0.01)
        assert p.in_waiting == 7  # raw: nothing translated
        assert p.read(3) == b"hel"
        assert p.read(100) == b"lo\r\n"

        os.write(master, b"junk")
        time.sleep(0.05)
        p.reset_input_buffer()
        assert p.in_waiting == 0

        assert p.write(b"sniffer start\r\n") == 15
        p.flush()
        assert os.read(master, 100) == b"sniffer start\r\n"  # no echo either

        p.baudrate = 921600
        assert p.baudrate == 921600
        assert termios.tcgetattr(p.fd)[5] == termios.B921600
        with pytest.raises(ValueError):
            p.baudrate = 12345

        # A second open of the same tty is refused (TIOCEXCL).
        with pytest.raises(OSError):
            os.close(ws.RawSerial(name, 115200).fd)
    finally:
        p.close()
        p.close()  # idempotent
        os.close(master)
        os.close(slave)


@linux_only
def test_raw_serial_errors():
    with pytest.raises(ws.SerialException):
        ws.RawSerial("/nonexistent/tty", 115200)


# A frame list for the fake board: (psdu without FCS, power, lqi, time in us since boot).
FRAMES = [
    (bytes([0x02, 0x00, seq]), -32768 if seq % 2 else -60 - seq, 100 + seq, 1_000_000 + 5_000 * seq)
    for seq in range(1, 9)
] + [(bytes(range(1, 126)), -80, 255, 9_000_000)]  # 125 bytes: with the FCS a full 127 byte frame


class FakeBoard(threading.Thread):
    """The firmware's side of a pty: the shell with the sniffer commands, VT100 noise included."""

    PROMPT = "\x1b[1;32muart:~$ \x1b[m"

    def __init__(self, master):
        super().__init__(daemon=True)
        self.master = master
        self.commands = []
        self.running = False
        self.channel = 99
        self.stop_event = threading.Event()
        self.sent = 0
        self.speed = 115200
        self.buf = b""

    def out(self, text):
        os.write(self.master, text.encode())

    def hangup(self):
        """The board goes away: the master end of the pty closes."""
        self.stop_event.set()
        self.join(2)
        os.close(self.master)
        self.master = None

    def reply(self, text):
        self.out(text + "\r\n" + self.PROMPT)

    def frame_line(self, psdu, power, lqi, us):
        return f"psdu: {psdu.hex()} power: {power} lqi: {lqi} time: {us}\r\n"

    def traffic(self):
        for frame in FRAMES:
            if self.stop_event.is_set() or not self.running:
                return
            # The prompt redraw the shell does after each line, ahead of the next one.
            try:
                self.out(self.PROMPT + self.frame_line(*frame))
            except OSError:
                return
            self.sent += 1

    def handle(self, cmd):
        self.commands.append(cmd)
        if cmd == "":
            self.out("\r\n" + self.PROMPT)
        elif cmd == "sniffer start":
            self.reply("already started" if self.running else "started")
            self.running = True
        elif cmd == "sniffer stop":
            self.reply("already stopped" if not self.running else "stopped")
            self.running = False
        elif cmd.startswith("sniffer channel "):
            if self.running:
                self.out("\x1b[1;31mcannot change channel while running\x1b[m\r\n" + self.PROMPT)
            else:
                self.channel = int(cmd.split()[2])
                self.reply(f"channel {self.channel}")
        elif cmd == "sniffer stats":
            self.reply(f"enq={self.sent} drop=0 tx_rec={self.sent} tx_bytes=1 q_hwm=2 q_used=0/16")
            if self.running and not self.sent:
                # The tool drops what arrives while it configures the board (as upstream's does),
                # so the traffic starts a little later.
                threading.Timer(0.3, self.traffic).start()
        elif cmd.startswith("sniffer speed"):
            if len(cmd.split()) > 2:
                self.speed = int(cmd.split()[2])
            self.reply(f"speed {self.speed}")
        elif cmd == "shell echo off":
            self.out(self.PROMPT)
        else:
            self.reply("unknown")

    def run(self):
        import select
        while not self.stop_event.is_set():
            ready, _, _ = select.select([self.master], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(self.master, 1024)
            except OSError:
                return
            self.buf += data
            while b"\n" in self.buf:
                line, _, self.buf = self.buf.partition(b"\n")
                self.handle(line.decode(errors="replace").strip("\r "))


def read_pcap(path):
    """(link type, [(sec, usec, bytes)]) of a classic pcap file."""
    blob = open(path, "rb").read()
    magic, vmaj, vmin, _, _, snaplen, link = struct.unpack_from("<IHHiIII", blob)
    assert (magic, vmaj, vmin) == (0xA1B2C3D4, 2, 4)
    records, off = [], 24
    while off < len(blob):
        sec, usec, incl, orig = struct.unpack_from("<IIII", blob, off)
        assert incl == orig <= snaplen and off + 16 + incl <= len(blob)
        records.append((sec, usec, blob[off + 16:off + 16 + incl]))
        off += 16 + incl
    assert off == len(blob)
    return link, records


@pytest.fixture
def board():
    master, slave = os.openpty()
    b = FakeBoard(master)
    b.slave_name = os.ttyname(slave)
    b.start()
    yield b
    b.stop_event.set()
    b.join(2)
    if b.master is not None:
        os.close(master)
    os.close(slave)


def capture(board, tmp_path, *args, ending="sigterm"):
    out = tmp_path / "cap.pcap"
    log = tmp_path / "cap.log"
    proc = subprocess.Popen([sys.executable, str(TOOL), "--capture", "--extcap-interface",
                             board.slave_name, "--fifo", str(out), "--log-file", str(log), *args],
                            stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and proc.poll() is None:
            if out.exists() and out.stat().st_size >= 24:
                try:
                    if len(read_pcap(out)[1]) >= len(FRAMES):
                        break
                except (AssertionError, struct.error):
                    pass  # a record half written
            time.sleep(0.05)
        if ending == "sigterm":
            proc.send_signal(signal.SIGTERM)
        else:
            board.hangup()
        err = proc.communicate(timeout=10)[1]
    finally:
        if proc.poll() is None:
            proc.kill()
    return out, log, proc.returncode, err


@linux_only
def test_capture_against_a_fake_board(board, tmp_path):
    out, log, rc, err = capture(board, tmp_path)
    assert rc == 0, err
    link, records = read_pcap(out)
    assert link == 283
    assert len(records) == len(FRAMES)
    stamps = []
    for (sec, usec, body), (psdu, power, lqi, us) in zip(records, FRAMES):
        _, _, tlvs, frame = parse_tap(body)
        assert frame == psdu + kermit(psdu)  # the FCS appended, and right
        assert tlvs[0] == b"\x01"
        assert tlvs[3][:2] == struct.pack("<H", 11)  # the default channel
        assert tlvs[10][0] == lqi
        if power == -32768:
            assert 1 not in tlvs
        else:
            assert struct.unpack("<f", tlvs[1]) == (float(power),)
        stamps.append(sec * 1_000_000 + usec)
    # The device clock's spacing carries over to the capture.
    assert [b - a for a, b in zip(stamps, stamps[1:])] == [5000] * 7 + [9_000_000 - 1_040_000]
    assert abs(stamps[0] - time.time() * 1e6) < 30e6
    # What the tool said to the board: quiet shell, stopped, channel, start, stats; none of the
    # speed dance (capture baud rate 0), and stopped again on the way out.
    cmds = [c for c in board.commands if c]
    assert cmds[:5] == ["shell echo off", "sniffer stop", "sniffer channel 11", "sniffer start",
                        "sniffer stats"]
    assert cmds[-1] == "sniffer stop" and not board.running
    assert not any("speed" in c for c in cmds)
    assert "frame lines" in log.read_text()


@linux_only
def test_capture_channel_option(board, tmp_path):
    out, log, rc, err = capture(board, tmp_path, "--channel", "26")
    assert rc == 0, err
    assert "sniffer channel 26" in board.commands and board.channel == 26
    _, records = read_pcap(out)
    assert parse_tap(records[0][2])[2][3][:2] == struct.pack("<H", 26)


@linux_only
def test_capture_speed_dance_when_asked(board, tmp_path):
    out, log, rc, err = capture(board, tmp_path, "--capture-baudrate", "921600")
    assert rc == 0, err
    assert "sniffer speed 921600" in board.commands
    assert board.commands[-1] == "sniffer speed 115200"  # put back after the stop
    assert len(read_pcap(out)[1]) == len(FRAMES)


@linux_only
def test_capture_ends_when_the_board_goes(board, tmp_path):
    out, log, rc, err = capture(board, tmp_path, ending="hangup")
    assert rc != 0 and "read failed" in err or "write failed" in err
    assert len(read_pcap(out)[1]) == len(FRAMES)  # what came in is all in the file


@linux_only
def test_capture_refuses_a_missing_port(tmp_path):
    res = subprocess.run([sys.executable, str(TOOL), "--capture", "--extcap-interface",
                          "/nonexistent/tty", "--fifo", str(tmp_path / "x.pcap")],
                         capture_output=True, text=True, timeout=20)
    assert res.returncode != 0 and "/nonexistent/tty" in res.stderr


@linux_only
@pytest.mark.skipif(shutil.which("tshark") is None, reason="tshark not installed")
def test_tshark_reads_the_capture(board, tmp_path):
    out, log, rc, err = capture(board, tmp_path)
    assert rc == 0, err
    res = subprocess.run(["tshark", "-r", str(out), "-T", "fields", "-e", "wpan.fcs_ok",
                          "-e", "wpan.frame_type", "-e", "wpan.seq_no"],
                         capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, res.stderr
    rows = [line.split("\t") for line in res.stdout.splitlines()]
    assert len(rows) == len(FRAMES)
    assert all(r[0] == "True" for r in rows), rows  # every FCS checks
    assert [r[2] for r in rows[:3]] == ["1", "2", "3"]


@linux_only
def test_probe_accepts_a_prompt_without_newline(board, tmp_path):
    # On a quiet channel the shell answers a bare newline with its prompt and nothing after it.
    out, log, rc, err = capture(board, tmp_path, ending="hangup")
    text = log.read_text()
    assert "nothing readable" not in text
    assert text.count("open at") == 1
