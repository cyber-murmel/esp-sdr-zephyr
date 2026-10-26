# SPDX-License-Identifier: GPL-3.0-or-later
"""The radio's crystal calibration against LTE, measured on this chip.

On the WHY2025 badge the ESP32-P4 measures its ESP32-C6's crystal from the C6's 2 MS/s stream
with the library's estimator (esdr.lte_detect) and hands the result to the C6 (ESDR_REQ_SET_CAL),
which corrects its tuning from then on and follows a model over its die temperature once points
at several temperatures exist. The C6 has no room for a settings store, so the record lives
here, in FILE, and goes to the radio again at every start (restore(), from the launcher).

    import ltecal
    ltecal.calibrate()      # survey LTE band 1, find a cell, measure, add a point, keep it
    ltecal.show()           # the correction in force, the temperature, the model's points
    ltecal.set_ppb(-5500)   # a constant by hand (the points go)
    ltecal.clear()
    ltecal.restore()        # FILE's record to the radio (session only there)

Terms as in doc/lte-calibration.md: ppb is the crystal's error, positive when the LO runs high;
the correction in force is what the radio tunes with; an estimate is relative to that.
"""

import os
import struct
import time

import esdr

GET_CAL, SET_CAL = 0x19, 0x1A
CAL_ADD, CAL_SET, CAL_CLEAR, CAL_LOAD = 0, 1, 2, 3
TEMP_UNKNOWN = -32768
POINTS = 8
# struct esdr_cal: corr_ppb, lo_residual_hz, temp_dc, stored, last_store, reserved; then the
# record: ppb, spread_ppb, carrier_khz, cell_id, estimates, temp_dc, points, reserved and
# POINTS points (temp_dc, reserved, ppb); struct esdr_cal_meas for CAL_ADD.
_HDR = "<iihhhH"
_REC = "<iiIhHhBB"
_PT = "<hhi"
_MEAS = "<iiIhHhH"
HDR_SIZE = struct.calcsize(_HDR)
REC_SIZE = struct.calcsize(_REC) + POINTS * struct.calcsize(_PT)
CAL_SIZE = HDR_SIZE + REC_SIZE
FILE = "/FLASH:/esdr_cal.bin"
# The stream rate the estimator takes, and a capture of 20 ms: four synchronization pairs.
FS = 2000000
N = 40000
N_SURVEY = 4000
MAX_SPREAD = 500
# With 1 MHz survey steps a carrier is within 500 kHz of the LO (as the library searches).
MAX_OFFSET = 520000


def _temp(dc):
    return "?" if dc == TEMP_UNKNOWN else "%.1f" % (dc / 10)


def _rec(b):
    f = struct.unpack_from(_REC, b)
    n = min(f[6], POINTS)
    pts = [struct.unpack_from(_PT, b, struct.calcsize(_REC) + 8 * i) for i in range(n)]
    return dict(ppb=f[0], spread_ppb=f[1], carrier_khz=f[2], cell_id=f[3], estimates=f[4],
                temp_dc=f[5], points=[(p[0], p[2]) for p in pts])


def cal():
    """The radio's answer to ESDR_REQ_GET_CAL as a dict, the record under 'rec'."""
    b = esdr.request(GET_CAL, 0, None, CAL_SIZE)
    if len(b) != CAL_SIZE:
        raise OSError("short GET_CAL reply")
    h = struct.unpack_from(_HDR, b)
    return dict(corr_ppb=h[0], lo_residual_hz=h[1], temp_dc=h[2], stored=h[3], last_store=h[4],
                rec=_rec(memoryview(b)[HDR_SIZE:]))


