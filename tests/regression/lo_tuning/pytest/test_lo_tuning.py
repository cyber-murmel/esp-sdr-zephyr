# SPDX-License-Identifier: GPL-3.0-or-later
"""
Two-board regression test of the LO tuning (lib/esp_sdr/src/esp_sdr.c): a real
xiao_esp32s3 transmits a tone, a real xiao_esp32c6 captures it and reports where it
landed, at pairs of LO frequencies that overlap.

The tone must show up at (TX LO - RX LO) + TONE_HZ. Pairs 10 MHz apart, not only
equal ones, matter: two PLLs that both miss their frequency the same way still agree
with each other at equal LOs, but put the tone in the wrong place once they differ.

What it guards:
- the 5/6 LO divider below 2210 MHz in both directions (transmit used to leave it
  off and go out at 6/5 of the frequency);
- the switch between divider and direct tuning at 2209/2210 MHz;
- esp_sdr_set_freq()'s -ERANGE: reported far outside the tuning range, and never
  missing where the PLL does not lock (checked at the S3's window edges).

Both boards run apps/rfprobe: its `probe` shell tunes, loops the tone on the S3 and
dumps a raw capture from the C6, which is analysed here (analyse()). Needs two
connected boards and a hardware map with required_devices (see doc/testing.md);
twister flashes and resets both. The levels depend on the RF path
between the boards (the C6 image listens on its U.FL port): RX_GAIN and TX_STEP are
set for this desk, and the radios fixture checks the link at 2412 MHz first, so a weak
link fails as such instead of as misplaced tones.
"""
import base64
import logging
import re

import numpy as np
import pytest
from twister_harness import Shell

logger = logging.getLogger(__name__)

TONE_HZ = 3_000_000
STEP_MHZ = 10
# Fixed C6 receive gain index, S3 transmit power step, for this desk's RF path: no
# clipping at the weakest step, tones 35 to 51 dB above the floor. Another RF path
# (attenuator, antennas) may need others.
RX_GAIN = 40
TX_STEP = 0
# Tone level above the local noise floor (median of +-1 MHz) at the expected offset.
ARRIVES_DB = 20.0
ABSENT_DB = 12.0
# Where the strongest tone may sit: crystal tolerance of both boards plus an FFT bin.
MATCH_HZ = 150_000
ERANGE = -34

# The capture: 8192 samples at 80 MS/s (rate 0 of `probe rx`), a Welch average of eight
# Hann-windowed 1024-point FFTs.
RX_SAMPLES = 8192
RX_FS_HZ = 80_000_000
NFFT = 1024
# The receiver's own DC and LO leakage.
DC_SKIP_HZ = 150_000
# Local noise floor: median of the bins within +-1 MHz (27 bins).
FLOOR_HALF = 13

F_RE = re.compile(r"^f (-?\d+) ret (-?\d+)", re.M)
RX_RE = re.compile(r"^rx n (\d+) f (\d+) fs (\d+)", re.M)
TONE_ON_RE = re.compile(r"^tone on ", re.M)
TONE_OFF_RE = re.compile(r"^tone off", re.M)


def run(shell: Shell, command: str, pattern: re.Pattern) -> re.Match:
    text = "\n".join(shell.exec_command(command))
    m = pattern.search(text)
    if not m:
        pytest.fail(f"{command!r}: no {pattern.pattern!r} in: {text}")
    return m


def capture(shell: Shell) -> tuple[np.ndarray, int]:
    """One `probe rx` capture: the raw words as complex I/Q (-512 to 511), and the LO."""
    lines = shell.exec_command(f"probe rx {RX_SAMPLES} 0")
    m = RX_RE.search("\n".join(lines))
    if not m or int(m.group(1)) != RX_SAMPLES or int(m.group(3)) != RX_FS_HZ:
        pytest.fail(f"probe rx: no capture of {RX_SAMPLES} samples at 80 MS/s in: "
                    f"{lines[:3]} ... {lines[-3:]}")
    data = b"".join(base64.b64decode(line[2:].strip()) for line in lines
                    if line.startswith("d "))
    w = np.frombuffer(data, dtype="<u4")
    if len(w) != RX_SAMPLES:
        pytest.fail(f"probe rx: {len(w)} of {RX_SAMPLES} samples decoded")
    i = ((w >> 10) & 0x3FF).astype(np.int32)
    q = (w & 0x3FF).astype(np.int32)
    i[i >= 512] -= 1024
    q[q >= 512] -= 1024
    return i + 1j * q, int(m.group(2))


