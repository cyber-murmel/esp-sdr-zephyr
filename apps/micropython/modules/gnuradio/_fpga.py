# SPDX-License-Identifier: GPL-3.0-or-later
"""The MCH2022 badge's FPGA behind qtgui's waterfall and frequency sinks (the fpga module, in
firmware built with a bitstream): it computes the spectrum of the monitored samples and draws
the plot areas itself, so LVGL keeps only the sinks' label rows. The FPGA has one spectrum
engine, one waterfall and one trace: the first waterfall sink and the first frequency sink with
the same FFT settings share it; any other sink draws in software."""

from gnuradio import _gui

try:
    import fpga as _hw

    _ok = _hw.ok()
except ImportError:
    _hw = None
    _ok = False

_users = []


def available():
    return _ok


def claim(sink):
    """Whether the sink gets the FPGA (call when it is placed on the screen)."""
    if not _ok:
        return False
    for s in _users:
        if s._accel == sink._accel or (s._size, s._win) != (sink._size, sink._win):
            return False
    _users.append(sink)
    return True


def release(sink=None):
    """Hand the FPGA back (one sink, or all of them: the screen is rebuilt or closes)."""
    if sink is None:
        _users.clear()
    elif sink in _users:
        _users.remove(sink)
    if _ok:
        _hw.stop()
        if _users:
            configure()


def _palette(src):
    import array

    pal = array.array("H", [0] * 256)
    for k in range(_hw.PALETTE_SIZE):
        pal[k] = src[k * len(src) // _hw.PALETTE_SIZE]
    pal[_hw.PAL_BG] = _gui.BG
    pal[_hw.PAL_TRACE] = _gui.TRACE[0]
    pal[_hw.PAL_CENTER] = _gui.DIM
    return pal


def configure():
    """Regions, levels and FFT settings from the sinks that share the FPGA."""
    if not _ok or not _users:
        return
    # (hasattr() would not do: a sink answers every name.)
    wf = [s for s in _users if s._accel == "waterfall"]
    tr = [s for s in _users if s._accel == "trace"]
    lead = wf[0] if wf else tr[0]
    x, y, w, h = lead._fpga_area
    # The waterfall's heat scale (set_gamma() changes it per sink), else the default one.
    _hw.palette(_palette(wf[0]._palette if wf else _gui.PALETTE))
    if wf:
        _hw.waterfall(*wf[0]._fpga_area)
    if tr:
        tx, ty, tw, th = tr[0]._fpga_area
        _hw.spectrum(tx, ty, tw, th, True)
    imin, imax = (wf[0]._imin, wf[0]._imax) if wf else (-140.0, 10.0)
    ymin, ymax = (tr[0]._ymin, tr[0]._ymax) if tr else (-140.0, 10.0)
    rows = tr[0]._fpga_area[3] if tr else 2
    # Lines on demand: one per feed(), that is per redraw of the lead sink, as in software (the
    # nominal sample rate says little: the radio may deliver in bursts with gaps).
    _hw.dsp(lead._size, lead._win, lead._avg, 0, w, imin, imax, ymax, ymin, rows)


def feed():
    _hw.feed()
