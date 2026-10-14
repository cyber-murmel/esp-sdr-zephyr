# SPDX-License-Identifier: GPL-3.0-or-later
"""
Two-board regression test of the gapless streams. One board transmits a tone through
apps/osmosdr_serial, the other receives it through the coax link, then the roles swap. The
recording must hold no gap: no index jump or overflow flag from the device, and nothing
tools/gapcheck.py can find in the tone (a lost sample moves its phase, a zero-filled one
dents its amplitude). The receive side is whatever the image under test does: the CPU-paced
IQ estimator, or the diagnostic-bus ring (tests/regression/stream_gapless/testcase.yaml has one
scenario for each).

Guards the properties the streams were built for and that only ever showed on the bench:

- the estimator receive is exact at 250 kS/s while the CPU also feeds the USB FIFO;
- the diagnostic-bus ring delivers every one of its 40 MB/s, and its decimated stream reaches
  the host over a USB-Serial-JTAG IN endpoint that takes one 64-byte packet per visit (a
  consumer serving the FIFO once per ring block lost a quarter of the samples);
- the transmit stream keeps up with the host (underruns stay below a fraction of a percent).

Needs two connected boards on a coax link, and a hardware map with required_devices (see
doc/testing.md); twister flashes and resets both. The RF levels (below) suit a 30 dB attenuator
between the U.FL ports; another path may need other gains.
"""
import logging
import os
import sys
import threading
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "apps" / "osmosdr_serial" / "tools"))
import esdr_serial as es  # noqa: E402
import gapcheck  # noqa: E402

logger = logging.getLogger(__name__)

FREQ_HZ = 2437_000_000  # in both radios' range, away from the Wi-Fi channel the boards probe
TX_RATE = 125_000  # both chips transmit this reliably (250 kS/s outruns USB OUT)
# Receive rate -> TX tone: the received tone lands about 7 kHz lower (crystals, warm-up) and
# must stay inside the band of the narrowest rate.
RATES = {250_000: 37_000, 31_250: 5_000}
WARMUP_S = 15  # the transmitter's tone drifts a few ppm for its first half minute: gapcheck follows it
CAPTURE_S = 10
TX_TAIL_S = 5
# Chip name -> (receive gain index, transmit power step) on the 30 dB coax link: about 10 dB
# below full scale on the C5, with the C6 at the best level it showed.
RX_GAIN = {"c5": 40, "c6": 30}
TX_STEP = {"c5": 2, "c6": 2}
MIN_PEAK_DB = 30.0  # the tone over the median bin: below this there is no usable link
MAX_TX_UNDERRUN = 0.005  # of the samples the transmitter played

CHIPS = {"waveshare_esp32c5_zero": "c5", "xiao_esp32c6": "c6"}


def chip_of(dut) -> str:
    platform = dut.device_config.platform
    for prefix, chip in CHIPS.items():
        if platform.startswith(prefix):
            return chip
    pytest.fail(f"no stream settings for platform {platform}")


def env_int(name: str, default: int) -> int:
    return int(os.environ.get(name, default))


@pytest.fixture(scope="module")
def ports(duts):
    """The two boards' serial ports, after twister flashed them. The harness holds the port open
    and reads the console; the stream is binary, so the harness lets go of it."""
    out = {}
    for dut in duts:
        out[chip_of(dut)] = dut.device_config.serial_configs[0].port
        dut.disconnect()
    assert len(out) == 2, f"need a C5 and a C6, got {sorted(out)}"
    time.sleep(4)  # the application boots again after the flash
    for chip, port in out.items():
        for attempt in range(10):
            try:
                d = es.Esdr(port)
                try:
                    info = d.info()
                finally:
                    d.close()
                break
            except TimeoutError:
                time.sleep(1)
        else:
            pytest.fail(f"{chip}: no answer from osmosdr_serial on {port}")
        logger.info("%s on %s: %s", chip, port, info["firmware"].rstrip(b"\0").decode())
    return out