def show():
    c = cal()
    r = c["rec"]
    print("correction %d ppb, LO %+d Hz off the frequency asked, die %s C"
          % (c["corr_ppb"], c["lo_residual_hz"], _temp(c["temp_dc"])))
    print("calibration %d ppb: cell %d at %d kHz, %d estimates, spread %d ppb, %s C"
          % (r["ppb"], r["cell_id"], r["carrier_khz"], r["estimates"], r["spread_ppb"],
             _temp(r["temp_dc"])))
    for t, p in r["points"]:
        print("  point %s C: %d ppb" % (_temp(t), p))
    try:
        kept = os.stat(FILE)[6] == REC_SIZE
    except OSError:
        kept = False
    print("stored on the radio: %s; kept in %s: %s"
          % ({0: "yes", -2: "no"}.get(c["stored"], "no store"), FILE, "yes" if kept else "no"))
    return c


def keep():
    """The record in force on the radio into FILE."""
    b = esdr.request(GET_CAL, 0, None, CAL_SIZE)
    with open(FILE, "wb") as f:
        f.write(bytes(b[HDR_SIZE:HDR_SIZE + REC_SIZE]))


def restore():
    """FILE's record to the radio; True when there was one."""
    try:
        with open(FILE, "rb") as f:
            b = f.read()
    except OSError:
        return False
    if len(b) != REC_SIZE:
        return False
    esdr.request(SET_CAL, CAL_LOAD, b)
    return True


def set_ppb(ppb):
    esdr.request(SET_CAL, CAL_SET, struct.pack("<i", int(ppb)))
    keep()


def clear():
    esdr.request(SET_CAL, CAL_CLEAR)
    try:
        os.remove(FILE)
    except OSError:
        pass


