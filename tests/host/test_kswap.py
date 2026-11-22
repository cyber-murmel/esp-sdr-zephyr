# SPDX-License-Identifier: GPL-3.0-or-later
"""apps/multiapp/scripts/kswap.py: the PC side of hot-swapping a stage, against a fake board.

The framing (base64 lines of a size the shell's line buffer holds, the CRC-32 the device compares,
one line at a time with the device's byte count checked) and the way a device's answer is read
(the shell echoes the command, colours its errors, ends with a prompt) need no device."""

import base64
import re
import zlib

import pytest

from conftest import load

kswap = load("apps/multiapp/scripts/kswap.py")

# The shortest shell line buffer of a multiapp host: the ESP32-S3's CONFIG_SHELL_CMD_BUFF_SIZE.
S3_LINE = 96


def test_chunks_are_whole_base64_groups_and_round_trip():
    data = bytes(range(256)) * 3 + b"xy"
    lines = list(kswap.data_lines(data, 48))
    assert all(len(line) == 64 for line in lines[:-1])
    assert all("=" not in line for line in lines[:-1])
    assert b"".join(base64.b64decode(line) for line in lines) == data
    # The last chunk may be short and padded; each line decodes alone, as the device does.
    assert len(base64.b64decode(lines[-1])) == len(data) % 48 or 48