def analyse(x: np.ndarray, expect_hz: int) -> dict:
    """The strongest tone outside DC and the level at expect_hz, each against the median of
    the bins within 1 MHz, so the C6's uneven noise floor does not count as a tone. Levels
    in dB over that floor, rms in raw units after the DC is taken off."""
    clip = int(np.sum((np.abs(x.real) >= 511) | (np.abs(x.imag) >= 511)))
    x = x - x.mean()
    rms = float(np.sqrt(np.mean(np.abs(x) ** 2)))
    segs = x.reshape(-1, NFFT) * np.hanning(NFFT + 1)[:NFFT]
    psd = np.sum(np.abs(np.fft.fft(segs, axis=1)) ** 2, axis=0)
    floor = np.median([np.roll(psd, d) for d in range(-FLOOR_HALF, FLOOR_HALF + 1)], axis=0)
    db = 10 * np.log10(np.maximum(psd / np.maximum(floor, 1e-20), 1e-6))
    f = np.fft.fftfreq(NFFT, 1 / RX_FS_HZ)
    peak = int(np.argmax(np.where(np.abs(f) > DC_SKIP_HZ, db, -np.inf)))
    near = np.abs(f - expect_hz) <= MATCH_HZ
    if not near.any():
        pytest.fail(f"{expect_hz} Hz is outside the capture")
    match = int(np.argmax(np.where(near, db, -np.inf)))
    return {"peak_hz": float(f[peak]), "peak_db": float(db[peak]), "expect_hz": expect_hz,
            "expect_db": float(db[match]), "rms": rms, "clip": clip}


class Radios:
    def __init__(self, c6: Shell, s3: Shell):
        self.c6 = c6
        self.s3 = s3

    @staticmethod
    def tune(shell: Shell, mhz: int) -> int:
        return int(run(shell, f"probe f {mhz}", F_RE).group(2))

    def measure(self, tx_mhz: int, rx_mhz: int, expect_hz: int | None = None,
                tone: bool = True) -> dict:
        """Tunes both boards, plays the tone on the S3 (unless tone is False) and
        captures on the C6. expect_hz defaults to where the LOs put the tone."""
        if expect_hz is None:
            expect_hz = (tx_mhz - rx_mhz) * 1_000_000 + TONE_HZ
        tx_ret = self.tune(self.s3, tx_mhz)
        rx_ret = self.tune(self.c6, rx_mhz)
        if tone:
            run(self.s3, f"probe tone on {TONE_HZ}", TONE_ON_RE)
        try:
            x, _ = capture(self.c6)
        finally:
            if tone:
                run(self.s3, "probe tone off", TONE_OFF_RE)
        return {"tx": tx_mhz, "rx": rx_mhz, "tx_ret": tx_ret, "rx_ret": rx_ret, "tone": tone,
                **analyse(x, expect_hz)}


def arrives(m: dict) -> bool:
    return m["expect_db"] >= ARRIVES_DB and abs(m["peak_hz"] - m["expect_hz"]) <= MATCH_HZ


def describe(m: dict) -> str:
    return (f"tx {m['tx']} MHz (ret {m['tx_ret']}), rx {m['rx']} MHz (ret {m['rx_ret']}): "
            f"{m['expect_db']:.1f} dB at {m['expect_hz'] / 1e6:+.3f} MHz, strongest "
            f"{m['peak_db']:.1f} dB at {m['peak_hz'] / 1e6:+.3f} MHz, rms {m['rms']:.1f}, "
            f"clip {m['clip']}")


def check_pairs(radios: Radios, pairs) -> None:
    results = [radios.measure(a, b) for a, b in pairs]
    for m in results:
        logger.info(describe(m))
    bad = [m for m in results if m["tx_ret"] or m["rx_ret"] or not arrives(m)]
    assert not bad, "tone not where the LOs put it:\n" + "\n".join(describe(m) for m in bad)


@pytest.fixture(scope="module")
def radios(shells: list[Shell]) -> Radios:
    assert len(shells) > 1, (
        "this test needs two connected boards (a hardware map with required_devices; "
        "see doc/testing.md)"
    )
    # shells[0] is the scenario's own board, the C6; shells[1] the S3, the only one
    # with a transmitter.
    c6, s3 = shells[0], shells[1]
    run(s3, "probe tone off", TONE_OFF_RE)
    run(c6, f"probe gain {RX_GAIN}", re.compile(rf"^rxgain {RX_GAIN} ret 0", re.M))
    run(s3, f"probe gain -1 {TX_STEP}", re.compile(rf"^txgain {TX_STEP} ret 0", re.M))
    radios = Radios(c6, s3)
    # 2412 MHz is a Wi-Fi channel, tuned the vendor's way: nothing under test here.
    m = radios.measure(2412, 2412)
    logger.info("link check: %s", describe(m))
    if m["tx_ret"] or m["rx_ret"] or not arrives(m):
        pytest.fail("no usable link at 2412 MHz, where tuning is not in question: "
                    f"{describe(m)}. Check the RF path between the boards (the C6 "
                    "listens on its U.FL port), RX_GAIN and TX_STEP.")
    return radios


# With their +10 MHz partners and the 2205 to 2215 MHz pairs below, these put the PLL
# between 2210 and 2651 MHz (6/5 of the frequency below 2210 MHz, the frequency itself
# above): the envelope upstream esp-sdr qualified on all chips.
IN_RANGE_MHZ = (1900, 2000, 2100, 2200, 2300, 2412, 2600)


