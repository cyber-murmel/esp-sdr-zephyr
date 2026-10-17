# SPDX-License-Identifier: GPL-3.0-or-later
"""analog: the signal source for transmit flowgraphs. sig_source_c runs in esdr's transmit
engine (C, a sine table with linear interpolation, 16-bit output), not in Python; its setters
take effect at once while the flowgraph transmits. The non-sinusoidal waveforms are real (the
Q part is zero)."""

import esdr
from gnuradio import gr

GR_CONST_WAVE = 100
GR_SIN_WAVE = 101
GR_COS_WAVE = 102
GR_SQR_WAVE = 103
GR_TRI_WAVE = 104
GR_SAW_WAVE = 105


class sig_source_c(gr.tx_source):
    def __init__(self, sampling_freq, waveform, wave_freq, ampl, offset=0, phase=0):
        super().__init__("sig_source_c")
        self.out_itemsize = gr.sizeof_gr_complex
        self._fs = float(sampling_freq)
        self._wave = int(waveform)
        self._freq = float(wave_freq)
        self._ampl = float(ampl)
        self._offset = complex(offset)
        if not GR_CONST_WAVE <= self._wave <= GR_SAW_WAVE:
            raise ValueError("sig_source_c: waveform {}".format(waveform))
        if phase:
            print("sig_source_c: the start phase is not supported on the board")

    def _gen(self, scale=1.0):
        cycles = self._freq / self._fs if self._fs else 0.0
        return (self._wave - GR_CONST_WAVE, cycles, self._ampl * scale,
                self._offset.real * scale, self._offset.imag * scale)

    def _tx_args(self):
        return {"gen": self._gen}

    def _update(self):
        if self._tb is not None and self._tb._running:
            self._tb._tx_live()

    def set_frequency(self, frequency):
        self._freq = float(frequency)
        self._update()

    def frequency(self):
        return self._freq

    def set_amplitude(self, ampl):
        self._ampl = float(ampl)
        self._update()

    def amplitude(self):
        return self._ampl

    def set_offset(self, offset):
        self._offset = complex(offset)
        self._update()

    def offset(self):
        return self._offset

    def set_waveform(self, waveform):
        self._wave = int(waveform)
        self._update()

    def waveform(self):
        return self._wave

    def set_sampling_freq(self, sampling_freq):
        self._fs = float(sampling_freq)
        self._update()

    def sampling_freq(self):
        return self._fs

    def set_phase(self, phase):
        pass
