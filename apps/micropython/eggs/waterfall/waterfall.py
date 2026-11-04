#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: Waterfall
# Author: esp-sdr
# Description: A gapless waterfall of 250 kHz to 2 MHz (a chooser, up to the board's top rate: 2 MS/s on the WHY2025 badge, 500 kS/s on the S3), or of 40 or 80 MHz: without a gap where the radio streams it (the WHY2025 badge: the bursts its channelizer finds, each at its channel, over quiet noise), else from bursts; the knob tunes from 1.8 to 2.8 GHz and sets the radio's gain (or leaves it to AGC), the FFT size and the lower and upper levels of the colour scale (at 40 and 80 MHz the lower 25 dB higher, the upper 40 dB higher).
# GNU Radio version: 3.10.12.0

from PyQt5 import Qt
from gnuradio import qtgui
from PyQt5 import QtCore
from PyQt5.QtCore import QObject, pyqtSlot
from gnuradio import gr
from gnuradio.filter import firdes
from gnuradio.fft import window
import sys
import signal
from PyQt5 import Qt
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
from gnuradio import soapy
import sip
import threading


def snipfcn_fft_reaches_the_sinks(self):
    for sink in (self.qtgui_waterfall_sink_x_0,):
        self._fft_combo_box.currentIndexChanged.connect(lambda i, s=sink: s.set_fft_size(self._fft_options[i]))


def snippets_main_after_init(tb):
    snipfcn_fft_reaches_the_sinks(tb)

