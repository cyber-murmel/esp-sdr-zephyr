#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""High rate receiver: vrt_udp -o - into a pipe into GNU Radio, with spectrum and waterfall.

  vrt_pipe.py BOARD [-r samples/s] [-f Hz] [-g gain|auto] [--vrt-udp PATH] [--no-gui --seconds N]

The C receiver does the packet work and zero fills lost packets, so the pipe carries a continuous cs8
stream. GNU Radio reads it with file_descriptor_source, scales it to +-1 and shows it. The displays only
see one frame in K (drawn from the raw bytes, before the conversion), so a slow GUI never stalls the
reader; the pipe holds only 1 MiB, which is 13 ms at 40 MS/s.
"""
import argparse
import fcntl
import os
import subprocess
import sys

from gnuradio import blocks, gr

FFT = 4096


def start_receiver(cmd):
    """Run cmd with a big stdout pipe (F_SETPIPE_SZ, up to /proc/sys/fs/pipe-max-size); stderr is inherited."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0)
    try:
        limit = int(open("/proc/sys/fs/pipe-max-size").read())
        fcntl.fcntl(p.stdout.fileno(), fcntl.F_SETPIPE_SZ, limit)
    except OSError as e:
        print("pipe size not raised:", e, file=sys.stderr)
    return p


EXIT = {3: "another host has the stream (vrt_udp -T takes it over)", 4: "the board refused a setting",
        5: "no stream arrived"}


def report(rc):
    """Say what vrt_udp's exit status means; returns the status for sys.exit."""
    print("vrt_udp exited with status %d%s" % (rc, ": " + EXIT[rc] if rc in EXIT else ""), file=sys.stderr)
    return rc


def vrt_udp_cmd(a):
    cmd = [a.vrt_udp, a.board, "-o", "-"]
    for flag, val in (("-r", a.rate), ("-f", a.freq), ("-g", a.gain)):
        if val is not None:
            cmd += [flag, str(int(val)) if isinstance(val, float) else val]
    return cmd


def frames(tb, src, rate, fft=FFT, fps=60):
    """cs8 stream to complex +-1 with every K-th frame of fft samples only: the display branch."""
    keep = max(1, int(rate / fft / fps))
    chain = [src, blocks.stream_to_vector(1, 2 * fft), blocks.keep_one_in_n(2 * fft, keep),
             blocks.vector_to_stream(1, 2 * fft), blocks.interleaved_char_to_complex(False, 128.0)]
    tb.connect(*chain)
    return chain[-1]


class Rx(gr.top_block):
    """Flowgraph around a file descriptor; the GUI variant lives in Gui below."""
    def __init__(self, fd, rate, gui_sinks=None):
        gr.top_block.__init__(self, "vrt_pipe")
        self.src = blocks.file_descriptor_source(1, fd, False)
        self.tail = frames(self, self.src, rate)
        for sink in gui_sinks or [blocks.null_sink(gr.sizeof_gr_complex)]:
            self.connect(self.tail, sink)


def run_gui(a, proc):
    from PyQt5 import Qt
    import sip
    from gnuradio import qtgui
    from gnuradio.fft import window
    app = Qt.QApplication(sys.argv)
    rate, freq = a.rate or 40e6, a.freq or 2.44e9
    fsink = qtgui.freq_sink_c(FFT, window.WIN_BLACKMAN_hARRIS, freq, rate, "spectrum", 1)
    wsink = qtgui.waterfall_sink_c(FFT, window.WIN_BLACKMAN_hARRIS, freq, rate, "waterfall", 1)
    fsink.set_y_axis(-100, 0)
    wsink.set_intensity_range(-100, 0)
    tb = Rx(proc.stdout.fileno(), rate, [fsink, wsink])
    win = Qt.QWidget()
    win.setWindowTitle("S31 VRT stream, %s" % a.board)
    lay = Qt.QVBoxLayout(win)
    status = Qt.QLabel("receiving")
    for s in (fsink, wsink):
        lay.addWidget(sip.wrapinstance(s.qwidget(), Qt.QWidget))
    lay.addWidget(status)
    win.resize(1000, 800)
    win.show()
    tb.start()

    def poll():                                   # the receiver exiting closes the pipe: say so, keep the window
        if proc.poll() is not None:
            done.setdefault("rc", proc.returncode)
            status.setText("vrt_udp exited with status %d%s; the last frames stay on screen" % (
                proc.returncode, ": " + EXIT[proc.returncode] if proc.returncode in EXIT else ""))
    done = {}
    timer = Qt.QTimer()
    timer.timeout.connect(poll)
    timer.start(500)
    app.aboutToQuit.connect(lambda: (poll(), proc.terminate(), tb.stop(), tb.wait()))
    app.exec_()
    return report(done["rc"]) if done.get("rc") else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board")
    ap.add_argument("-r", "--rate", type=float, help="samples/s (default: what the board streams)")
    ap.add_argument("-f", "--freq", type=float, help="RF centre frequency in Hz")
    ap.add_argument("-g", "--gain", help="gain or auto")
    ap.add_argument("--vrt-udp", default="vrt_udp")
    ap.add_argument("--no-gui", action="store_true", help="count only, for tests")
    ap.add_argument("--seconds", type=float, default=0, help="pass -t to vrt_udp")
    a = ap.parse_args()
    cmd = vrt_udp_cmd(a) + (["-t", str(a.seconds)] if a.seconds else [])
    proc = start_receiver(cmd)
    if not a.no_gui:
        sys.exit(run_gui(a, proc))
    tb = Rx(proc.stdout.fileno(), a.rate or 40e6)
    tb.run()                                      # returns when the pipe closes
    sys.exit(report(proc.wait()))


if __name__ == "__main__":
    main()
