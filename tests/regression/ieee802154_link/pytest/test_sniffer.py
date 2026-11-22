# SPDX-License-Identifier: GPL-3.0-or-later
"""
Sniffer regression: apps/wpan's `sniffer` mode on both radios (doc: apps/wpan/README.rst,
section Sniffer). One board sniffs while the other sends test frames, then the roles swap.

Checked per direction:

- every capture line parses with the Wireshark extcap's own code (apps/wpan/tools/wpan_sniffer.py),
  and the pcap that code builds has a valid FCS on every frame (tshark, when installed);
- the capture holds every test frame the sniffing board's `wpan status` counted;
- the firmware queued every frame it was handed (`drop=0`, `enq` == frames in the capture);
- the transmitter lost at most MAX_TX_ERRORS frames of its own.

Shares the module-scoped `shells` fixture with test_interop.py (tests/regression/conftest.py),
and leaves the radios as that test expects, in either run order.
"""
import importlib.util
import logging
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest
from test_interop import MAX_TX_ERRORS, RX_RE, TX_RE  # pytest's rootdir import mode
from twister_harness import DeviceAdapter, Shell

logger = logging.getLogger(__name__)

CHANNEL = 15
# Within the S3 port's measured capacity of about 25 KB/s: the interop test's 100-byte 5 ms
# frames saturate its output.
FRAMES = 100
INTERVAL_MS = 10
PAYLOAD = 20
SETTLE_S = 1.0  # the software radio retunes and restarts its receive ring on a channel change
IDLE_S = 1.5  # no capture line for this long: the sniffer's queue has drained
MAGIC_OFFSET = 9  # "X154" in apps/wpan's test frames, then the counter, both LE32
COUNTER_OFFSET = 13
STATS_RE = re.compile(r"enq=(\d+) drop=(\d+) tx_rec=(\d+)")

EXTCAP = Path(__file__).resolve().parents[4] / "apps" / "wpan" / "tools" / "wpan_sniffer.py"


def load_extcap():
    spec = importlib.util.spec_from_file_location("wpan_sniffer", EXTCAP)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def restore(shells: list[Shell]) -> None:
    """The state test_interop.py's radios_on fixture sets. `sniffer stop` leaves the radio off
    and promiscuous mode on."""
    for s in shells:
        s.exec_command("wpan set promisc 0")
        s.exec_command("wpan set print 0")
        s.exec_command("wpan on")
    shells[0].exec_command("wpan set pan 0xabcd addr 0x0002")
    shells[1].exec_command("wpan set pan 0xabcd addr 0x0001")


@pytest.fixture(scope="module", autouse=True)
def radios_prepared(shells: list[Shell]):
    assert len(shells) > 1, "this test needs two connected boards (see doc/dev/testing.md)"
    for shell, radio in zip(shells, ("radio ieee802154 ", "radio esp_sdr_154 ")):
        lines = shell.exec_command("wpan status")
        assert any(radio in line for line in lines), f"expected '{radio.strip()}' in: {lines}"
    restore(shells)
    yield
    restore(shells)


def collect(dut: DeviceAdapter, min_s: float, max_s: float) -> list[str]:
    """Reads what the board prints on its own for at least min_s, then until it has been quiet
    for IDLE_S (at most max_s in all). Capture lines arrive asynchronously while another board
    sends, so this cannot go through Shell.exec_command, which clears the buffer first."""
    lines: list[str] = []
    start = last = time.monotonic()
    while True:
        now = time.monotonic()
        # DeviceAdapter.readlines returns what is buffered now, without waiting.
        new = dut.readlines(print_output=False)
        if new:
            lines.extend(new)
            last = now
        if now - start >= max_s or (now - start >= min_s and now - last >= IDLE_S):
            return lines
        time.sleep(0.1)


def frame_counter(psdu: bytes) -> int | None:
    """The counter of one of apps/wpan's test frames, None for any other frame."""
    if len(psdu) < COUNTER_OFFSET + 4:
        return None
    if psdu[MAGIC_OFFSET:MAGIC_OFFSET + 4] != b"X154":
        return None
    return struct.unpack_from("<I", psdu, COUNTER_OFFSET)[0]


def check_pcap(ext, records: list[bytes], tmp_path: Path, name: str, problems: list[str]) -> None:
    pcap = tmp_path / f"{name}.pcap"
    pcap.write_bytes(ext.pcap_file_header(ext.DLT_IEEE802_15_4_TAP) + b"".join(records))
    logger.info("capture written to %s", pcap)
    tshark = shutil.which("tshark")
    if tshark is None:
        logger.warning("tshark not installed: FCS check of %s skipped", pcap)
        return
    # wpan.fcs_ok is the dissector's checksum verdict, 1 when it matches.
    run = subprocess.run(
        [tshark, "-r", str(pcap), "-T", "fields", "-e", "wpan.fcs_ok"],
        capture_output=True, text=True, timeout=60,
    )
    if run.returncode != 0:
        logger.warning("tshark failed (%d), FCS check skipped: %s", run.returncode, run.stderr)
        return
    verdicts = run.stdout.split()
    bad = [i + 1 for i, v in enumerate(verdicts) if v not in ("1", "True")]
    if len(verdicts) != len(records) or bad:
        problems.append(f"tshark sees {len(verdicts)} of {len(records)} frames, FCS not ok in "
                        f"frames {bad[:10]}")