class waterfall(gr.top_block, Qt.QWidget):

    def __init__(self):
        gr.top_block.__init__(self, "Waterfall", catch_exceptions=True)
        Qt.QWidget.__init__(self)
        self.setWindowTitle("Waterfall")
        qtgui.util.check_set_qss()
        try:
            self.setWindowIcon(Qt.QIcon.fromTheme('gnuradio-grc'))
        except BaseException as exc:
            print(f"Qt GUI: Could not set Icon: {str(exc)}", file=sys.stderr)
        self.top_scroll_layout = Qt.QVBoxLayout()
        self.setLayout(self.top_scroll_layout)
        self.top_scroll = Qt.QScrollArea()
        self.top_scroll.setFrameStyle(Qt.QFrame.NoFrame)
        self.top_scroll_layout.addWidget(self.top_scroll)
        self.top_scroll.setWidgetResizable(True)
        self.top_widget = Qt.QWidget()
        self.top_scroll.setWidget(self.top_widget)
        self.top_layout = Qt.QVBoxLayout(self.top_widget)
        self.top_grid_layout = Qt.QGridLayout()
        self.top_layout.addLayout(self.top_grid_layout)

        self.settings = Qt.QSettings("gnuradio/flowgraphs", "waterfall")

        try:
            geometry = self.settings.value("geometry")
            if geometry:
                self.restoreGeometry(geometry)
        except BaseException as exc:
            print(f"Qt GUI: Could not restore geometry: {str(exc)}", file=sys.stderr)
        self.flowgraph_started = threading.Event()

        ##################################################
        # Variables
        ##################################################
        self.upper = upper = -60
        self.samp_rate = samp_rate = 250e3
        self.lower = lower = -90
        self.gain = gain = 48
        self.freq = freq = 2.44e9
        self.fft = fft = 1024
        self.agc = agc = True

        ##################################################
        # Blocks
        ##################################################

        self._upper_range = qtgui.Range(-100, -10, 1, -60, 200)
        self._upper_win = qtgui.RangeWidget(self._upper_range, self.set_upper, "Upper dB", "counter_slider", float, QtCore.Qt.Horizontal)
        self.top_layout.addWidget(self._upper_win)
        # Create the options list
        self._samp_rate_options = [250000.0, 500000.0, 1000000.0, 2000000.0, 40000000.0, 80000000.0]
        # Create the labels list
        self._samp_rate_labels = ['250 kS/s', '500 kS/s', '1 MS/s', '2 MS/s', '40 MS/s', '80 MS/s']
        # Create the combo box
        self._samp_rate_tool_bar = Qt.QToolBar(self)
        self._samp_rate_tool_bar.addWidget(Qt.QLabel("Rate" + ": "))
        self._samp_rate_combo_box = Qt.QComboBox()
        self._samp_rate_tool_bar.addWidget(self._samp_rate_combo_box)
        for _label in self._samp_rate_labels: self._samp_rate_combo_box.addItem(_label)
        self._samp_rate_callback = lambda i: Qt.QMetaObject.invokeMethod(self._samp_rate_combo_box, "setCurrentIndex", Qt.Q_ARG("int", self._samp_rate_options.index(i)))
        self._samp_rate_callback(self.samp_rate)
        self._samp_rate_combo_box.currentIndexChanged.connect(
            lambda i: self.set_samp_rate(self._samp_rate_options[i]))
        # Create the radio buttons
        self.top_grid_layout.addWidget(self._samp_rate_tool_bar, 1, 0, 1, 1)
        for r in range(1, 2):
            self.top_grid_layout.setRowStretch(r, 1)
        for c in range(0, 1):
            self.top_grid_layout.setColumnStretch(c, 1)
        self._lower_range = qtgui.Range(-110, -20, 1, -90, 200)
        self._lower_win = qtgui.RangeWidget(self._lower_range, self.set_lower, "Lower dB", "counter_slider", float, QtCore.Qt.Horizontal)
        self.top_layout.addWidget(self._lower_win)
        self._gain_range = qtgui.Range(0, 73, 1, 48, 200)
        self._gain_win = qtgui.RangeWidget(self._gain_range, self.set_gain, "Gain", "counter_slider", float, QtCore.Qt.Horizontal)
        self.top_grid_layout.addWidget(self._gain_win, 0, 0, 1, 1)
        for r in range(0, 1):
            self.top_grid_layout.setRowStretch(r, 1)
        for c in range(0, 1):
            self.top_grid_layout.setColumnStretch(c, 1)
        self._freq_range = qtgui.Range(1.8e9, 2.8e9, 1e6, 2.44e9, 200)
        self._freq_win = qtgui.RangeWidget(self._freq_range, self.set_freq, "Frequency", "counter_slider", float, QtCore.Qt.Horizontal)
        self.top_layout.addWidget(self._freq_win)
        # Create the options list
        self._fft_options = [256, 512, 1024, 2048, 4096]
        # Create the labels list
        self._fft_labels = ['256', '512', '1024', '2048', '4096']
        # Create the combo box
        self._fft_tool_bar = Qt.QToolBar(self)
        self._fft_tool_bar.addWidget(Qt.QLabel("FFT" + ": "))
        self._fft_combo_box = Qt.QComboBox()
        self._fft_tool_bar.addWidget(self._fft_combo_box)
        for _label in self._fft_labels: self._fft_combo_box.addItem(_label)
        self._fft_callback = lambda i: Qt.QMetaObject.invokeMethod(self._fft_combo_box, "setCurrentIndex", Qt.Q_ARG("int", self._fft_options.index(i)))
        self._fft_callback(self.fft)
        self._fft_combo_box.currentIndexChanged.connect(
            lambda i: self.set_fft(self._fft_options[i]))
        # Create the radio buttons
        self.top_grid_layout.addWidget(self._fft_tool_bar, 1, 1, 1, 1)
        for r in range(1, 2):
            self.top_grid_layout.setRowStretch(r, 1)
        for c in range(1, 2):
            self.top_grid_layout.setColumnStretch(c, 1)
        _agc_check_box = Qt.QCheckBox("AGC")
        self._agc_choices = {True: True, False: False}
        self._agc_choices_inv = dict((v,k) for k,v in self._agc_choices.items())
        self._agc_callback = lambda i: Qt.QMetaObject.invokeMethod(_agc_check_box, "setChecked", Qt.Q_ARG("bool", self._agc_choices_inv[i]))
        self._agc_callback(self.agc)
        _agc_check_box.stateChanged.connect(lambda i: self.set_agc(self._agc_choices[bool(i)]))
        self.top_grid_layout.addWidget(_agc_check_box, 0, 1, 1, 1)
        for r in range(0, 1):
            self.top_grid_layout.setRowStretch(r, 1)
        for c in range(1, 2):
            self.top_grid_layout.setColumnStretch(c, 1)
        self.soapy_plutosdr_source_0 = None
        dev = 'driver=plutosdr'
        stream_args = ''
        tune_args = ['']
        settings = ['']

        self.soapy_plutosdr_source_0 = soapy.source(dev, "fc32", 1, '',
                                  stream_args, tune_args, settings)
        self.soapy_plutosdr_source_0.set_sample_rate(0, samp_rate)
        self.soapy_plutosdr_source_0.set_bandwidth(0, 0)
        self.soapy_plutosdr_source_0.set_gain_mode(0, agc)
        self.soapy_plutosdr_source_0.set_frequency(0, freq)
        self.soapy_plutosdr_source_0.set_gain(0, min(max(gain, 0.0), 73.0))
        self.qtgui_waterfall_sink_x_0 = qtgui.waterfall_sink_c(
            fft, #size
            window.WIN_BLACKMAN_hARRIS, #wintype
            freq, #fc
            samp_rate, #bw
            "Waterfall", #name
            1, #number of inputs
            None # parent
        )
        self.qtgui_waterfall_sink_x_0.set_update_time(0.02)
        self.qtgui_waterfall_sink_x_0.enable_grid(True)
        self.qtgui_waterfall_sink_x_0.enable_axis_labels(True)



        labels = ['', '', '', '', '',
                  '', '', '', '', '']
        colors = [0, 0, 0, 0, 0,
                  0, 0, 0, 0, 0]
        alphas = [1.0, 1.0, 1.0, 1.0, 1.0,
                  1.0, 1.0, 1.0, 1.0, 1.0]

        for i in range(1):
            if len(labels[i]) == 0:
                self.qtgui_waterfall_sink_x_0.set_line_label(i, "Data {0}".format(i))
            else:
                self.qtgui_waterfall_sink_x_0.set_line_label(i, labels[i])
            self.qtgui_waterfall_sink_x_0.set_color_map(i, colors[i])
            self.qtgui_waterfall_sink_x_0.set_line_alpha(i, alphas[i])

        self.qtgui_waterfall_sink_x_0.set_intensity_range(lower + (25 if samp_rate > 10e6 else 0), upper + (40 if samp_rate > 10e6 else 0))

        self._qtgui_waterfall_sink_x_0_win = sip.wrapinstance(self.qtgui_waterfall_sink_x_0.qwidget(), Qt.QWidget)

        self.top_layout.addWidget(self._qtgui_waterfall_sink_x_0_win)


        ##################################################
        # Connections
        ##################################################
        self.connect((self.soapy_plutosdr_source_0, 0), (self.qtgui_waterfall_sink_x_0, 0))


    def closeEvent(self, event):
        self.settings = Qt.QSettings("gnuradio/flowgraphs", "waterfall")
        self.settings.setValue("geometry", self.saveGeometry())
        self.stop()
        self.wait()

        event.accept()

    def get_upper(self):
        return self.upper

    def set_upper(self, upper):
        self.upper = upper
        self.qtgui_waterfall_sink_x_0.set_intensity_range(self.lower + (25 if self.samp_rate > 10e6 else 0), self.upper + (40 if self.samp_rate > 10e6 else 0))

    def get_samp_rate(self):
        return self.samp_rate

    def set_samp_rate(self, samp_rate):
        self.samp_rate = samp_rate
        self._samp_rate_callback(self.samp_rate)
        self.qtgui_waterfall_sink_x_0.set_frequency_range(self.freq, self.samp_rate)
        self.qtgui_waterfall_sink_x_0.set_intensity_range(self.lower + (25 if self.samp_rate > 10e6 else 0), self.upper + (40 if self.samp_rate > 10e6 else 0))
        self.soapy_plutosdr_source_0.set_sample_rate(0, self.samp_rate)

    def get_lower(self):
        return self.lower

    def set_lower(self, lower):
        self.lower = lower
        self.qtgui_waterfall_sink_x_0.set_intensity_range(self.lower + (25 if self.samp_rate > 10e6 else 0), self.upper + (40 if self.samp_rate > 10e6 else 0))

    def get_gain(self):
        return self.gain

    def set_gain(self, gain):
        self.gain = gain
        self.soapy_plutosdr_source_0.set_gain(0, min(max(self.gain, 0.0), 73.0))

    def get_freq(self):
        return self.freq

    def set_freq(self, freq):
        self.freq = freq
        self.qtgui_waterfall_sink_x_0.set_frequency_range(self.freq, self.samp_rate)
        self.soapy_plutosdr_source_0.set_frequency(0, self.freq)

    def get_fft(self):
        return self.fft

    def set_fft(self, fft):
        self.fft = fft
        self._fft_callback(self.fft)

    def get_agc(self):
        return self.agc

    def set_agc(self, agc):
        self.agc = agc
        self._agc_callback(self.agc)
        self.soapy_plutosdr_source_0.set_gain_mode(0, self.agc)




def main(top_block_cls=waterfall, options=None):

    qapp = Qt.QApplication(sys.argv)

    tb = top_block_cls()
    snippets_main_after_init(tb)
    tb.start()
    tb.flowgraph_started.set()

    tb.show()

    def sig_handler(sig=None, frame=None):
        tb.stop()
        tb.wait()

        Qt.QApplication.quit()

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    timer = Qt.QTimer()
    timer.start(500)
    timer.timeout.connect(lambda: None)

    qapp.exec_()

if __name__ == '__main__':
    main()
