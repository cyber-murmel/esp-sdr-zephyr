#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: Data link
# Author: esp-sdr
# Description: Text messages between two boards running this app (or a GNU Radio peer): GFSK at 25 kbaud, CRC-32, acknowledged and sent again until acknowledged; received text and link statistics on the display.
# GNU Radio version: 3.10.12.0

from PyQt5 import Qt
from gnuradio import qtgui
from PyQt5 import QtCore
from PyQt5.QtCore import QObject, pyqtSlot
from gnuradio import blocks
import pmt
from gnuradio import digital
from gnuradio import eng_notation
from gnuradio import gr
from gnuradio.filter import firdes
from gnuradio.fft import window
import sys
import signal
from PyQt5 import Qt
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import gr, pdu
from gnuradio import soapy
import datalink_epy_block_0 as epy_block_0  # embedded python block
import math
import threading



class datalink(gr.top_block, Qt.QWidget):

    def __init__(self):
        gr.top_block.__init__(self, "Data link", catch_exceptions=True)
        Qt.QWidget.__init__(self)
        self.setWindowTitle("Data link")
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

        self.settings = Qt.QSettings("gnuradio/flowgraphs", "datalink")

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
        self.samp_rate = samp_rate = 250000
        self.fdev = fdev = 25e3
        self.tx_text = tx_text = '-'
        self.tx_gain = tx_gain = 8
        self.sps = sps = 10
        self.sensitivity = sensitivity = math.tau * fdev / samp_rate
        self.rx_text = rx_text = '-'
        self.rx_gain = rx_gain = 32
        self.peer_text = peer_text = '-'
        self.msg = msg = 'Hello'
        self.link_text = link_text = '-'
        self.hdr_format = hdr_format = digital.header_format_default('0101010101010101010101010101010111100100001011000110011001011101',8, 1)
        self.freq = freq = 2.44e9
        self.auto = auto = False

        ##################################################
        # Blocks
        ##################################################

        self.tabs = Qt.QTabWidget()
        self.tabs_widget_0 = Qt.QWidget()
        self.tabs_layout_0 = Qt.QBoxLayout(Qt.QBoxLayout.TopToBottom, self.tabs_widget_0)
        self.tabs_grid_layout_0 = Qt.QGridLayout()
        self.tabs_layout_0.addLayout(self.tabs_grid_layout_0)
        self.tabs.addTab(self.tabs_widget_0, "Link")
        self.tabs_widget_1 = Qt.QWidget()
        self.tabs_layout_1 = Qt.QBoxLayout(Qt.QBoxLayout.TopToBottom, self.tabs_widget_1)
        self.tabs_grid_layout_1 = Qt.QGridLayout()
        self.tabs_layout_1.addLayout(self.tabs_grid_layout_1)
        self.tabs.addTab(self.tabs_widget_1, "Radio")
        self.top_grid_layout.addWidget(self.tabs, 0, 0, 1, 1)
        for r in range(0, 1):
            self.top_grid_layout.setRowStretch(r, 1)
        for c in range(0, 1):
            self.top_grid_layout.setColumnStretch(c, 1)
        self._tx_gain_range = qtgui.Range(0, 17, 1, 8, 200)
        self._tx_gain_win = qtgui.RangeWidget(self._tx_gain_range, self.set_tx_gain, "TX gain", "counter_slider", int, QtCore.Qt.Horizontal)
        self.tabs_grid_layout_1.addWidget(self._tx_gain_win, 1, 0, 1, 1)
        for r in range(1, 2):
            self.tabs_grid_layout_1.setRowStretch(r, 1)
        for c in range(0, 1):
            self.tabs_grid_layout_1.setColumnStretch(c, 1)
        self._rx_gain_range = qtgui.Range(0, 82, 1, 32, 200)
        self._rx_gain_win = qtgui.RangeWidget(self._rx_gain_range, self.set_rx_gain, "RX gain", "counter_slider", int, QtCore.Qt.Horizontal)
        self.tabs_grid_layout_1.addWidget(self._rx_gain_win, 2, 0, 1, 1)
        for r in range(2, 3):
            self.tabs_grid_layout_1.setRowStretch(r, 1)
        for c in range(0, 1):
            self.tabs_grid_layout_1.setColumnStretch(c, 1)
        # Create the options list
        self._msg_options = ['Hello', 'Ping', '73', 'Over']
        # Create the labels list
        self._msg_labels = ['Hello', 'Ping', '73', 'Over']
        # Create the combo box
        self._msg_tool_bar = Qt.QToolBar(self)
        self._msg_tool_bar.addWidget(Qt.QLabel("Text" + ": "))
        self._msg_combo_box = Qt.QComboBox()
        self._msg_tool_bar.addWidget(self._msg_combo_box)
        for _label in self._msg_labels: self._msg_combo_box.addItem(_label)
        self._msg_callback = lambda i: Qt.QMetaObject.invokeMethod(self._msg_combo_box, "setCurrentIndex", Qt.Q_ARG("int", self._msg_options.index(i)))
        self._msg_callback(self.msg)
        self._msg_combo_box.currentIndexChanged.connect(
            lambda i: self.set_msg(self._msg_options[i]))
        # Create the radio buttons
        self.tabs_grid_layout_0.addWidget(self._msg_tool_bar, 0, 0, 1, 1)
        for r in range(0, 1):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(0, 1):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self._freq_range = qtgui.Range(2.401e9, 2.482e9, 1e6, 2.44e9, 200)
        self._freq_win = qtgui.RangeWidget(self._freq_range, self.set_freq, "Frequency", "counter_slider", float, QtCore.Qt.Horizontal)
        self.tabs_grid_layout_1.addWidget(self._freq_win, 0, 0, 1, 1)
        for r in range(0, 1):
            self.tabs_grid_layout_1.setRowStretch(r, 1)
        for c in range(0, 1):
            self.tabs_grid_layout_1.setColumnStretch(c, 1)
        _auto_check_box = Qt.QCheckBox("Auto (every 2 s)")
        self._auto_choices = {True: True, False: False}
        self._auto_choices_inv = dict((v,k) for k,v in self._auto_choices.items())
        self._auto_callback = lambda i: Qt.QMetaObject.invokeMethod(_auto_check_box, "setChecked", Qt.Q_ARG("bool", self._auto_choices_inv[i]))
        self._auto_callback(self.auto)
        _auto_check_box.stateChanged.connect(lambda i: self.set_auto(self._auto_choices[bool(i)]))
        self.tabs_grid_layout_0.addWidget(_auto_check_box, 1, 0, 1, 2)
        for r in range(1, 2):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(0, 2):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self._tx_text_tool_bar = Qt.QToolBar(self)

        if None:
            self._tx_text_formatter = None
        else:
            self._tx_text_formatter = lambda x: str(x)

        self._tx_text_tool_bar.addWidget(Qt.QLabel("TX:"))
        self._tx_text_label = Qt.QLabel(str(self._tx_text_formatter(self.tx_text)))
        self._tx_text_tool_bar.addWidget(self._tx_text_label)
        self.tabs_grid_layout_0.addWidget(self._tx_text_tool_bar, 3, 0, 1, 2)
        for r in range(3, 4):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(0, 2):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self.soapy_custom_source_0 = None
        dev = 'driver=' + 'esdr'
        stream_args = ''
        tune_args = ['']
        settings = ['']
        self.soapy_custom_source_0 = soapy.source(dev, "fc32",
                                  1, '',
                                  stream_args, tune_args, settings)
        self.soapy_custom_source_0.set_sample_rate(0, samp_rate)
        self.soapy_custom_source_0.set_bandwidth(0, 0)
        self.soapy_custom_source_0.set_antenna(0, '')
        self.soapy_custom_source_0.set_frequency(0, freq)
        self.soapy_custom_source_0.set_frequency_correction(0, 0)
        self.soapy_custom_source_0.set_gain_mode(0, False)
        self.soapy_custom_source_0.set_gain(0, rx_gain)
        self.soapy_custom_source_0.set_dc_offset_mode(0, False)
        self.soapy_custom_source_0.set_dc_offset(0, 0)
        self.soapy_custom_source_0.set_iq_balance(0, 0)
        self.soapy_custom_sink_0 = None
        dev = 'driver=' + 'esdr'
        stream_args = ''
        tune_args = ['']
        settings = ['']
        self.soapy_custom_sink_0 = soapy.sink(dev, "fc32",
                                1, '',
                                stream_args, tune_args, settings)
        self.soapy_custom_sink_0.set_sample_rate(0, samp_rate)
        self.soapy_custom_sink_0.set_bandwidth(0, 0)
        self.soapy_custom_sink_0.set_antenna(0, '')
        self.soapy_custom_sink_0.set_frequency(0, freq)
        self.soapy_custom_sink_0.set_frequency_correction(0, 0)
        self.soapy_custom_sink_0.set_gain(0, tx_gain)
        self.soapy_custom_sink_0.set_dc_offset(0, 0)
        self.soapy_custom_sink_0.set_iq_balance(0, 0)
        self.send = _send_toggle_button = qtgui.MsgPushButton('Send', 'text',msg,"default","default")
        self.send = _send_toggle_button

        self.tabs_grid_layout_0.addWidget(_send_toggle_button, 0, 1, 1, 1)
        for r in range(0, 1):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(1, 2):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self._rx_text_tool_bar = Qt.QToolBar(self)

        if None:
            self._rx_text_formatter = None
        else:
            self._rx_text_formatter = lambda x: str(x)

        self._rx_text_tool_bar.addWidget(Qt.QLabel("RX:"))
        self._rx_text_label = Qt.QLabel(str(self._rx_text_formatter(self.rx_text)))
        self._rx_text_tool_bar.addWidget(self._rx_text_label)
        self.tabs_grid_layout_0.addWidget(self._rx_text_tool_bar, 2, 0, 1, 2)
        for r in range(2, 3):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(0, 2):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self._peer_text_tool_bar = Qt.QToolBar(self)

        if None:
            self._peer_text_formatter = None
        else:
            self._peer_text_formatter = lambda x: str(x)

        self._peer_text_tool_bar.addWidget(Qt.QLabel("Peer:"))
        self._peer_text_label = Qt.QLabel(str(self._peer_text_formatter(self.peer_text)))
        self._peer_text_tool_bar.addWidget(self._peer_text_label)
        self.tabs_grid_layout_0.addWidget(self._peer_text_tool_bar, 5, 0, 1, 2)
        for r in range(5, 6):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(0, 2):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self.pdu_tagged_stream_to_pdu_0 = pdu.tagged_stream_to_pdu(gr.types.byte_t, 'packet_len')
        self.pdu_pdu_to_tagged_stream_0 = pdu.pdu_to_tagged_stream(gr.types.byte_t, 'packet_len')
        self._link_text_tool_bar = Qt.QToolBar(self)

        if None:
            self._link_text_formatter = None
        else:
            self._link_text_formatter = lambda x: str(x)

        self._link_text_tool_bar.addWidget(Qt.QLabel("Link:"))
        self._link_text_label = Qt.QLabel(str(self._link_text_formatter(self.link_text)))
        self._link_text_tool_bar.addWidget(self._link_text_label)
        self.tabs_grid_layout_0.addWidget(self._link_text_tool_bar, 4, 0, 1, 2)
        for r in range(4, 5):
            self.tabs_grid_layout_0.setRowStretch(r, 1)
        for c in range(0, 2):
            self.tabs_grid_layout_0.setColumnStretch(c, 1)
        self.epy_block_0 = epy_block_0.blk(auto=auto, period=2.0, timeout=0.4, retries=5)
        self.digital_protocol_formatter_bb_0 = digital.protocol_formatter_bb(hdr_format, "packet_len")
        self.digital_gfsk_mod_0 = digital.gfsk_mod(
            samples_per_symbol=sps,
            sensitivity=sensitivity,
            bt=0.5,
            verbose=False,
            log=False,
            do_unpack=True)
        self.digital_gfsk_demod_0 = digital.gfsk_demod(
            samples_per_symbol=sps,
            sensitivity=sensitivity,
            gain_mu=0.175,
            mu=0.5,
            omega_relative_limit=0.005,
            freq_error=0.0,
            verbose=False,
            log=False)
        self.digital_correlate_access_code_xx_ts_0 = digital.correlate_access_code_bb_ts('0101010101010101010101010101010111100100001011000110011001011101',
          8, 'packet_len')
        self.blocks_tagged_stream_mux_0 = blocks.tagged_stream_mux(gr.sizeof_char*1, 'packet_len', 0)
        self.blocks_repack_bits_bb_0 = blocks.repack_bits_bb(1, 8, "packet_len", False, gr.GR_MSB_FIRST)
        self.blocks_multiply_const_vxx_0 = blocks.multiply_const_cc(0.5)
        self.blocks_msg_pair_to_var_3 = blocks.msg_pair_to_var(self.set_peer_text)
        self.blocks_msg_pair_to_var_2 = blocks.msg_pair_to_var(self.set_link_text)
        self.blocks_msg_pair_to_var_1 = blocks.msg_pair_to_var(self.set_tx_text)
        self.blocks_msg_pair_to_var_0 = blocks.msg_pair_to_var(self.set_rx_text)
        self.blocks_message_strobe_0 = blocks.message_strobe(pmt.intern("tick"), 100)


        ##################################################
        # Connections
        ##################################################
        self.msg_connect((self.blocks_message_strobe_0, 'strobe'), (self.epy_block_0, 'tick'))
        self.msg_connect((self.epy_block_0, 'rx_text'), (self.blocks_msg_pair_to_var_0, 'inpair'))
        self.msg_connect((self.epy_block_0, 'tx_text'), (self.blocks_msg_pair_to_var_1, 'inpair'))
        self.msg_connect((self.epy_block_0, 'link_text'), (self.blocks_msg_pair_to_var_2, 'inpair'))
        self.msg_connect((self.epy_block_0, 'peer_text'), (self.blocks_msg_pair_to_var_3, 'inpair'))
        self.msg_connect((self.epy_block_0, 'tx'), (self.pdu_pdu_to_tagged_stream_0, 'pdus'))
        self.msg_connect((self.pdu_tagged_stream_to_pdu_0, 'pdus'), (self.epy_block_0, 'rx'))
        self.msg_connect((self.send, 'pressed'), (self.epy_block_0, 'send'))
        self.connect((self.blocks_multiply_const_vxx_0, 0), (self.soapy_custom_sink_0, 0))
        self.connect((self.blocks_repack_bits_bb_0, 0), (self.pdu_tagged_stream_to_pdu_0, 0))
        self.connect((self.blocks_tagged_stream_mux_0, 0), (self.digital_gfsk_mod_0, 0))
        self.connect((self.digital_correlate_access_code_xx_ts_0, 0), (self.blocks_repack_bits_bb_0, 0))
        self.connect((self.digital_gfsk_demod_0, 0), (self.digital_correlate_access_code_xx_ts_0, 0))
        self.connect((self.digital_gfsk_mod_0, 0), (self.blocks_multiply_const_vxx_0, 0))
        self.connect((self.digital_protocol_formatter_bb_0, 0), (self.blocks_tagged_stream_mux_0, 0))
        self.connect((self.pdu_pdu_to_tagged_stream_0, 0), (self.blocks_tagged_stream_mux_0, 1))
        self.connect((self.pdu_pdu_to_tagged_stream_0, 0), (self.digital_protocol_formatter_bb_0, 0))
        self.connect((self.soapy_custom_source_0, 0), (self.digital_gfsk_demod_0, 0))


    def closeEvent(self, event):
        self.settings = Qt.QSettings("gnuradio/flowgraphs", "datalink")
        self.settings.setValue("geometry", self.saveGeometry())
        self.stop()
        self.wait()

        event.accept()

    def get_samp_rate(self):
        return self.samp_rate

    def set_samp_rate(self, samp_rate):
        self.samp_rate = samp_rate
        self.set_sensitivity(math.tau * self.fdev / self.samp_rate)

    def get_fdev(self):
        return self.fdev

    def set_fdev(self, fdev):
        self.fdev = fdev
        self.set_sensitivity(math.tau * self.fdev / self.samp_rate)

    def get_tx_text(self):
        return self.tx_text

    def set_tx_text(self, tx_text):
        self.tx_text = tx_text
        Qt.QMetaObject.invokeMethod(self._tx_text_label, "setText", Qt.Q_ARG("QString", str(self._tx_text_formatter(self.tx_text))))

    def get_tx_gain(self):
        return self.tx_gain

    def set_tx_gain(self, tx_gain):
        self.tx_gain = tx_gain
        self.soapy_custom_sink_0.set_gain(0, self.tx_gain)

    def get_sps(self):
        return self.sps

    def set_sps(self, sps):
        self.sps = sps

    def get_sensitivity(self):
        return self.sensitivity

    def set_sensitivity(self, sensitivity):
        self.sensitivity = sensitivity

    def get_rx_text(self):
        return self.rx_text

    def set_rx_text(self, rx_text):
        self.rx_text = rx_text
        Qt.QMetaObject.invokeMethod(self._rx_text_label, "setText", Qt.Q_ARG("QString", str(self._rx_text_formatter(self.rx_text))))

    def get_rx_gain(self):
        return self.rx_gain

    def set_rx_gain(self, rx_gain):
        self.rx_gain = rx_gain
        self.soapy_custom_source_0.set_gain(0, self.rx_gain)

    def get_peer_text(self):
        return self.peer_text

    def set_peer_text(self, peer_text):
        self.peer_text = peer_text
        Qt.QMetaObject.invokeMethod(self._peer_text_label, "setText", Qt.Q_ARG("QString", str(self._peer_text_formatter(self.peer_text))))

    def get_msg(self):
        return self.msg

    def set_msg(self, msg):
        self.msg = msg
        self._msg_callback(self.msg)
        self.send.set_new_msg(self.msg)

    def get_link_text(self):
        return self.link_text

    def set_link_text(self, link_text):
        self.link_text = link_text
        Qt.QMetaObject.invokeMethod(self._link_text_label, "setText", Qt.Q_ARG("QString", str(self._link_text_formatter(self.link_text))))

    def get_hdr_format(self):
        return self.hdr_format

    def set_hdr_format(self, hdr_format):
        self.hdr_format = hdr_format
        self.digital_protocol_formatter_bb_0.set_header_format(self.hdr_format)

    def get_freq(self):
        return self.freq

    def set_freq(self, freq):
        self.freq = freq
        self.soapy_custom_sink_0.set_frequency(0, self.freq)
        self.soapy_custom_source_0.set_frequency(0, self.freq)

    def get_auto(self):
        return self.auto

    def set_auto(self, auto):
        self.auto = auto
        self._auto_callback(self.auto)
        self.epy_block_0.auto = self.auto




def main(top_block_cls=datalink, options=None):

    qapp = Qt.QApplication(sys.argv)

    tb = top_block_cls()

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