def check_direction(direction: str, ext, tx: Shell, rx: Shell, rx_dut: DeviceAdapter,
                    tmp_path: Path) -> None:
    for s in (tx, rx):
        s.exec_command("wpan set promisc 0")
        s.exec_command("wpan set print 0")
        s.exec_command("wpan on")
    tx.exec_command(f"wpan set chan {CHANNEL}")
    rx.exec_command("wpan clear")
    rx.exec_command("sniffer stats_reset")
    rx.exec_command(f"sniffer channel {CHANNEL}")
    time.sleep(SETTLE_S)
    tx.exec_command("wpan clear")
    start = "\n".join(rx.exec_command("sniffer start"))
    assert "started" in start, f"sniffer start: {start}"

    tx.exec_command(f"wpan tx {FRAMES} {INTERVAL_MS} {PAYLOAD}")
    burst_s = FRAMES * (INTERVAL_MS / 1000.0 + 0.01)
    lines = collect(rx_dut, burst_s + 2, burst_s + 20)

    rx.exec_command("sniffer stop")
    stats = rx.exec_command("sniffer stats")
    rx_status = rx.exec_command("wpan status")
    tx_status = tx.exec_command("wpan status")

    problems: list[str] = []
    records: list[bytes] = []
    counters: list[int] = []
    unparsed = 0
    for line in lines:
        if "psdu:" not in line:
            continue
        m = ext.FRAME_RE.search(line)
        try:
            psdu = bytes.fromhex(m.group(1)) if m else b""
            rssi, lqi, device_us = int(m.group(2)), int(m.group(3)), int(m.group(4))
            psdu += ext.fcs(psdu)
            payload = ext.tap_header(CHANNEL, rssi, lqi) + psdu
            records.append(ext.pcap_record(device_us, payload))
        except (AttributeError, ValueError, struct.error):
            unparsed += 1
            logger.error("unparsable capture line: %r", line)
            continue
        counter = frame_counter(psdu[:-2])
        if counter is not None:
            counters.append(counter)
    if unparsed:
        problems.append(f"{unparsed} capture lines the extcap's pattern does not parse")
    check_pcap(ext, records, tmp_path, direction.replace(" ", "").replace(">", "_"), problems)

    stats_m = next((m for m in map(STATS_RE.search, stats) if m), None)
    enq, drop, tx_rec = (int(x) for x in stats_m.groups()) if stats_m else (-1, -1, -1)
    if stats_m is None:
        problems.append(f"no sniffer stats line in {stats}")
    elif drop or enq != len(records) + unparsed:
        problems.append(f"sniffer stats enq {enq} drop {drop} tx_rec {tx_rec}, but the capture "
                        f"holds {len(records) + unparsed} frames")

    _, test, bad, dups, lost = counts_from(rx_status, RX_RE)
    sent, ok, noack, busy, err = counts_from(tx_status, TX_RE)
    unique = set(counters)
    if sent != FRAMES or ok < FRAMES - MAX_TX_ERRORS or busy:
        problems.append(f"the transmitter sent {sent} of {FRAMES} frames, {ok} ok, {err} errors, "
                        f"{busy} busy")
    if len(unique) != test - dups:
        problems.append(f"the capture holds {len(unique)} unique test frames, the receiver "
                        f"counted {test} ({dups} duplicates)")
    if test < ok:
        problems.append(f"the receiver counted {test} test frames, fewer than the {ok} the "
                        "transmitter completed")
    if bad:
        problems.append(f"the receiver counted {bad} corrupt test frames")

    if problems:
        dump = {name: "\n".join(s.exec_command(cmd)) for name, s, cmd in (
            ("rx sniffer stats", rx, "sniffer stats"), ("tx sniffer stats", tx, "sniffer stats"),
            ("rx wpan status", rx, "wpan status"), ("tx wpan status", tx, "wpan status"))}
        pytest.fail(
            f"channel {CHANNEL}, {direction}: " + "; ".join(problems) + f" (receiver lost {lost})"
            + "".join(f"\n--- {k} ---\n{v}" for k, v in dump.items())
        )


def counts_from(lines: list[str], pattern: re.Pattern) -> tuple[int, ...]:
    for line in lines:
        if m := pattern.search(line):
            return tuple(int(x) for x in m.groups())
    pytest.fail(f"no line matching {pattern.pattern!r} in: {lines}")


@pytest.fixture(scope="module")
def extcap():
    return load_extcap()


def test_s3_sniffs_c6_frames(shells: list[Shell], duts: list[DeviceAdapter], extcap, tmp_path):
    check_direction("native -> sdr", extcap, shells[0], shells[1], duts[1], tmp_path)


def test_c6_sniffs_s3_frames(shells: list[Shell], duts: list[DeviceAdapter], extcap, tmp_path):
    check_direction("sdr -> native", extcap, shells[1], shells[0], duts[0], tmp_path)