def stream_stats(d) -> dict:
    s = d.stats()
    return {k: s[k] for k in ("rx_overflows", "rx_ring_lost_pairs", "ring_errors", "tx_samples",
                              "tx_underruns")}


@pytest.mark.parametrize("rate", RATES)
@pytest.mark.parametrize("direction", ["c6_to_c5", "c5_to_c6"])
def test_stream(ports, direction, rate):
    tx_chip, rx_chip = direction.split("_to_")
    seconds = WARMUP_S + env_int("ESP_SDR_STREAM_CAPTURE_S", CAPTURE_S) + TX_TAIL_S
    capture_s = env_int("ESP_SDR_STREAM_CAPTURE_S", CAPTURE_S)
    tx = es.Esdr(ports[tx_chip])
    rx = es.Esdr(ports[rx_chip])
    sent = {}

    def transmit():
        try:
            sent["n"], sent["s"] = es.transmit(
                tx, TX_RATE, seconds, RATES[rate], 8, FREQ_HZ,
                env_int(f"ESP_SDR_STREAM_{tx_chip.upper()}_TX_STEP", TX_STEP[tx_chip]))
        except Exception as e:  # reported below, after the receiver is done
            sent["error"] = e

    try:
        tx_before, rx_before = stream_stats(tx), stream_stats(rx)
        thread = threading.Thread(target=transmit)
        thread.start()
        time.sleep(WARMUP_S)
        x, flags, jumps, elapsed = es.capture(
            rx, rate, capture_s, 8, FREQ_HZ,
            env_int(f"ESP_SDR_STREAM_{rx_chip.upper()}_RX_GAIN", RX_GAIN[rx_chip]))
        thread.join(timeout=seconds + 30)
        tx_after, rx_after = stream_stats(tx), stream_stats(rx)
    finally:
        rx.close()
        tx.close()

    assert "error" not in sent, f"transmit failed: {sent.get('error')!r}"
    delta = {k: (tx_after if k.startswith("tx_") else rx_after)[k] -
             (tx_before if k.startswith("tx_") else rx_before)[k] for k in tx_after}
    r = gapcheck.analyse(x, rate)
    evidence = (f"{direction} at {rate} S/s: {len(x)} samples in {elapsed:.2f} s, flags {flags:#x}, "
                f"{len(jumps)} index jumps, tone {r['tone_hz_first'] / 1e3:+.2f} -> "
                f"{r['tone_hz_last'] / 1e3:+.2f} kHz at {r['peak_db']:.1f} dB, device {delta}")

    assert r["peak_db"] >= MIN_PEAK_DB and abs(r["tone_hz"]) < 0.45 * rate, \
        f"no usable link, or the tone left the band (check the cabling, the attenuator and the " \
        f"gains): {evidence}"
    assert len(x) >= 0.98 * rate * capture_s, f"samples missing: {evidence}"
    assert not jumps, f"index jumps (first: {jumps[:3]}): {evidence}"
    # Bit 0 (a gap) is set on the first block after the stream starts; bit 1 (overflow) is a drop.
    assert flags & 0x2 == 0, f"the device dropped samples (overflow): {evidence}"
    assert delta["rx_overflows"] == 0 and delta["rx_ring_lost_pairs"] == 0, \
        f"the receiver lost samples: {evidence}"
    assert gapcheck.is_clean(r), f"gaps in the tone: {r['phase_steps']} {r['amplitude_dips']}: {evidence}"
    assert delta["tx_samples"] > 0
    assert delta["tx_underruns"] <= MAX_TX_UNDERRUN * (delta["tx_samples"] + delta["tx_underruns"]), \
        f"the transmitter ran dry: {evidence}"
    assert sent["s"] < 1.2 * seconds, f"the host fed the transmitter at {sent['n'] / sent['s']:.0f} S/s"
