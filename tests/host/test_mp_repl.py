# SPDX-License-Identifier: GPL-3.0-or-later
"""The MicroPython REPL's input ring of the ESP32-S31 multiapp host (apps/multiapp/ext/mp/repl_core.h), built for
the PC: bytes from an SSH channel, the interrupt character that must reach the interpreter though nothing reads
the ring, a full ring, and a channel that returns an error."""

import ctypes
import shutil
import subprocess

import pytest
from conftest import REPO

EXT = REPO / "apps/multiapp/ext/mp"

HARNESS = r"""
#include <string.h>
#include "repl_core.h"

static struct repl_ring ring;
/* the "channel": what read() hands out, a chunk at a time */
static uint8_t chan[4096];
static size_t chan_len, chan_pos;
static int chan_err;

static int chan_read(void *ctx, void *data, size_t len)
{
	size_t n = chan_len - chan_pos;

	(void)ctx;
	if (chan_err != 0 && n == 0) {
		return chan_err;
	}
	if (n > len) {
		n = len;
	}
	memcpy(data, chan + chan_pos, n);
	chan_pos += n;
	return (int)n;
}

void h_reset(void) { repl_ring_reset(&ring); chan_len = chan_pos = 0; chan_err = 0; }
void h_chan(const uint8_t *d, size_t n) { memcpy(chan + chan_len, d, n); chan_len += n; }
void h_err(int e) { chan_err = e; }
unsigned int h_feed(int intr, size_t *got) { return repl_feed(&ring, chan_read, NULL, intr, got); }
int h_put(uint8_t c) { return repl_ring_put(&ring, c); }
int h_get(void) { return repl_ring_get(&ring); }
size_t h_count(void) { return repl_ring_count(&ring); }
uint32_t h_dropped(void) { return ring.dropped; }
size_t h_size(void) { return REPL_RING_SIZE; }
"""


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    tmp = tmp_path_factory.mktemp("repl")
    (tmp / "h.c").write_text(HARNESS)
    so = tmp / "repl.so"
    r = subprocess.run([cc, "-shared", "-fPIC", "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(so),
                        str(tmp / "h.c"), f"-I{EXT}"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    c = ctypes.CDLL(str(so))
    c.h_chan.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    c.h_feed.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)]
    c.h_feed.restype = ctypes.c_uint
    c.h_put.argtypes = [ctypes.c_uint8]
    c.h_count.restype = ctypes.c_size_t
    c.h_size.restype = ctypes.c_size_t
    c.h_dropped.restype = ctypes.c_uint32
    return c


def drain(lib):
    out = bytearray()
    while (c := lib.h_get()) >= 0:
        out.append(c)
    return bytes(out)


def feed(lib, data, intr=3):
    lib.h_chan(data, len(data))
    got = ctypes.c_size_t()
    ints = lib.h_feed(intr, ctypes.byref(got))
    return ints, got.value


def test_bytes_pass_in_order(lib):
    lib.h_reset()
    assert feed(lib, b"print(1+1)\r") == (0, 11)
    assert drain(lib) == b"print(1+1)\r"
    assert lib.h_get() == -1


def test_ctrl_c_is_not_stored_and_counted(lib):
    lib.h_reset()
    ints, got = feed(lib, b"ab\x03cd\x03\x03")
    assert (ints, got) == (3, 7)
    assert drain(lib) == b"abcd"


def test_ctrl_c_found_in_the_first_ring_of_a_long_stream(lib):
    lib.h_reset()
    data = b"x" * 300 + b"\x03" + b"y" * 300
    ints, got = feed(lib, data)
    assert ints == 1
    assert got == 512, "reading stops when the ring has no room for a chunk"
    assert len(drain(lib)) == 511


def test_input_beyond_the_ring_stays_in_the_channel(lib):
    # 2 KB pasted: the ring takes what fits, the rest waits in the channel (the SSH window holds the client back) and comes
    # as the REPL empties the ring: nothing is dropped, the order is kept
    lib.h_reset()
    data = bytes((i * 31 + 7) % 251 + 1 if (i * 31 + 7) % 251 + 1 != 3 else 4 for i in range(2000))
    lib.h_chan(data, len(data))
    out = bytearray()
    for _ in range(40):
        got = ctypes.c_size_t()
        lib.h_feed(3, ctypes.byref(got))
        assert lib.h_count() <= lib.h_size()
        out += drain(lib)
        if len(out) >= len(data):
            break
    assert bytes(out) == data
    assert lib.h_dropped() == 0


def test_the_ring_is_never_overrun_by_a_feed(lib):
    lib.h_reset()
    lib.h_chan(b"z" * 1500, 1500)
    got = ctypes.c_size_t()
    lib.h_feed(3, ctypes.byref(got))
    assert got.value == 512 and lib.h_count() == 512 and lib.h_dropped() == 0


def test_interrupt_char_disabled(lib):
    # mp_interrupt_char is -1 when the program turned it off (raw REPL, binary transfers): 0x03 is data
    lib.h_reset()
    assert feed(lib, b"a\x03b", intr=-1) == (0, 3)
    assert drain(lib) == b"a\x03b"


def test_ctrl_d_and_other_controls_are_data(lib):
    # Ctrl-D (soft reset), Ctrl-B/E (raw and paste mode) are for the REPL's own reader
    lib.h_reset()
    feed(lib, b"\x04\x02\x05\x01")
    assert drain(lib) == b"\x04\x02\x05\x01"


def test_full_ring_drops_and_counts(lib):
    lib.h_reset()
    size = lib.h_size()
    assert size & (size - 1) == 0
    for _ in range(size):
        assert lib.h_put(65) == 0
    assert lib.h_put(66) == -1
    assert lib.h_dropped() == 1
    assert lib.h_count() == size
    assert lib.h_get() == 65
    assert lib.h_put(66) == 0


def test_wraparound(lib):
    lib.h_reset()
    for rnd in range(5):
        data = bytes((rnd * 7 + i) & 0x7F | 0x04 for i in range(400))
        data = data.replace(b"\x03", b"\x04")
        feed(lib, data)
        assert drain(lib) == data


def test_channel_error_ends_the_feed(lib):
    lib.h_reset()
    lib.h_chan(b"ok", 2)
    lib.h_err(-104)  # -ECONNRESET after the data
    got = ctypes.c_size_t()
    assert lib.h_feed(3, ctypes.byref(got)) == 0
    assert got.value == 2
    assert drain(lib) == b"ok"


def test_reset_empties(lib):
    lib.h_reset()
    feed(lib, b"abc")
    lib.h_reset()
    assert lib.h_count() == 0 and lib.h_get() == -1