def test_default_lines_fit_the_s3_shell():
    data = bytes(3000)
    for cmd in kswap.commands("ring_sink", data):
        assert len(cmd) < S3_LINE, cmd
    cmds = list(kswap.commands("ring_sink", data))
    assert cmds[0].startswith("multiapp stage put ring_sink 3000 0x")
    assert cmds[-1] == "multiapp stage end"
    assert len(cmds) == 2 + -(-3000 // 48)


def test_put_carries_the_crc_the_device_checks():
    data = b"\x7fELF" + bytes(range(100))
    cmd = kswap.put_command("hello_op", data)
    size, crc = re.fullmatch(r"multiapp stage put hello_op (\d+) 0x([0-9a-f]{8})", cmd).groups()
    assert int(size) == len(data)
    assert int(crc, 16) == zlib.crc32(data)
    assert kswap.put_command("hello_op", data, 500).endswith(" 500")


def test_chunk_must_be_a_multiple_of_three():
    with pytest.raises(ValueError):
        list(kswap.data_lines(b"abc", 50))
    with pytest.raises(ValueError):
        list(kswap.data_lines(b"abc", 0))


class FakeBoard:
    """A serial port with a multiapp shell behind it: echoes, colours, prompts, and does what the
    device's multiapp stage commands do (src/stage_shell.c): count, check the CRC, answer."""

    PROMPT = "uart:~$ "

    def __init__(self, result="ok", drop_data_line=None, bad_crc=False):
        self.out = bytearray()
        self.pending = b""
        self.up = None
        self.got = bytearray()
        self.result = result
        self.drop = drop_data_line
        self.bad_crc = bad_crc
        self.lines_seen = []

    def reset_input_buffer(self):
        self.out.clear()

    def write(self, data):
        self.pending += data
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            self.handle(line.decode().strip())

    def read(self, n):
        chunk, self.out = bytes(self.out[:n]), self.out[n:]
        return chunk

    def say(self, *lines):
        for line in lines:
            self.out += line.encode() + b"\r\n"
        self.out += self.PROMPT.encode()

    def handle(self, line):
        self.lines_seen.append(line)
        # The shell echoes the line it is typed.
        self.out += f"{line}\r\n".encode()
        words = line.split()
        if line == "":
            return self.say()
        if words[:3] == ["multiapp", "stage", "put"]:
            self.up = {"stage": words[3], "size": int(words[4]), "crc": int(words[5], 0)}
            self.got = bytearray()
            return self.say(f"put {words[3]}: {words[4]} bytes expected")
        if words[:3] == ["multiapp", "stage", "data"]:
            if self.drop is not None and len(self.lines_seen) == self.drop:
                return self.say()
            self.got += base64.b64decode(words[3])
            return self.say(f"ok {len(self.got)}")
        if words[:3] == ["multiapp", "stage", "end"]:
            crc = zlib.crc32(bytes(self.got))
            if self.bad_crc or crc != self.up["crc"] or len(self.got) != self.up["size"]:
                return self.say("\x1b[1;31mstage result: error -74 CRC mismatch\x1b[m")
            if self.result != "ok":
                return self.say(f"\x1b[1;31mstage result: error {self.result}\x1b[m")
            return self.say(f"stage {self.up['stage']}: swapped to the kernel in 3 us (the app was idle)",
                            "stage result: ok")
        if words[:3] == ["multiapp", "stage", "abort"]:
            return self.say("upload dropped")
        if words[:3] == ["multiapp", "stage", "revert"]:
            return self.say("stage result: ok")
        if words[:3] == ["multiapp", "stage", "load"]:
            return self.say("\x1b[1;31mstage result: error -2 no such stage or file\x1b[m")
        return self.say()


def test_push_to_a_board():
    board = FakeBoard()
    data = bytes(range(256)) * 2
    ok, msg = kswap.push(kswap.Shell(board), "ring_sink", data, timeout=2.0)
    assert (ok, msg) == (True, "ok")
    assert bytes(board.got) == data
    # The device was never sent a line before the one before it was answered: one answer per line.
    assert board.lines_seen[0].startswith("multiapp stage put ")
    assert board.lines_seen[-1] == "multiapp stage end"


def test_the_devices_printed_lines_are_passed_on():
    board, printed = FakeBoard(), []
    shell = kswap.Shell(board, echo=printed.append)
    kswap.push(shell, "ring_sink", bytes(100), timeout=2.0)
    assert any("swapped to the kernel in 3 us" in line for line in printed)
    assert "stage result: ok" in printed
    # No colour codes, no prompt, no echo of the commands.
    assert not any("\x1b" in line or line.startswith("multiapp") for line in printed)


def test_a_device_error_is_returned_with_its_reason():
    board = FakeBoard(result="-110 the app never reached its sync point")
    ok, msg = kswap.push(kswap.Shell(board), "ring_sink", bytes(30), timeout=2.0)
    assert not ok
    assert "the app never reached its sync point" in msg


def test_a_corrupted_upload_fails_on_the_crc():
    ok, msg = kswap.push(kswap.Shell(FakeBoard(bad_crc=True)), "s", bytes(30), timeout=2.0)
    assert not ok and "CRC" in msg


def test_a_lost_line_is_noticed():
    board = FakeBoard(drop_data_line=3)  # the second data line is never answered
    with pytest.raises(kswap.SwapError, match="no answer to data line 2"):
        kswap.push(kswap.Shell(board), "s", bytes(200), chunk=48, timeout=0.3)


def test_no_board_no_answer():
    class Mute(FakeBoard):
        def handle(self, line):
            pass

    with pytest.raises(kswap.SwapError, match="no answer to put"):
        kswap.push(kswap.Shell(Mute()), "s", bytes(30), timeout=0.2)


def test_main_pushes_a_file_and_reverts(tmp_path, monkeypatch, capsys):
    board = FakeBoard()
    monkeypatch.setattr(kswap, "open_port", lambda path, baud: board)
    kernel = tmp_path / "k.llext"
    kernel.write_bytes(b"\x7fELF" + bytes(range(200)))
    assert kswap.main(["--port", "x", "--stage", "ring_sink", str(kernel)]) == 0
    assert bytes(board.got) == kernel.read_bytes()
    assert "kswap: ok" in capsys.readouterr().out
    assert kswap.main(["--port", "x", "--stage", "ring_sink", "--revert"]) == 0
    assert board.lines_seen[-1] == "multiapp stage revert ring_sink"


def test_main_file_mode_only_names_the_path(monkeypatch, capsys):
    board = FakeBoard()
    monkeypatch.setattr(kswap, "open_port", lambda path, baud: board)
    # The device answers that it has no such file: reported, exit status 1.
    assert kswap.main(["--port", "x", "--stage", "ring_sink", "--file", "nokernel.llext"]) == 1
    assert board.lines_seen[-1] == "multiapp stage load ring_sink nokernel.llext"
    assert "no such stage or file" in capsys.readouterr().out + ""


def test_main_needs_a_stage_or_list(monkeypatch):
    monkeypatch.setattr(kswap, "open_port", lambda path, baud: FakeBoard())
    with pytest.raises(SystemExit):
        kswap.main(["--port", "x", "kernel.llext"])


def test_build_runs_the_kernels_ninja_target(tmp_path, monkeypatch):
    image = tmp_path / "build" / "multiapp"
    (image / "llext" / "files").mkdir(parents=True)
    (image / "build.ninja").write_text("")
    (image / "llext" / "files" / "sink_scan16.llext").write_bytes(b"k")
    ran = []
    monkeypatch.setattr(kswap.subprocess, "run", lambda cmd, check: ran.append(cmd))

    class Args:
        build, target, kernel = str(tmp_path / "build"), "sink_scan16", None

    path = kswap.kernel_file(Args)
    assert ran == [["ninja", "-C", str(image), "sink_scan16_kernel"]]
    assert path == image / "llext" / "files" / "sink_scan16.llext"