@pytest.mark.parametrize("mhz", IN_RANGE_MHZ)
def test_tone_lands_where_the_los_put_it(radios, mhz):
    check_pairs(radios, ((mhz, mhz), (mhz, mhz + STEP_MHZ), (mhz + STEP_MHZ, mhz)))


def test_divider_switch_both_ways(radios):
    """Divider on one board, direct tuning on the other, both ways round."""
    check_pairs(radios, ((2209, 2210), (2210, 2209), (2205, 2215), (2215, 2205)))


def test_divided_transmit_is_not_at_six_fifths(radios):
    """Before the transmit path selected the divider, a tone at 2000 MHz went out at
    the undivided PLL frequency, 2400 MHz plus its offset."""
    check_pairs(radios, ((2000, 2000),))
    m = radios.measure(2000, 2400, expect_hz=TONE_HZ)
    logger.info(describe(m))
    assert m["expect_db"] < ABSENT_DB, "a 2000 MHz transmission shows up at 2400 MHz: " + \
        describe(m)


def test_silent_transmitter_is_not_detected(radios):
    """The detector itself: no tone, nothing at the offset."""
    m = radios.measure(2412, 2412, tone=False)
    logger.info(describe(m))
    assert m["expect_db"] < ABSENT_DB, "a tone found with the transmitter silent: " + \
        describe(m)


def test_far_out_of_range_is_reported(radios):
    for name, shell in (("S3", radios.s3), ("C6", radios.c6)):
        rets = {mhz: Radios.tune(shell, mhz) for mhz in (1000, 1700, 3000, 5000, 2412)}
        assert all(rets[mhz] == ERANGE for mhz in (1000, 1700, 3000, 5000)), \
            f"{name}: expected -ERANGE far outside the tuning range: {rets}"
        assert rets[2412] == 0, f"{name}: 2412 MHz: {rets[2412]}"


def window_edge(shell: Shell, start: int, stop: int, step: int) -> int:
    """The last MHz from start towards stop that tunes without -ERANGE. start must tune
    (the other tests already need it to); reaching stop without -ERANGE fails: no chip
    measured locks that far out."""
    last = None
    for mhz in range(start, stop, step):
        ret = Radios.tune(shell, mhz)
        if ret == ERANGE:
            assert last is not None, f"{start} MHz already reports -ERANGE"
            return last
        assert ret == 0, f"probe f {mhz}: {ret}"
        last = mhz
    pytest.fail(f"no -ERANGE from {start} to {stop} MHz")


EDGE_POINTS = 3
# The C6 sits this much further inside the S3's window; the next inset if it reports
# -ERANGE there itself.
EDGE_RX_INSETS_MHZ = (8, 16)


def edge_pairs(radios: Radios, edge: int, inward: int) -> tuple[list, list]:
    """Measures the last EDGE_POINTS MHz inside one end of the S3's window (inward: +1 at
    the bottom, -1 at the top). Returns the pairs both boards tuned without -ERANGE, and
    those where the S3 reported it on the re-tune (its last MHz can flip)."""
    checked, flipped = [], []
    for d in range(EDGE_POINTS):
        a = edge + inward * d
        for inset in EDGE_RX_INSETS_MHZ:
            b = a + inward * inset
            ret = Radios.tune(radios.c6, b)
            if ret == 0:
                break
            assert ret == ERANGE, f"C6 probe f {b}: {ret}"
        else:
            pytest.fail(f"the C6 reports -ERANGE at every inset from {a} MHz")
        m = radios.measure(a, b)
        logger.info(describe(m))
        if m["tx_ret"] == ERANGE:
            flipped.append(m)
            continue
        assert m["tx_ret"] == 0 and m["rx_ret"] == 0, describe(m)
        checked.append(m)
    return checked, flipped


def test_no_error_means_the_tone_arrives(radios):
    """-ERANGE errs on the safe side: wherever both boards tune without it, the tone
    must arrive. Checked at the last EDGE_POINTS MHz inside each end of the S3's window,
    found by tuning MHz by MHz, with the C6 a few MHz further in."""
    lo = window_edge(radios.s3, 1880, 1700, -1)
    hi = window_edge(radios.s3, 2610, 2950, 1)
    logger.info("S3 tunes without -ERANGE from %d to %d MHz", lo, hi)
    problems = []
    for name, edge, inward in (("lower", lo, 1), ("upper", hi, -1)):
        checked, flipped = edge_pairs(radios, edge, inward)
        if len(checked) < EDGE_POINTS - 1:
            problems.append(f"{name} edge: only {len(checked)} of {EDGE_POINTS} pairs tuned "
                            "without -ERANGE")
        problems += [f"{name} edge, no -ERANGE but no tone: {describe(m)}"
                     for m in checked if not arrives(m)]
    assert not problems, "\n".join(problems)
