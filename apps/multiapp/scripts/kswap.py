#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Swap a stage of a running multiapp app for a kernel (doc/apps/multiapp-internals.md, "Hot-swapping a stage").

    kswap.py --port /dev/ttyACM0 --stage ring_sink --build build/hotswap --target sink_scan16
    kswap.py --port /dev/ttyACM0 --stage ring_sink path/to/sink_scan16.llext
    kswap.py --port /dev/ttyACM0 --stage ring_sink --file sink_scan16.llext   # a file of the volume
    kswap.py --port /dev/ttyACM0 --stage ring_sink --revert
    kswap.py --port /dev/ttyACM0 --list

The kernel file is pushed over the device's shell as base64 lines (multiapp stage put, data, end),
one at a time, each answered "ok <bytes so far>" before the next goes out, so that the shell's
small input buffers never overflow (UART or CDC-ACM alike). The device loads it, swaps the stage
at the app's quiescent point and prints a line "stage result: ok" or "stage result: error <errno> <why>".
This tool prints every line the device answers, and exits 1 on an error.

--build takes a sysbuild directory (or the multiapp image's own) and, with --target, runs
ninja for the kernel's target (<target>_kernel) first; the file is <image>/llext/files/<target>.llext.
"""

import argparse
import base64
import re
import subprocess
import sys
import time
import zlib
from pathlib import Path

# The shell's prompt, which ends a command's output; set by the board's CONFIG_SHELL_PROMPT_UART.
PROMPT = "uart:~$"
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
# A shell line is at most CONFIG_SHELL_CMD_BUFF_SIZE (96 on the ESP32-S3, 256 on the ESP32-S31)
# including "multiapp stage data " (20 characters) and its terminator: 48 bytes are 64 characters.
DEFAULT_CHUNK = 48
RESULT = re.compile(r"stage result: (ok|error (-?\d+) (.*))")


def crc32(data):
    """The CRC-32 the device computes (crc32_ieee(), zlib's)."""
    return zlib.crc32(data) & 0xFFFFFFFF


def data_lines(data, chunk=DEFAULT_CHUNK):
    """The base64 text of each chunk bytes of data (a multiple of 3 keeps '=' off all but the last)."""
    if chunk < 3 or chunk % 3:
        raise ValueError("the chunk is a multiple of 3 bytes")
    for off in range(0, len(data), chunk):
        yield base64.b64encode(data[off:off + chunk]).decode()


def put_command(stage, data, timeout_ms=None, name=None):
    """name: what a signed kernel was signed for (apptrail.py --name); a timeout of 0 is the default."""
    cmd = f"multiapp stage put {stage} {len(data)} 0x{crc32(data):08x}"
    if name:
        timeout_ms = timeout_ms or 0
    cmd = cmd if timeout_ms is None else f"{cmd} {timeout_ms}"
    return f"{cmd} {name}" if name else cmd


def commands(stage, data, chunk=DEFAULT_CHUNK, timeout_ms=None, name=None):
    """Every shell line of one push: put, the data lines, end."""
    yield put_command(stage, data, timeout_ms, name)
    for line in data_lines(data, chunk):
        yield f"multiapp stage data {line}"
    yield "multiapp stage end"


def clean(raw):
    """The text of a device read: no escape sequences, no carriage returns."""
    return ANSI.sub("", raw.decode(errors="replace")).replace("\r", "")


class Shell:
    """A shell on a serial-like object (write(bytes), read(n) with a timeout, as pyserial's)."""

    def __init__(self, port, char_delay=0.0, echo=None):
        self.port = port
        self.char_delay = char_delay
        self.echo = echo or (lambda line: None)
        self.buf = ""

    def send(self, line):
        data = line.encode() + b"\r\n"
        if self.char_delay:
            for i in range(len(data)):
                self.port.write(data[i:i + 1])
                time.sleep(self.char_delay)
        else:
            self.port.write(data)

    def lines(self, timeout):
        """The device's lines as they complete, until the timeout since the last byte."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            raw = self.port.read(4096)
            if raw:
                end = time.monotonic() + timeout
                self.buf += clean(raw)
            while "\n" in self.buf:
                line, self.buf = self.buf.split("\n", 1)
                yield line.strip()

    def run(self, line, until, timeout=5.0):
        """Send one command, return its answer lines up to the first one @p until accepts, or what
        arrived by the timeout (None as the last item then)."""
        self.send(line)
        seen = []
        for text in self.lines(timeout):
            text = text.removeprefix(PROMPT).strip()
            # The shell echoes the command it was sent: not an answer.
            if not text or text == line or line.endswith(text) or text.startswith(line[:24]):
                continue
            seen.append(text)
            self.echo(text)
            if until(text):
                return seen
        seen.append(None)
        return seen


class SwapError(Exception):
    pass


def result_of(lines):
    """(ok, message) from the lines of an answer: the device's "stage result" line."""
    for text in reversed([x for x in lines if x is not None]):
        m = RESULT.search(text)
        if m:
            return m.group(1) == "ok", m.group(1) if m.group(1) == "ok" else m.group(3)
    return False, "no answer from the device (is the multiapp shell on this port?)"


def finish(sh, cmd, timeout):
    lines = sh.run(cmd, lambda t: RESULT.search(t) is not None, timeout)
    return result_of(lines), lines


def push(sh, stage, data, chunk=DEFAULT_CHUNK, timeout_ms=None, timeout=10.0, progress=None, name=None):
    """Upload a kernel file and swap it in; (ok, message). Raises SwapError when a line is lost."""
    cmds = list(commands(stage, data, chunk, timeout_ms, name))
    start = sh.run(cmds[0], lambda t: t.startswith("put ") or RESULT.search(t), timeout)
    if any(RESULT.search(t or "") for t in start):
        return result_of(start)
    if start[-1] is None:
        raise SwapError("no answer to put")
    sent = 0
    for i, cmd in enumerate(cmds[1:-1], 1):
        want = min(i * chunk, len(data))
        answer = sh.run(cmd, lambda t: re.match(r"ok \d+$", t) or RESULT.search(t), timeout)
        last = answer[-1]
        if last is None:
            raise SwapError(f"no answer to data line {i} of {len(cmds) - 2}")
        if RESULT.search(last):
            return result_of(answer)
        sent = int(last.split()[1])
        if sent != want:
            raise SwapError(f"the device has {sent} bytes after line {i}, {want} were sent: "
                            "a line was lost, start again")
        if progress:
            progress(sent, len(data))
    if sent != len(data):
        raise SwapError(f"{sent} of {len(data)} bytes arrived")
    swap_wait = (timeout_ms / 1000 if timeout_ms else 2.0) + timeout
    (ok, msg), _ = finish(sh, cmds[-1], swap_wait)
    return ok, msg


def kernel_file(args):
    """The file to push: the argument, or the built kernel of --target."""
    if args.build and args.target:
        top = Path(args.build)
        image = top / "multiapp" if (top / "multiapp" / "build.ninja").exists() else top
        subprocess.run(["ninja", "-C", str(image), f"{args.target}_kernel"], check=True)
        return image / "llext" / "files" / f"{args.target}.llext"
    if args.kernel:
        return Path(args.kernel)
    raise SystemExit("kswap.py: give a kernel file, or --build and --target")


def open_port(path, baud):
    import serial  # pyserial: not needed by the framing's tests

    return serial.Serial(path, baud, timeout=0.1)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("kernel", nargs="?", help="the kernel file (<name>.llext)")
    p.add_argument("--port", required=True, help="the board's shell port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--stage", help="the stage to swap (multiapp stage list)")
    p.add_argument("--build", help="a sysbuild (or multiapp image) build directory")
    p.add_argument("--target", help="the kernel's name in multiapp_kernel(): build it, push it")
    p.add_argument("--file", help="a kernel file already on the volume: the device loads it itself")
    p.add_argument("--revert", action="store_true", help="back to the app's own function")
    p.add_argument("--list", action="store_true", help="list the stages and their kernels")
    p.add_argument("--chunk", type=int, default=DEFAULT_CHUNK,
                   help="bytes per data line, a multiple of 3 (the shell's line is 96 characters "
                   "on the ESP32-S3: %(default)s keeps it under)")
    p.add_argument("--timeout-ms", type=int, help="how long the device waits for the app's sync point")
    p.add_argument("--char-delay", type=float, default=0.0005,
                   help="seconds between characters: the shell's serial receive ring is 64 bytes "
                   "(CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE), a whole data line sent at once "
                   "overflows it; 0 only with a larger ring")
    a = p.parse_args(argv)
    if not (a.list or a.stage):
        p.error("--stage is needed")

    data = name = None
    if not (a.list or a.revert or a.file):
        path = kernel_file(a)
        data = path.read_bytes()
        name = path.stem  # a signed kernel is signed under its file's name (the device checks it)
    port = open_port(a.port, a.baud)
    sh = Shell(port, a.char_delay, echo=lambda line: print(f"device: {line}"))
    # A bare line makes the shell show its prompt, and drops anything half typed.
    port.write(b"\r\n")
    time.sleep(0.2)
    port.reset_input_buffer()
    try:
        if a.list:
            sh.run("multiapp stage list", lambda t: t.startswith("kernel buffer"), 5.0)
            return 0
        if a.revert:
            ok, msg = finish(sh, f"multiapp stage revert {a.stage}" +
                             (f" {a.timeout_ms}" if a.timeout_ms else ""), 10.0)[0]
        elif a.file:
            ok, msg = finish(sh, f"multiapp stage load {a.stage} {a.file}" +
                             (f" {a.timeout_ms}" if a.timeout_ms else ""), 10.0)[0]
        else:
            ok, msg = push(sh, a.stage, data, a.chunk, a.timeout_ms, name=name)
    except SwapError as err:
        sh.run("multiapp stage abort", lambda t: t.startswith("upload"), 2.0)
        print(f"kswap: {err}", file=sys.stderr)
        return 1
    print(f"kswap: {'ok' if ok else 'failed: ' + msg}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
