#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless self test, no board and no display: a fake sender on 127.0.0.1 (fake_vrt.py) with known
losses, received through vrt_source and through the pipe path with a stand-in for vrt_udp.

  nix-shell -p gnuradio.pythonEnv --run 'QT_QPA_PLATFORM=offscreen python3 selftest.py'
"""
import subprocess
import threading
import sys
import time
from pathlib import Path

import numpy as np
import pmt
from gnuradio import blocks, gr

import vrt_pipe
import fake_vrt
from vrt_source import VrtBusy, command, parse, parse_ack, vrt_source

HERE = Path(__file__).resolve().parent
# Packets encoded by libvrt as the board (src/ctl.c) and vrt_udp.c build them: see golden_gen.c.
GOLDEN_CONTEXT = bytes.fromhex(
    "42e500125d5d00010000007b000000001b2e0200a8a1800200000008000007a120000000000916f7200000000000ff80000007a12000000000000007a00001c70000000000000004")
GOLDEN_COMMAND_ALL = bytes.fromhex(
    "6000000a5d5d0001010d00000000000108a00000000916f72000000000000f00000007a120000000")
GOLDEN_COMMAND_AUTO = bytes.fromhex(
    "600000085d5d0001010d00000000000200a000000000ff8000002625a0000000")
GOLDEN_COMMAND_FREQ = bytes.fromhex(
    "600000075d5d0001010d000000000003080000000008fc4330000000")
GOLDEN_ACK_EXECUTED = bytes.fromhex(
    "640000045d5d00010008040000000001")
GOLDEN_ACK_REFUSED = bytes.fromhex(
    "640000075d5d00010009000000000002082000001000000010000000")
RATE, TONE, PACKETS, DROP = 1e6, 125e3, 600, "100-102,300,450-452"
SPP = 720
PORT = 4992 + 17


def sender(rate=RATE, drop=DROP, packets=PACKETS, port=PORT, pps=0, other=False, extra=()):
    return subprocess.Popen([sys.executable, str(HERE / "fake_vrt.py"), "serve", "--port", str(port), "--rate", str(rate),
                             "--tone", str(TONE), "--drop", drop, "--packets", str(packets), "--pps", str(pps)]
                            + ["--other-host"] * other + list(extra))


def check(name, x, tags=None):
    zero = np.abs(x) == 0
    runs = np.flatnonzero(np.diff(np.r_[0, zero.astype(int), 0]))   # start/stop of zero runs
    starts, lens = runs[0::2], runs[1::2] - runs[0::2]
    good = x[~zero]
    spec = np.abs(np.fft.fft(good[:65536] * np.blackman(65536)))
    f = np.fft.fftfreq(65536, 1 / RATE)[np.argmax(spec)]
    amp = np.sqrt(np.mean(np.abs(good) ** 2))
    print(f"{name}: {len(x)} samples (expected {PACKETS * SPP}), tone {f / 1e3:.2f} kHz (expected {TONE / 1e3:g}), "
          f"rms {amp:.3f} (expected {0.5:.3f}), zero runs at packets {starts // SPP} of {lens // SPP} packets")
    ok = len(x) == PACKETS * SPP and abs(f - TONE) < RATE / 65536 * 2 and abs(amp - 0.5) < 0.01
    ok &= list(starts // SPP) == [100, 300, 450] and list(lens // SPP) == [3, 1, 3]
    if tags is not None:
        ov = [(t.offset, pmt.to_long(t.value)) for t in tags if pmt.symbol_to_string(t.key) == "overflow"]
        other = {pmt.symbol_to_string(t.key): t.value for t in tags if pmt.symbol_to_string(t.key) != "overflow"}
        print(f"  overflow tags at packets {[o // SPP for o, _ in ov]} of {[n // SPP for _, n in ov]} packets;"
              f" rx_freq {pmt.to_double(other['rx_freq']) / 1e6:g} MHz, rx_rate {pmt.to_double(other['rx_rate']):g}, "
              f"rx_time {pmt.write_string(other['rx_time'])}")
        ok &= [o // SPP for o, _ in ov] == [100, 300, 450] and [n // SPP for _, n in ov] == [3, 1, 3]
        ok &= pmt.to_double(other["rx_rate"]) == RATE and pmt.to_double(other["rx_freq"]) == 2.44e9
    return bool(ok)


def via_block():
    tb = gr.top_block()
    src, snk = vrt_source("127.0.0.1", PORT), blocks.vector_sink_c()
    tb.connect(src, snk)
    s = sender()
    tb.start()
    s.wait()
    time.sleep(0.5)
    tb.stop()
    tb.wait()
    return check("vrt_source", np.array(snk.data(), np.complex64), snk.tags())


def via_pipe():
    s = sender(port=PORT + 1)
    proc = vrt_pipe.start_receiver([sys.executable, str(HERE / "fake_vrt.py"), "vrt_udp", f"127.0.0.1:{PORT + 1}", "-o", "-"])
    tb = gr.top_block()
    src, snk = blocks.file_descriptor_source(1, proc.stdout.fileno(), False), blocks.vector_sink_c()
    tb.connect(src, blocks.interleaved_char_to_complex(False, 128.0), snk)
    tb.run()                                              # ends when the stand-in closes the pipe
    s.wait()
    print("  stand-in exit status", proc.wait())
    return check("pipe", np.array(snk.data(), np.complex64))


def discontinuity(name, port, extra, drop="", packets=300, want=None, **kw):
    """Run the block against the fake with a fault; want = (samples, zero runs [(packet, packets)], tag names)."""
    tb = gr.top_block()
    src, snk = vrt_source("127.0.0.1", port), blocks.vector_sink_c()
    tb.connect(src, snk)
    s = sender(port=port, drop=drop, packets=packets, extra=extra)
    tb.start()
    s.wait()
    time.sleep(0.5)
    tb.stop()
    tb.wait()
    x = np.array(snk.data(), np.complex64)
    zero = np.abs(x) == 0
    edges = np.flatnonzero(np.diff(np.r_[0, zero.astype(int), 0]))
    runs = [(int(a) // SPP, int(b - a) // SPP) for a, b in zip(edges[0::2], edges[1::2])]
    tags = sorted({pmt.symbol_to_string(t.key) for t in snk.tags()} - {"rx_freq", "rx_rate"})
    ov = [(t.offset // SPP, pmt.to_long(t.value) // SPP) for t in snk.tags() if pmt.symbol_to_string(t.key) == "overflow"]
    f = np.angle(x[1:] * np.conj(x[:-1])) * RATE / 2 / np.pi
    print(f"{name}: {len(x)} samples, zero runs (packet, length) {runs}, tags {tags}, overflow {ov}, "
          f"restarts {src.restarts}, late {src.late}, tone jitter {np.median(np.abs(f - TONE)) / 1e3:.2f} kHz")
    return (len(x), runs, tags) == want


def golden():
    """The packets as libvrt writes them: the fake must build the same bytes, and the parser must read
    them (the first context parser read the CIF1 word as a frequency and a rate of 268 MS/s)."""
    ctx = fake_vrt.context_packet(5, 2.44e9, 8e6, -1, 123, 456000000, True, 7)
    p = parse(GOLDEN_CONTEXT)
    cmds = [command(2.44e9, 30, 8e6, 1), command(None, -1, 40e6, 2), command(2.412e9, None, None, 3)]
    print(f"golden context: rf {p['rf_frequency']:g} Hz, rate {p['sample_rate']:g}, fake builds the same bytes: {ctx == GOLDEN_CONTEXT}; "
          f"commands equal: {cmds == [GOLDEN_COMMAND_ALL, GOLDEN_COMMAND_AUTO, GOLDEN_COMMAND_FREQ]}; "
          f"fake decodes: {fake_vrt.decode_command(GOLDEN_COMMAND_ALL)}")
    refused = fake_vrt.ack_packet(2, False, {"freq": 3.5e9, "rate": 3e6})
    acks = (parse_ack(GOLDEN_ACK_EXECUTED), parse_ack(GOLDEN_ACK_REFUSED), parse_ack(fake_vrt.struct.pack(">IIII", 6 << 28 | 4, 0x5D5D0001, 1 << 19, 9)))
    print(f"acks: executed {acks[0]}, refused {acks[1]}, a CAM without executed {acks[2]}; fake builds the same bytes: "
          f"{fake_vrt.ack_packet(1, True) == GOLDEN_ACK_EXECUTED and refused == GOLDEN_ACK_REFUSED}")
    src = vrt_source("127.0.0.1", PORT + 5)                      # feed it the context, then two packets in a row
    src._packet(GOLDEN_CONTEXT)
    for i in range(2):
        src._packet(fake_vrt.data_packet(i, 100, 90 * i * 10**6, bytes(2 * SPP)))
    src.flush()
    print(f"block: rate {src.rate:g}, {len(src.buf)} samples buffered for 2 packets (no gap fill)")
    return (ctx == GOLDEN_CONTEXT and p["sample_rate"] == 8e6 and p["rf_frequency"] == 2.44e9 and src.rate == 8e6
            and cmds == [GOLDEN_COMMAND_ALL, GOLDEN_COMMAND_AUTO, GOLDEN_COMMAND_FREQ] and len(src.buf) == 2 * SPP
            and fake_vrt.decode_command(GOLDEN_COMMAND_ALL) == (2.44e9, 30.0, 8e6)
            and acks == ((True, []), (False, ['rf frequency (out of range)', 'sample rate (out of range)']), (False, []))
            and refused == GOLDEN_ACK_REFUSED and fake_vrt.ack_packet(1, True) == GOLDEN_ACK_EXECUTED)


def evicted():
    """Another host takes the stream while the flowgraph runs: the renewal gets VRTB. Does top_block end?"""
    s = sender(port=PORT + 12, drop="", packets=4000, extra=["--evict", "100"])
    tb = gr.top_block()
    src = vrt_source("127.0.0.1", PORT + 12)
    tb.connect(src, blocks.null_sink(8))
    t = time.time()
    tb.start()
    done = threading.Thread(target=tb.wait, daemon=True)
    done.start()
    done.join(8)
    ended = not done.is_alive()
    print(f"evicted: flowgraph ended by itself after {time.time() - t:.1f} s: {ended}; error: {src.error}")
    s.kill()
    tb.stop()
    tb.wait()
    return ended and isinstance(src.error, VrtBusy)


def settings():
    """rate, freq and gain arguments: the command goes out after the subscription and the context changes."""
    s = sender(port=PORT + 6, drop="", packets=300)
    tb = gr.top_block()
    src, snk = vrt_source("127.0.0.1", PORT + 6, rate=RATE, freq=2.412e9, gain="auto"), blocks.vector_sink_c()
    tb.connect(src, snk)
    tb.start()
    s.wait()
    time.sleep(0.5)
    tb.stop()
    tb.wait()
    freqs = [pmt.to_double(t.value) for t in snk.tags() if pmt.symbol_to_string(t.key) == "rx_freq"]
    print(f"settings: acknowledged {src.acked}, rx_freq tags {freqs}")
    return src.acked and freqs == [2.44e9, 2.412e9]


def refusal():
    """A frequency and a rate the board refuses: the answer has no executed and carries the error words;
    the flowgraph must end with an error that names both settings. Without the check it would run on."""
    s = sender(port=PORT + 13, drop="", packets=3000)
    tb = gr.top_block()
    src = vrt_source("127.0.0.1", PORT + 13, rate=3e6, freq=3.5e9, gain="auto")
    tb.connect(src, blocks.null_sink(8))
    tb.start()
    done = threading.Thread(target=tb.wait, daemon=True)
    done.start()
    done.join(8)
    print(f"refusal: flowgraph ended: {not done.is_alive()}; error: {src.error}")
    s.kill()
    tb.stop()
    tb.wait()
    return not done.is_alive() and not src.acked and "rf frequency" in str(src.error) and "sample rate" in str(src.error)


def busy_and_takeover():
    """Another host holds the stream: VRTB must surface as VrtBusy, takeover=True must get the samples."""
    s = sender(port=PORT + 3, packets=100, drop="", other=True)
    src, raised = vrt_source("127.0.0.1", PORT + 3), False
    for _ in range(20):
        if src.work([], [np.zeros(1000, np.complex64)]) < 0:
            raised = isinstance(src.error, VrtBusy)
            print("busy:", src.error)
            break
    src.sock.close()
    tb = gr.top_block()
    src, snk = vrt_source("127.0.0.1", PORT + 3, takeover=True), blocks.vector_sink_c()
    tb.connect(src, snk)
    tb.start()
    s.wait()
    time.sleep(0.5)
    tb.stop()
    tb.wait()
    print(f"takeover: {len(snk.data())} samples (expected {100 * SPP})")
    return raised and len(snk.data()) == 100 * SPP


def clean_run():
    """No loss: the tone must be phase continuous (instantaneous frequency constant) through the block."""
    tb = gr.top_block()
    src, snk = vrt_source("127.0.0.1", PORT + 2), blocks.vector_sink_c()
    tb.connect(src, snk)
    s = sender(port=PORT + 2, drop="", packets=300)
    tb.start()
    s.wait()
    time.sleep(0.5)
    tb.stop()
    tb.wait()
    x = np.array(snk.data(), np.complex64)
    f = np.angle(x[1:] * np.conj(x[:-1])) * RATE / 2 / np.pi
    print(f"continuity: {len(x)} samples, instantaneous frequency {f.mean() / 1e3:.2f} kHz, std {f.std() / 1e3:.2f} kHz")
    return bool(len(x) == 300 * SPP and abs(f.mean() - TONE) < 1e3 and f.std() < 5e3)


if __name__ == "__main__":
    cases = [
        discontinuity("16+ lost packets", PORT + 7, [], "100-119", want=(300 * SPP, [(100, 20)], ["overflow", "rx_time"])),
        discontinuity("board drop, counter intact", PORT + 8, ["--jump", "100:3"], want=(303 * SPP, [(100, 3)], ["overflow", "rx_time"])),
        discontinuity("restart (+50 s)", PORT + 9, ["--restart", "150:50"], want=(300 * SPP, [], ["restart", "rx_time"])),
        discontinuity("swapped packets", PORT + 10, ["--swap", "150"], want=(300 * SPP, [], ["rx_time"])),
    ]
    results = [golden(), *cases, evicted(), refusal(), clean_run(), via_block(), via_pipe(), busy_and_takeover(), settings()]
    print("PASS" if all(results) else "FAIL", results)
    sys.exit(not all(results))