def _order(pw, start, candidates):
    """The LO steps to search, as the library's survey: the runs of steps 6 dB over the median
    power, the strongest run first, each from its centre outwards."""
    s = sorted(pw)
    thr = 4 * s[len(s) // 2]
    used = [False] * len(pw)
    order = []
    while len(order) < candidates:
        best, lo, hi = 0, 0, 0
        i = 0
        while i < len(pw):
            if pw[i] < thr or used[i]:
                i += 1
                continue
            j = i
            tot = 0
            while j < len(pw) and pw[j] >= thr and not used[j]:
                tot += pw[j]
                j += 1
            if tot / (j - i) > best:
                best, lo, hi = tot / (j - i), i, j
            i = j
        if best == 0:
            break
        mid = (lo + hi - 1) // 2
        for d in range(hi - lo + 1):
            if mid + d < hi and len(order) < candidates:
                order.append(start + mid + d)
            if d > 0 and mid - d >= lo and len(order) < candidates:
                order.append(start + mid - d)
        for k in range(lo, hi):
            used[k] = True
    return order


def survey(start=2110, stop=2170, gain=30):
    """Received power (mean I^2 + Q^2 of three 2 ms captures) at each MHz, as a list."""
    esdr.init()
    old_gain = esdr.rx_gain()
    esdr.rx_gain(gain)
    buf = bytearray(4 * N_SURVEY)
    pw = []
    try:
        for mhz in range(start, stop + 1):
            esdr.tune(mhz * 1000000)
            p = 0.0
            for k in range(3):
                esdr.rx_start(FS, buf, N_SURVEY)
                esdr.rx_wait(2000)
                p += esdr.iq_power(buf)
            pw.append(p / 3)
    finally:
        esdr.rx_gain(old_gain)
    return pw


def measure(mhz, buf=None, verbose=False):
    """One capture at mhz through the estimator: the dict of esdr.lte_detect() with 'ppb'
    on top of the correction in force ('corr_ppb'), the die temperature ('temp_dc') and
    'good' (a cell decoded with margin whose reference signals cohere: a false SSS decode on
    noise has none), or None without a PSS. About 20 s on the P4 (the transforms run from
    PSRAM)."""
    if buf is None:
        buf = bytearray(4 * N)
    esdr.tune(mhz * 1000000)
    esdr.rx_start(FS, buf, N)
    esdr.rx_wait(3000)
    centre = esdr.radio_state()["centre"]
    c = cal()
    r = esdr.lte_detect(buf, FS, centre, max_offset=MAX_OFFSET)
    if r is None:
        return None
    r["good"] = r["valid"] and r["crs_symbols"] > 0
    r["delta_ppb"] = r["ppb"]
    r["ppb"] = c["corr_ppb"] + r["delta_ppb"]
    r["corr_ppb"] = c["corr_ppb"]
    r["temp_dc"] = c["temp_dc"]
    r["lo_hz"] = centre
    if verbose:
        print("  LO %d Hz: cell %d %s, sss %.2f, crs %d at %.2f, %+.1f ppb (%+.1f on %d), %s C"
              % (centre, r["cell"], "good" if r["good"] else "valid" if r["valid"] else "no",
                 r["sss"], r["crs_symbols"], r["crs_metric"], r["ppb"], r["delta_ppb"],
                 r["corr_ppb"], _temp(r["temp_dc"])))
    return r


def calibrate(start=2110, stop=2170, gain=30, candidates=6, tries=4, estimates=5, verbose=True):
    """Measure the radio's crystal against an LTE cell and keep the result, as
    esp_sdr_lte_calibrate() does on a chip with the radio on it: a power survey of the band
    (the LTE band 1 downlink by default), captures of 20 ms at the strongest places until a cell
    decodes, then `estimates` from that cell. Their median becomes a point of the radio's
    temperature model (ESDR_CAL_ADD), applied from its next tuning, and the record goes to
    FILE. Returns the result as a dict, or None without a cell."""
    t0 = time.ticks_ms()
    pw = survey(start, stop, gain)
    order = _order(pw, start, candidates)
    if verbose:
        print("survey %d to %d MHz: %d places to search (%d ms)"
              % (start, stop, len(order), time.ticks_diff(time.ticks_ms(), t0)))
    old_gain = esdr.rx_gain()
    esdr.rx_gain(gain)
    buf = bytearray(4 * N)
    est, temps = [], []
    cell = None
    raster = 0
    captures = decoded = 0
    try:
        for mhz in order:
            budget = tries
            t = 0
            while t < budget and len(est) < estimates:
                t += 1
                captures += 1
                r = measure(mhz, buf, verbose)
                if r is None or not r["good"]:
                    continue
                decoded += 1
                if cell is None:
                    cell, raster = r["cell"], r["raster_khz"]
                    # A cell here: give it the captures the estimates need.
                    budget = tries * estimates
                    if verbose:
                        print("cell %d at %d kHz (LO %d MHz)" % (cell, raster, mhz))
                if r["cell"] != cell or r["raster_khz"] != raster:
                    continue
                est.append(r["ppb"])
                if r["temp_dc"] != TEMP_UNKNOWN:
                    temps.append(r["temp_dc"])
            if cell is not None:
                break
    finally:
        esdr.rx_gain(old_gain)
    if not est:
        if verbose:
            print("no cell decoded in %d captures" % captures)
        return None
    est.sort()
    ppb = int(round(est[len(est) // 2]))
    spread = int(round((est[-1] - est[0]) / 2))
    temp = (temps[0] + temps[-1]) // 2 if temps else TEMP_UNKNOWN
    res = dict(ppb=ppb, spread_ppb=spread, cell=cell, carrier_khz=raster, estimates=len(est),
               captures=captures, decoded=decoded, temp_dc=temp,
               temp_drift_dc=temps[-1] - temps[0] if temps else 0,
               ms=time.ticks_diff(time.ticks_ms(), t0))
    if spread > MAX_SPREAD:
        if verbose:
            print("estimates disagree: spread %d ppb, nothing applied" % spread)
        res["applied"] = False
        return res
    esdr.request(SET_CAL, CAL_ADD,
                 struct.pack(_MEAS, ppb, spread, raster, cell, len(est), temp, 0))
    keep()
    c = cal()
    res["applied"] = True
    res["corr_ppb"] = c["corr_ppb"]
    res["points"] = len(c["rec"]["points"])
    if verbose:
        print("crystal %+d ppb (spread %d, %d estimates of %d captures, %s C): correction now "
              "%d ppb, %d points, %d ms"
              % (ppb, spread, len(est), captures, _temp(temp), c["corr_ppb"], res["points"],
                 res["ms"]))
    return res
