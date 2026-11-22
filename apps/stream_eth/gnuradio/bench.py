#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Throughput benchmarks, no board: how fast can GNU Radio drain the pipe, and what can vrt_source take.

  nix-shell -p gnuradio.pythonEnv --run 'python3 bench.py [pipe|block]'

pipe: head -c 4 GB of zeros into a pipe, unthrottled, into (a) a null sink, (b) the scaling chain
(cs8 to complex float, +-1) into a null sink, (c) the display branch of vrt_pipe.py into a null sink.
block: fake_vrt.py serves a tone at stepped sample rates; the highest rate vrt_source takes without loss.
"""
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from gnuradio import blocks, gr

import vrt_pipe
from vrt_source import vrt_source

HERE = Path(__file__).resolve().parent
GB = 4 << 30


def pipe_rate(name, build):
    proc = vrt_pipe.start_receiver(["head", "-c", str(GB), "/dev/zero"])
    tb = gr.top_block()
    src = blocks.file_descriptor_source(1, proc.stdout.fileno(), False)
    build(tb, src)
    t = time.perf_counter()
    tb.run()
    dt = time.perf_counter() - t
    print(f"{name}: {GB / dt / 1e6:.0f} MB/s of cs8 = {GB / 2 / dt / 1e6:.1f} MS/s ({dt:.1f} s)", flush=True)


def block_rates():
    port = 5300
    for rate in (1e6, 4e6, 8e6, 16e6, 24e6, 32e6, 40e6):
        port += 1
        packets = int(rate / 720 * 8)                               # 8 seconds
        tb = gr.top_block()
        src, snk = vrt_source("127.0.0.1", port), blocks.null_sink(8)
        tb.connect(src, snk)
        s = subprocess.Popen([sys.executable, str(HERE / "fake_vrt.py"), "serve", "--port", str(port), "--rate", str(rate),
                              "--packets", str(packets)])
        tb.start()
        s.wait()
        time.sleep(0.3)
        tb.stop()
        tb.wait()
        print(f"vrt_source at {rate / 1e6:g} MS/s ({rate / 720:.0f} packets/s): {src.lost / (packets * 720):.1%} of the samples lost, "
              f"{src.nitems_written(0) / 1e6:.2f} M samples out", flush=True)


if __name__ == "__main__":
    what = sys.argv[1:] or ["pipe", "block"]
    if "pipe" in what:
        pipe_rate("pipe to null sink", lambda tb, src: tb.connect(src, blocks.null_sink(1)))
        pipe_rate("pipe to cs8 to complex +-1 to null sink",
                  lambda tb, src: tb.connect(src, blocks.interleaved_char_to_complex(False, 128.0), blocks.null_sink(8)))
        pipe_rate("pipe to display branch (1 frame in K) to null sink",
                  lambda tb, src: tb.connect(vrt_pipe.frames(tb, src, 40e6), blocks.null_sink(8)))
    if "block" in what:
        block_rates()
