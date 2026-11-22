#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wideband power spectrum, averaged and max-held, with peak markers: the 2.4 GHz band in one view.

  wideband_peaks.py BOARD [-r 40e6] [-f 2.44e9] [-g auto] [--no-gui --seconds N]

At 40 MS/s one 4096 point FFT bin is 9.8 kHz wide, so a Wi-Fi channel (20 MHz) spans 2000 bins and a
Bluetooth LE channel (1 MHz) 100. The stream comes from vrt_udp through a pipe (see vrt_pipe.py); about
200 frames per second are taken from it, windowed, FFTed, converted to dB and averaged (exponential,
alpha 0.1) and max-held. The strongest local maxima of the average are listed as peaks.
"""
import argparse
import sys
import time

import numpy as np
from gnuradio import blocks, gr

import vrt_pipe

N = vrt_pipe.FFT


class psd(gr.sync_block):
    """Frames of N complex samples in; the averaged and the max-held spectrum in dB (fftshifted) out."""
    def __init__(self, alpha=0.1):
        gr.sync_block.__init__(self, "psd", [(np.complex64, N)], [(np.float32, N), (np.float32, N)])
        self.win, self.alpha = np.blackman(N).astype(np.float32), alpha
        self.avg = self.peak = None

    def work(self, input_items, output_items):
        for k, x in enumerate(input_items[0]):
            db = 20 * np.log10(np.abs(np.fft.fftshift(np.fft.fft(x * self.win))) / N + 1e-9)
            self.avg = db if self.avg is None else self.avg + self.alpha * (db - self.avg)
            self.peak = db if self.peak is None else np.maximum(self.peak, db)
            output_items[0][k], output_items[1][k] = self.avg, self.peak
        return len(input_items[0])

    def peaks(self, n=5, guard=20):
        """The n strongest local maxima of the average, at least guard bins apart: [(bin, dB)]."""
        a, out = self.avg.copy(), []
        while len(out) < n:
            i = int(np.argmax(a))
            out.append((i, float(a[i])))
            a[max(0, i - guard):i + guard + 1] = -1e9
        return out


def describe(peaks, freq, rate):
    return ["%.3f MHz  %.1f dB" % ((freq + (i - N / 2) * rate / N) / 1e6, db) for i, db in peaks]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board")
    ap.add_argument("-r", "--rate", type=float, default=40e6)
    ap.add_argument("-f", "--freq", type=float, default=2.44e9)
    ap.add_argument("-g", "--gain")
    ap.add_argument("--vrt-udp", default="vrt_udp")
    ap.add_argument("--no-gui", action="store_true", help="print the peaks once a second instead")
    ap.add_argument("--seconds", type=float, default=0)
    a = ap.parse_args()
    proc = vrt_pipe.start_receiver(vrt_pipe.vrt_udp_cmd(a) + (["-t", str(a.seconds)] if a.seconds else []))
    tb = gr.top_block("wideband_peaks")
    src = blocks.file_descriptor_source(1, proc.stdout.fileno(), False)
    # a frame of N samples every 5 ms: keep one in K of the raw frames, then convert
    cplx = vrt_pipe.frames(tb, src, a.rate, N, fps=200)
    spec = psd()
    tb.connect(cplx, blocks.stream_to_vector(gr.sizeof_gr_complex, N), spec)
    if a.no_gui:
        for i in range(2):
            tb.connect((spec, i), blocks.null_sink(4 * N))
        tb.start()
        while proc.poll() is None:
            time.sleep(1)
            if spec.avg is not None:
                print(*describe(spec.peaks(), a.freq, a.rate), sep="\n", end="\n\n", flush=True)
        tb.stop()
        tb.wait()
        sys.exit(vrt_pipe.report(proc.wait()))
    from PyQt5 import Qt
    import sip
    from gnuradio import qtgui
    app = Qt.QApplication(sys.argv)
    plot = qtgui.vector_sink_f(N, a.freq - a.rate / 2, a.rate / N, "Hz", "dB", "average and max hold", 2)
    plot.set_y_axis(-110, -20)
    plot.set_update_time(0.1)
    tb.connect((spec, 0), (plot, 0))
    tb.connect((spec, 1), (plot, 1))
    win = Qt.QWidget()
    lay = Qt.QVBoxLayout(win)
    lay.addWidget(sip.wrapinstance(plot.qwidget(), Qt.QWidget))
    label = Qt.QLabel()
    lay.addWidget(label)

    def tick():
        if proc.poll() is not None:
            label.setText("vrt_udp exited with status %d%s" % (proc.returncode, ": " + vrt_pipe.EXIT.get(proc.returncode, "")))
        elif spec.avg is not None:
            label.setText("peaks:  " + "     ".join(describe(spec.peaks(), a.freq, a.rate)))
    timer = Qt.QTimer()
    timer.timeout.connect(tick)
    timer.start(250)
    win.resize(1100, 600)
    win.show()
    tb.start()
    state = {}
    app.aboutToQuit.connect(lambda: (state.setdefault("rc", proc.poll()), proc.terminate(), tb.stop(), tb.wait()))
    app.exec_()
    sys.exit(vrt_pipe.report(state["rc"]) if state.get("rc") else 0)


if __name__ == "__main__":
    main()
