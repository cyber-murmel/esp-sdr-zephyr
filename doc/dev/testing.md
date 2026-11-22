# Testing

<!-- nav-start -->
Previous: [Code placement](code-placement.md) · Up: [Reference](index.md) · Next: [ZigBit reference](zigbit.md)
<!-- nav-end -->

Every pass/fail test of the firmware is a twister test. All but the S3 variant of
`esp_sdr_rx_api` and `esp_sdr_chancap_api` also run end to end under twister; those two are
deployed by hand ([ESP32-S3](#esp32-s3)). The Python tools have host tests of their own (pytest).

This page goes from the tests that need no board to those that need two, then to CI and the
measuring tools that are not gates. [Code placement](code-placement.md) describes the placement
tools that the lints and the build report check.

## Test tiers

| Tier | Where | Runs on | Scope |
| --- | --- | --- | --- |
| [Unit](#unit-tests) | `tests/unit/` | host (`native_sim/native/64`), no board | portable code as ztest suites |
| [Host](#host-tests) | `tests/host/` | host (CPython), no board | the Python tools that run in builds and CI, and the host tools of the apps |
| [Integration](#integration-tests) | `tests/integration/` | one real board | ztest on the radio itself |
| [Regression](#regression-tests) | `tests/regression/` | two real boards | pytest through twister's multi-board harness |
| [Smoke](#smoke-tests) | `apps/*/sample.yaml` | build, and one board | every app builds; boot checks on the board |

The MCH2022 badge's FPGA gateware has simulations of its own, run with pytest in `gateware/`
([MCH2022 badge](../apps/mch2022.md#building-and-loading)).

## Unit tests

### Suites

Each table lists the ztest suites of one area and what they check.

#### Radio, DSP and sample paths

| Suite | Covers |
|---|---|
| `iq_pack` | the `stream` app's I/Q packing |
| `diag_dsp` | the diagnostic-bus ring's box16 and CIC decimator, bit-exact against naive references |
| `dsp` | the shared signal processing helpers of `esp_sdr_dsp.h`: windows, sinc, the Kaiser window's I0 and beta, the FFT against a direct DFT, whole-cycle tones |
| `lte` | the LTE crystal calibration's estimator and temperature model (below) |
| `chancap_dsp` | the channelized burst capture's filter bank, decimator bank and detector, against a direct floating point model: tones per channel, rejection, the decimator, the detector |
| `gapless_sort` | the C6 gapless receiver's width sorting on synthetic 80 MS/s dump words: Wi-Fi-wide noise wide; Bluetooth LE, 802.15.4 and two tones at any spacing narrow; the DC taken out; the mean frequency |
| `arb_pred` | the radio arbiter's wake predictor on synthetic Wi-Fi enable times: the period as a median (robust to an odd long awake phase), the phase, MAX modem and ESP-NOW patterns, a change of power save, staleness |
| `esdr_seg` | the gapless block's segment codec against a model of the format, also under ASan |

`lte` checks:

- cells decoded from synthetic synchronization signals in noise;
- offsets, and the reference signals' fine offset to a few Hz;
- a 1 ms window, and a search window shorter than the capture;
- noise rejected;
- the ppb arithmetic;
- the points and fit of `esp_sdr_cal.h`.

#### IEEE 802.15.4

| Suite | Covers |
|---|---|
| `ieee154_phy` | the 802.15.4 PHY |
| `ieee154_dsp` | the driver's signal processing |

With `IEEE154_TABLES` set, both also print their sweep tables ([Sweep tables](#sweep-tables)).

#### Link codecs and modems

`tests/unit/link` holds the link's codecs and modems:

| Suite | Covers |
|---|---|
| `link_rs` | RS(255,223) |
| `link_bch` | BCH |
| `link_hamming` | Hamming(128,120) |
| `link_qam`, `link_ofdm` | the QAM and OFDM modems: frames through a simulated channel, decoded as the host simulators in `apps/link/tools` do |

#### P4 channelizer and pcap

| Suite | Covers |
|---|---|
| `esdr_pfb` | the P4's channelizer and burst detector of the gapless stream: the tests' float reference bank against numpy; M = 20 too (numpy reference regenerated with it) |
| `esdr_pfbq` | the fixed-point bank and its PIE kernels against that reference; M = 20 too |
| `esdr_link_host` | the P4 apps' host side of the serial transport to the C6: requests, replies and the splitter of the radio's stream |
| `pcapng` | the pcap app's pcapng writer and records, byte for byte: blocks, the records, the comment |
| `pcap_ble` | its Bluetooth LE decoder: the ubertooth_le golden packets GFSK modulated at 2 and 4 samples a symbol, LE 1M and 2M, offsets, bad CRCs, CONNECT_IND, an SNR sweep |
| `pcap_wpan` | its 802.15.4 decoder and the PHY's receiver instances: frames through the fixed-point bank, from one bin and from two neighbours combined; the combination's signs; bad FCS; the channel mode's 2 MS/s from the C6's boxcar at three chip offsets |
| `pcap_generic` | its generic burst analysis on synthetic bursts; its sample cap |
| `pcap_chain` | the survey path end to end: Bluetooth LE packets GFSK modulated at 80 MS/s at six bins, through the bank, decoded from their bin; nothing from the mirror bin |

#### osmosdr and Ubertooth

| Suite | Covers |
|---|---|
| `esdr_dev` | the osmosdr protocol's request checks, GET_INFO reply and block header check |
| `ubertooth_core` | the Ubertooth app's USB requests, clock and queue |
| `ubertooth_tx` | its GFSK modulator and packet builders |
| `ubertooth_specan` | its spectrum analyser |
| `ubertooth_le` | its BLE packet finder and connection follower, against golden vectors of `tools/le_ref.py` |
| `ubertooth_demod` | its GFSK demodulator, BR symbol blocks, RSSI conventions and hop sequences (below) |

`ubertooth_demod` checks:

- LE and BR bursts from a generator of its own: clean, near full scale, 60 kHz off in noise;
- the squelch keeping noise out and the preamble in;
- the time stamps over a gap;
- the noise floor after a loud start and across a gap;
- the modes' floor measurement (a low percentile) on plain noise and under Wi-Fi bursts 91 % of
  the time;
- the clip guard's gain steps;
- blocks, idle fill and the status flags;
- the sweep and AFH channel orders;
- the production demodulator against the frozen reference `gfsk_ref`, symbol for symbol with
  level, floor and horizon, on 8 bit LE and BR streams, under Wi-Fi bursts and at the extremes,
  pushed in pieces of 1, 256, 3000 and random sizes, with gaps and squelch changes.

#### Zigbee

The Zigbee stack of `lib/zigbee` has five suites:

- `zigbee_crypto` and `zigbee_frame`, both against independent implementations;
- `zigbee_znp`;
- `zigbee_mac`;
- `zigbee_stack`.

### Running

```
esp-sdr-zephyr/scripts/run-unit-tests.sh
esp-sdr-zephyr/scripts/run-unit-tests.sh -s unit.esp_sdr.link    # one test
```

- The script runs twister on `native_sim/native/64` with `NSI_OPT=-O2`.
- Plain `native_sim` is 32-bit, and this nix shell has no 32-bit glibc.
- Without an optimization level, glibc's `_FORTIFY_SOURCE` check warns inside native_sim's own
  runtime code, and `-Werror` makes that fatal.
- `tests/unit/link` builds the link's sources with `-U__ZEPHYR__`, so their host branch is
  compiled instead of the ESP-IDF one.
- The suites draw their random input from `tests/common/include/test_rng.h` (xorshift64 and
  normal deviates), seeded before each case.

### UBSan

```
UBSAN=1 esp-sdr-zephyr/scripts/run-unit-tests.sh
```

- A second pass over every suite with `CONFIG_UBSAN=y` and `-fno-sanitize-recover=undefined`, so
  the first finding aborts the test and fails it. One switch, no per-suite scenarios.
- It writes to `twister-out-ubsan`. CI runs it after the unit tests; pre-commit runs only the
  normal pass, to keep its run time.
- Findings print `file:line: runtime error: ...` in the test's `handler_stderr.log`.

### Coverage

```
NIX_HARDENING_ENABLE= esp-sdr-zephyr/scripts/run-unit-tests.sh \
  --coverage --coverage-basedir . --coverage-formats html
esp-sdr-zephyr/scripts/coverage-summary.py esp-sdr-zephyr/twister-out/coverage_summary.json
```

#### Build

- Coverage builds without optimization.
- The nix shell's compiler wrapper defines `_FORTIFY_SOURCE` regardless, and glibc's warning about
  that is fatal under `-Werror`.
- The empty `NIX_HARDENING_ENABLE` turns the wrapper's hardening off for the run. Ubuntu's gcc, as
  in CI, defines it only when optimizing.

#### Output

- Twister builds the suites with `CONFIG_COVERAGE`, runs them and has gcovr merge the results. The
  script runs twister from the repository root.
- `twister-out/coverage/index.html` shows coverage per file and line.
- `twister-out/coverage_summary.json` feeds `scripts/coverage-summary.py`, which prints the
  Markdown table CI puts on the job's summary page.
- The test sources themselves (`tests/`) are left out.
- Coverage counts only the files some unit test compiles. The script lists the C sources under
  `lib/` and `apps/` that none does: the radio drivers and the app glue, which only the
  integration and regression tests reach on the boards.

### Sweep tables

#### Cases

`ieee154_phy` and `ieee154_dsp` each hold one more test case, skipped unless the environment
variable `IEEE154_TABLES` is set (to anything):

| Case | Table |
|---|---|
| `test_per_table` (`ieee154_phy`) | the PHY's packet error rate over SNR and carrier offset |
| `test_decimator_table` (`ieee154_dsp`) | the decoded fraction of the driver's chain through each decimator over SNR |
| `test_cfo_estimator_accuracy` | with the variable set, also prints its carrier offset estimates |

These are the tables [IEEE 802.15.4](../modem/ieee802154.md#simulated-error-rate) quotes, and
what tuning the PHY or a decimator needs.

#### Running the tables

```
esp-sdr-zephyr/scripts/unit-tables.sh          # both
esp-sdr-zephyr/scripts/unit-tables.sh phy      # the PHY error rate table
esp-sdr-zephyr/scripts/unit-tables.sh dsp      # the decimator table
```

- The script runs the two tests with `IEEE154_TABLES=1` and prints their logs.
- On an idle machine each table takes between half a minute and a minute, plus the build.
  Concurrent builds can triple that, hence the generous twister timeouts of these ids (300 s and
  900 s).
- The table cases also assert what the documentation claims, after printing the whole table, so a
  regression fails the run and still shows the numbers:
  - `phy`: no frame lost from 6 dB SNR up, none twice, none with wrong content;
  - `dsp`: each decimator column reads 1.000 from 2 dB above the SNR where it first does.
- Without the variable the cases are skipped, and the unit tests stay at a few seconds.

## Host tests

### Covered tools

#### Build and CI tools

| Tool or test | Notes |
|---|---|
| multiapp's build steps: `mkfat.py` | its output read back by a FAT reader written from the spec |
| multiapp's build steps: `apptrail.py`, `appconfig.py`, `buildid.py`, `exports.py` | |
| the register page tools | |
| `twister-flash.py`, `coverage-summary.py` | |
| `test_place_lint.py` | the placement lint ([Flags and checks](code-placement.md#flags-and-checks)) |
| `test_place_macros.py` | runs the placement macros through the host preprocessor with stub headers |

#### MicroPython app

| Test | Subject | Checks |
|---|---|---|
| tools | `mkegg.py`, `eggs_index.py`, `check_capture.py` | the MicroPython app tools |
| `test_dusk.py` | its colours | `modules/dusk.py` holds what `tools/dusk.py` derives; its text pairs reach WCAG AA; the heat scale only gets lighter |
| `test_fsk.py` | its packet modem | the Python reference through a noisy channel, the C modem against it |
| `test_datalink.py` | its Data link egg | the unmodified export on the gnuradio stand-ins against a simulated peer, also 24 kHz off (the receiver follows it) and with a receive ring that loses input |
| `test_bursts.py` | its hand-written Bursts egg on a fake radio | the gapless source first, the triggered snapshots where the radio refuses or lacks it, the chooser between them |
| `test_pcap_egg.py` | its Pcap egg: the packet capture, the table and the app | [Pcap egg](#pcap-egg) |
| `test_scan_core.py` | its Scan egg: the spectrum of a burst in fixed point | [Scan egg](#scan-egg) |
| `test_scan_egg.py` | its Scan egg: the hop plans, the edges and the app | [Scan egg](#scan-egg) |

#### Pcap egg

`test_pcap_egg.py` tests the packet capture: `cmod/esdr_pcap_core.c` with apps/pcap's decoders,
built with the host compiler. It checks:

- the channel mode and decoders on synthetic Bluetooth LE and 802.15.4 streams fed in pieces of
  any size;
- a gap;
- 8-bit items;
- burst records;
- the detector's word-wise shift against samples scaled before;
- the table's dissector, filters, groups and sorts;
- the table's lines;
- the dissector in C against the Python one on every record cut short and on random bytes;
- the app on a fake radio whose capture is that C, with and without the engine:
  - the table;
  - the choosers;
  - an overrun;
  - Save's pcapng read back.

#### Scan egg

`test_scan_core.py` tests the spectrum of a burst in fixed point: `cmod/esdr_scan.c`, built with
the host compiler both ways. It checks:

- the int32 FFT against numpy;
- the S3's esp-dsp 16-bit kernel against numpy: its vector frame passes in C, its rounding bias
  taken off and each frame scaled up;
- the fold into the panorama.

`test_scan_egg.py` checks:

- the hop plans: every bin owned once, within the guard and the step;
- the edges' bisection;
- the app on a fake radio of each flavour (also a C6 that reports its PLL's lock, bisected, and
  one from before that report, on its table):
  - tones in their bins;
  - each spectrum from the second CPU's worker without a capture into the buffer it reads;
  - a refused or stuck hop;
  - a clipped hop;
  - a gain change;
  - tabs;
  - headless;
  - Save's rtl_power lines.

#### Library and app host tools

| Test | Subject | Checks |
|---|---|---|
| `test_ring_stage.py` | lib/esp_sdr's ring stage of the S3, on a buffer standing in for SRAM bank 2 | [Ring stage](#ring-stage) |
| `test_ble_pcap.py` | chancap's BLE pcap tool | the ubertooth_le golden packets, GFSK modulated with noise and a carrier offset at 2 and 4 samples a symbol |
| `test_esdr_pcap.py` | the pcap app's Wireshark extcap on a fake pyusb and badge | its interface, DLT and configuration output and options; a capture's vendor requests; the copy loop's resynchronisation and stop paths |
| | a stream from the firmware's pcapng writer and records, built with the host compiler | read back by a reader from the specification and, where it is installed, by tshark with the burst dissector (Lua) |
| `test_gr_pcap.py` | the pcap app's wideband host tool | [Wideband pcap tool](#wideband-pcap-tool) |

#### Ring stage

`test_ring_stage.py` checks:

- the sink's words in order;
- the stream index over holes and new runs;
- pairs averaged;
- a reader the sink laps;
- a lap after 2^31 samples without a hole;
- a stage started over;
- more holes than its log keeps.

#### Wideband pcap tool

`test_gr_pcap.py` runs the tool on a synthetic 20 MS/s recording. Its channels are cut with numpy
and, where GNU Radio is installed, by the tool's own flowgraph. It checks:

- Bluetooth LE and 802.15.4 packets on five channels each once;
- a data channel's packet through the access address a CONNECT_IND on another channel taught;
- no record of what a neighbouring channel decoded;
- one wideband record over a 12 MHz band;
- the cross-channel rules on records made by hand:
  - leaks;
  - duplicates;
  - a weak wide signal's pieces.

### Running

```
esp-sdr-zephyr/scripts/run-host-tests.sh              # all, then the coverage report
esp-sdr-zephyr/scripts/run-host-tests.sh -k mkfat     # some (pytest arguments)
```

- pytest runs under coverage.py, in under a minute (most of it the Data link simulation).
- The nix shell has both; elsewhere `tests/host/requirements.txt` pins them, with PyYAML,
  pyelftools and numpy.
- The tools are loaded by path (`tests/host/conftest.py`).
- `test_exports.py` compiles a small shared library with the host compiler and is skipped without
  one. So are the C parts of `test_fsk.py` and `test_datalink.py`
  (`apps/micropython/cmod/esdr_fsk.c`, through ctypes).
- `tests/host/coveragerc` sets what counts: the build and CI tools under `apps/multiapp/`,
  `apps/micropython/tools/`, `scripts/` and `doc/registers/`, files no test imports included.
- The bench tools under `apps/*/tools` need boards and stay out.

## Integration tests

### Suites

| Suite | Boards | Checks | On the air |
|---|---|---|---|
| `esp_sdr_rx_api` | S3, C6, C5, S31 | the receive API's contract: error codes, bounds, one real capture per supported rate | none: needs no antenna, peer or host tool, never transmits |
| `esp_sdr_diag16_api` | S31 | the gapless ring: rates, sampling phase, stops, a sink too slow for the DMA | none, as above |
| `esp_sdr_dumpring_api` | S31 | the ring through the dump engine alone, below | none, as above |
| `esp_sdr_diag_api` | C5 | the gapless diagnostic-bus ring: argument contract, a 10 s run's byte rate and integrity, every step of the decimation ladder, stop and restart | none, as above |
| `esp_sdr_replay_api` | C5, S31 | the DAC-engine replay, below | plays all-zero words, so only the LO leaks out: keep the board on its coax link |
| `esp_sdr_tx_paced_api` | S31 | the CPU-paced transmit, below | transmits a tone at an eighth of full scale |
| `esp_sdr_chancap_api` | S3 | the channelized burst capture, below | receives whatever is on the air |
| `display_transpose` | WHY2025 badge | the turned panel, below | |
| `esdr_fpga` | MCH2022 badge | the FPGA with a bitstream from `gateware/`: loaded at boot, the SPI link, the panel behind it, the DSP chain on a tone | |

`display_transpose` checks:

- every write transposed into the framebuffer, read back pixel for pixel over sizes and positions
  (blocks 32 n + 1 rows high among them);
- both by the PPA (`integration.display.transpose`) and by the CPU (`.cpu`);
- blocks of earlier frames kept through the double buffering;
- a full screen's time in LVGL's bands: 9.3 against 26 ms.

### Ids

Each test has one id per board:

| Id | Board and variant |
|---|---|
| `integration.esp_sdr.rx_api.c6`, `.c5`, `.s3`, `.s31` | one per chip |
| `integration.esp_sdr.diag_api.c5` | the C5's 4-bit ring |
| `integration.esp_sdr.diag16_api.s31` | the S31's 16-lane ring |
| `integration.esp_sdr.dumpring_api.s31`, `.s31.smp` | the S31's ring without the bus, single core and both cores |
| `integration.esp_sdr.replay_api.c5`, `.s31` | the DAC-engine replay |
| `integration.esp_sdr.tx_paced_api.s31` | the S31's CPU-paced transmit |

### Deploying per board

`s3pty.py` in the commands below is the workspace's console bridge, not part of this repository.

#### ESP32-C5 and ESP32-C6

The C5 and the C6 both have a USB-Serial-JTAG console and are deployed the same way:

```
west twister -p waveshare_esp32c5_zero/esp32c5/hpcore -T esp-sdr-zephyr/tests/integration/esp_sdr_diag_api \
    --device-testing --flash-before \
    --device-serial-pty "python3 .claude/skills/esp32s3-console/s3pty.py <by-id port>" \
    --west-flash="--esp-device=<by-id port>" --outdir <dir> --runtime-artifact-cleanup pass
```

- The C6 is flashed directly with twister's device testing.
- Opening the C6's USB-Serial-JTAG port resets the chip, and the suite finishes before a plain
  `--device-serial` attach after the flash would see it.
- So open the console after the flash, so the reset reruns the suite (`--flash-before`), or use a
  `--device-serial-pty` script that resets the chip once it is attached.

#### ESP32-S3

- The board normally runs an app that owns the USB OTG port, so esptool cannot reset it into the
  ROM bootloader.
- The S3 variant therefore builds with sysbuild and MCUboot and has its own CDC-ACM shell and DFU.
- Deploy it with `west dfu -d <build dir> -s <usb serial>` and read its CDC-ACM console.
- The suite sleeps 10 s after boot so the host can enumerate the port first: CDC-ACM does not
  replay output printed before a reader attaches.
- `esp_sdr_chancap_api` is deployed by hand the same way.

#### ESP32-S31 and VIEWE

`esp_sdr_rx_api`, `esp_sdr_diag16_api`, `esp_sdr_dumpring_api`, `esp_sdr_tx_paced_api` and
`esp_sdr_replay_api` run over the board's UART bridge, whose RTS resets it:

```console
west twister -p esp32s31_function_coreboard/esp32s31/hpcore -T esp-sdr-zephyr/tests/integration/esp_sdr_diag16_api \
  --device-testing --flash-before --device-serial-pty "python3 .claude/skills/esp32s3-console/s3pty.py <UART>" \
  --west-flash=--esp-device=<UART> --runtime-artifact-cleanup pass -j 1
```

- `esp_sdr_diag16_api` wants the 16 pads of the ring
  ([Gapless receive](../samples/rings.md#pads)) free on J2.
- The VIEWE board (`ues31s043h800v480c_u/esp32s31/hpcore`, its CH340 as `<UART>`) runs both the
  same way; its ring uses the panel's data lines.

#### MCH2022

- `integration.esdr_fpga` embeds a bitstream, built with
  `python3 -m esdr_fpga.build top --out <dir>` in `gateware/`
  ([MCH2022 badge](../apps/mch2022.md#building-and-loading)).
- The ESP32's console is the RP2040's first interface, whose DTR/RTS reset is unreliable after
  esptool's download reset.
- `tests/integration/esdr_fpga/mch2022_pty.py` boots the application through the RP2040's USB
  reset instead.
- The suite takes about 1.3 s after boot.

```
west twister -p mch2022/esp32/procpu -T esp-sdr-zephyr/tests/integration/esdr_fpga \
    --device-testing --flash-before \
    --device-serial-pty "python3 esp-sdr-zephyr/tests/integration/esdr_fpga/mch2022_pty.py <if00 port>" \
    --west-flash="--esp-device=<if00 port>" \
    -x=CONFIG_ESDR_FPGA_IMAGE=\"<dir>/top/top.bin\" --outdir <dir> --runtime-artifact-cleanup pass
```

### Run times and checks

#### esp_sdr_dumpring_api

It checks the S31's ring through the dump engine alone:

- the argument contract;
- a second's words against its time, with every unit's ends checked;
- the turns' cost;
- a capture refused from inside a run;
- the receiver afterwards.

The `.smp` variant also checks what a busy thread on the other core keeps.

#### esp_sdr_diag_api

It runs about 25 s and drives 8 pads as outputs, toggling at up to 40 MHz: nothing may be wired to
the C5's GPIO 0, 1, 3, 4, 5, 7, 10 and 23. Its checks are the ring's contract, not a signal:

- every byte of 40 MB/s arrives for the time of the run, to 2 %;
- no block is skipped, and the interrupt never falls behind;
- the byte count follows the system timer: a byte lost per ring wrap would be -17 ppm, and the
  bound is 5 ppm;
- the consumer's cost stays under the 6 cycles per byte the CPU has at 240 MHz;
- the receiver's noise reaches the pads (the ring is not a constant);
- each step of the decimation ladder (250 to 15.625 kS/s) has the right rate, a gapless index and
  the right count.

#### esp_sdr_replay_api

On the C5 and the S31 it checks:

- the argument contract;
- the pass time at each rate;
- the repeat count;
- restarts;
- receive afterwards.

On the S31 it also checks:

- its group left as it was;
- the CPU and timers running while it plays;
- captures and the carrier refused meanwhile.

It runs about 10 s and times the engine against the CPU cycle counter.

- A pass of n words takes n x 3 cycles at 80 MS/s and n x 6 at 40 MS/s (240 MHz CPU), plus a
  start of under 400 cycles.
- So do 255 passes in a row, which shows the passes follow back to back.
- Pointing the 40 MS/s bit at bit 15 instead of 17 fails the two 40 MS/s cases (146363 cycles for
  96000).
- On the S31 it takes under a second: 4, 8 and 16 cycles a word (320 MHz CPU), the group's 32768
  words at 20 MS/s too.
- The S31 has three cases of its own that each let the engine play while they look (20 to
  150 ms).

#### esp_sdr_tx_paced_api

It checks the S31's CPU-paced transmit:

- the argument contract;
- the vendor tone and the stream excluding each other;
- one second at 250 kS/s x4 with interrupts locked, at the exact rate with no late write;
- every interpolation factor;
- missing input counted;
- the TX DC register as it was;
- the receive floor afterwards.

It runs about 2 s.

- Its stream, input callback and polling loop are all in IRAM. So placed, every rate keeps its
  sample count to one and no write comes a whole period late; the worst lateness is about 27
  cycles.
- The receive check takes the third lowest of nine captures before and after, within 3 dB: a
  Wi-Fi frame can cover three captures in a row.

#### esp_sdr_chancap_api

It checks the S3's channelized burst capture:

- the start contract;
- the LO plan;
- the channelizer's timing on synthetic data;
- a bench with the vector kernels checked bit for bit against the C reference;
- short live runs.

## Regression tests

### Scenarios and ids

Three scenarios, all pytest through twister's multi-board harness, each with its own pair of
images:

| Scenario | Boards | What it checks |
|---|---|---|
| [`tests/regression/ieee802154_link`](#ieee802154_link) | C6, S3 | 802.15.4 frames between the C6's native radio and the S3's software radio |
| [`tests/regression/lo_tuning`](#lo_tuning) | S3, C6 | tones from the S3 to the C6 at overlapping LO frequencies, through the 5/6 divider and directly tuned; guards the transmit divider and `-ERANGE` |
| [`tests/regression/stream_gapless`](#stream_gapless) | C6, C5 | a tone streamed between the C6 and the C5 through a coax link, both ways, with no gap in the recording |

- Each has three ids: `regression.esp_sdr.<test>.c6` and `.s3` only build the two images
  (`build_only`).
- `regression.esp_sdr.<test>` runs `pytest/` on both boards (`required_devices` and
  `required_applications`).
- `stream_gapless` has the C6 and two C5 images: `.c5`, the receive paced by the CPU through the
  IQ estimator, and `.c5_diag`, from the diagnostic-bus ring.
- It has a scenario for each: `regression.esp_sdr.stream_gapless.c6_c5` and `.c6_c5_diag`.

### Running

Run them with both boards attached:

```
esp-sdr-zephyr/scripts/run-regression-tests.sh -O <output dir>
```

- For each scenario the script builds the two images (the scenario id builds none of its own),
  puts them on the boards and runs the tests, a few minutes in all.
- `-s regression.esp_sdr.lo_tuning` picks one.
- A relative `-O` is relative to the repository root.
- What is specific to a desk is in `tests/regression/hardware-map.yaml`: change the serial
  numbers for another desk. The radio levels of `lo_tuning` are the exception
  ([Setup and analysis](#setup-and-analysis)).

### Twister harness

Four pieces make twister cope with these two boards.

#### twister-flash.py

- `scripts/twister-flash.py` is passed as `--flash-command`. Twister calls it once per board with
  that board's build directory and its id from the map.
- An MCUboot image (the S3) goes on with `west dfu -s <id>`, anything else with `west flash` to
  the port of that id.
- Twister's own `west flash` (esptool) cannot reset an S3 whose app owns the USB OTG port.
- Both boards flash at once.
- Afterwards `/tmp/esp-sdr-tty/<id>` points at the board's port, found by serial number alone.
- The map names the S3's port by this link, because its `/dev/serial/by-id` name carries the USB
  product string of whichever scenario's image is on it.

#### flash_before

`flash_before: true` in the map opens the console after the flash, because the C6 resets when its
port is opened, and the S3's port goes away and comes back during the DFU update.

#### pytest_dut_scope and timeouts

`pytest_dut_scope: module` and a generous `timeout` in each scenario: both boards are flashed once
for all tests, and the flashing counts against the timeout.

#### conftest Ctrl-C

- `tests/regression/conftest.py` presses Ctrl-C on each shell before twister looks for the
  prompt.
- Twister presses Enter with a bare LF, which a Zephyr shell swallows after a CR (`process_nl()`
  in `subsys/shell/shell.c`), and `west dfu` ends its lines with CR LF. So the S3 would never
  show a prompt.
- Pytest loads this conftest for both scenarios because twister runs it from its own working
  directory (the repository root with the script). With no pytest ini file in the tree, that
  directory becomes pytest's rootdir.

### -Werror and the CDC-ACM log

- Twister compiles with `-Werror`; a plain `west build` does not.
- With CDC-ACM as console and shell, `usbd_cdc_acm.c` turns its own logging off with a
  `#warning`, which is fatal under twister.
- Every S3 app with a CDC-ACM shell therefore sets `CONFIG_USBD_CDC_ACM_LOG_LEVEL_OFF=y` itself.

### ieee802154_link

#### Build

`tests/regression/ieee802154_link` builds `apps/wpan` from its own directory.

- `src/main.c` is built by path, and `APPLICATION_CONFIG_DIR` and `KCONFIG_ROOT` point Zephyr's
  config lookup there. So `prj.conf`, `socs/`, `boards/`, `Kconfig` and the placement rules
  (`place.h`) are the app's own and cannot drift.
- Only `sysbuild.conf` is a copy, because sysbuild looks it up before the image's
  `CMakeLists.txt` runs.

#### What passes

- On channels 11, 15, 20 and 25, in both directions, 100 test frames of 100 bytes at 5 ms spacing.
- Every frame the transmitter completed must arrive, none corrupt or twice.
- A transmitter may fail up to 3 of the 100 itself. The S3's DAC generator fills a block late
  about once in 400 frames and cuts the frame (`-EIO`), and the C6 now and then reports a tx error
  of its own.
- That is not a receive loss, which is what the test is for, so it is bounded loosely and the
  receiver is held to what the transmitter reports as sent.
- The ACK check sends 20 frames with an ACK request from the S3 to the C6 and allows 2 to fail or
  go unanswered.
- When a check fails, the message carries `wpan status` of both boards.

#### Other desks: DevKitC and chip antenna

- `regression.esp_sdr.ieee802154_link.devkit` runs the same tests with an ESP32-S3-DevKitC
  (N16R8) in place of the XIAO S3 (`.s3_devkit` builds its image, `apps/wpan/boards/` gives it
  the 8 MB PSRAM overlay) and the C6 on its chip antenna (`.c6_chip`).
- The XIAO C6's RF switch is driven only by the board's `board.c` with
  `CONFIG_XIAO_ESP32C6_EXT_ANTENNA`; without it the radio is nearly deaf (LQI 5-7). `.c6_chip` sets
  `CONFIG_XIAO_ESP32C6_EXT_ANTENNA=n` and `CONFIG_WPAN_C6_CHIP_ANTENNA=y`, so that `apps/wpan`
  powers the switch and selects the chip antenna itself.
- First run, 2026-11-23, the two boards side by side on the desk: the sniffer cases, the ACK case
  and every C6 to devkit direction passed; devkit to C6 lost 7, 1, 0 and 2 of 100 frames on
  channels 11, 15, 20 and 25 (RSSI -67 to -78 dBm). By hand, 7 to 12 % of the 111-byte frames
  and about 3 % of 31-byte ones, at 5 or 10 ms spacing, with the sniffer built in or out. So it
  is this desk's radio link, which `test_frames_both_ways_no_loss` allows nothing for: its
  limits come from the XIAO S3 desk with coax.
- The devkit has its own entry in `hardware-map.yaml`. To run only this scenario:

```
esp-sdr-zephyr/scripts/run-regression-tests.sh -O <output dir> \
    -s regression.esp_sdr.ieee802154_link.devkit
```

#### Sniffer case

`pytest/test_sniffer.py` runs in both pytest scenarios, in either order relative to `test_interop.py`.

- On channel 15 one board sniffs (`sniffer start`) while the other sends 100 frames of 20 bytes,
  10 ms apart; then the roles swap.
- Every capture line must parse with the Wireshark extcap's own code
  (`apps/wpan/tools/wpan_sniffer.py`), which also builds a pcap in pytest's tmp dir; with `tshark`
  installed every frame's FCS must be ok.
- The capture holds every test frame the sniffer's `wpan status` counted, and `sniffer stats` has
  `drop=0` and `enq` equal to the frames captured.
- The transmitter may fail 3 frames, as above.
- Afterwards both radios are back to `wpan on`, promiscuous mode off and their addresses.

#### Bugs it guards against

The test guards against the two bugs that only ever showed as lost frames between real boards,
frames the sender had sent that never arrived:

- the Wi-Fi MAC interrupt inside the ring
  ([Gapless receive](../samples/rings.md#interrupts-inside-the-ring));
- the sample ring generation race (`lib/ieee802154/ieee154_esp_sdr.c`).

#### interop.py by hand

`apps/wpan/tools/interop.py --native <C6> --sdr <S3> --channels 11,15,20,25` runs a similar
exchange by hand. It has its own defaults (200 frames of 16 bytes, 10 ms apart) and, when a
direction fails completely, the I/Q inversion fallbacks.

### stream_gapless

#### Setup

- `tests/regression/stream_gapless` builds `apps/osmosdr_serial` from its own directory, as
  `ieee802154_link` does `apps/wpan`. `diag.conf` of the app selects the ring for the `.c5_diag`
  image.
- It needs the C6 and a C5 on a coax link with a 30 dB attenuator between their U.FL ports, at
  2437 MHz.
- The pytest asks twister for the flashed boards, lets go of their consoles (the stream is binary)
  and drives them with `apps/osmosdr_serial/tools/esdr_serial.py`.
- For each direction (C6 to C5, C5 to C6) and for the receive rates 250 and 31.25 kS/s, one board
  transmits a tone at 125 kS/s.
- After 15 s (the transmitter's tone drifts a few ppm while it warms up) the other records 10 s.

#### What passes

- A usable link: the tone at least 30 dB over the median bin and inside the band.
- The samples the stream owes (98 %), no index jump, no overflow flag, no lost sample counted by
  the device (`rx_overflows`, `rx_ring_lost_pairs`).
- `apps/osmosdr_serial/tools/gapcheck.py` finds no gap in the tone. It tracks the tone segment by
  segment, demodulates, averages blocks of 64 samples and looks for a spike in the block phase (k
  lost samples are k * 360 * f / fs degrees) or a dip of the amplitude (zero-filled samples).
- Phase noise of the blocks is about 1 degree at the bench levels; the threshold is 30 degrees.
- The transmitter played 99.5 % of what it was given (`tx_underruns`), and the host fed it in real
  time.
- A failure carries the evidence in one line: sample count, flags, index jumps, the tone's
  frequency and level, and the device's counters.

#### Desk levels and overrides

| Setting | Desk value | Override |
|---|---|---|
| C5 receive gain index | 40, about 10 dB below full scale; from index 48 up the C5 clips | `ESP_SDR_STREAM_C5_RX_GAIN` |
| C6 receive gain index | 30 | `ESP_SDR_STREAM_C6_RX_GAIN` |
| transmit step | 2 | `ESP_SDR_STREAM_C5_TX_STEP`, `ESP_SDR_STREAM_C6_TX_STEP` |
| recording time | 10 s | `ESP_SDR_STREAM_CAPTURE_S` |

Another SoC pair is another entry in the pytest's `CHIPS` and the gains, and another set of ids in
`testcase.yaml`.

#### Checked failure

The diagnostic-bus scenario was checked to fail on the bug it guards. Serving the USB FIFO once per
8 KiB ring block instead of every 2 KiB failed `c6_to_c5` at 250 kS/s with 1.55 M of 2.5 M
samples, 758 index jumps and 761 receiver overflows, while the other three streams passed.

#### gapcheck.py by hand

`gapcheck.py` also runs by hand on any recording:

```
esdr_serial.py <port> rx --out x.npy
gapcheck.py x.npy --rate 250000
```

`--selftest` checks the detector itself.

### lo_tuning

#### Setup and analysis

`tests/regression/lo_tuning` builds `apps/rfprobe` from its own directory for both boards, as
`ieee802154_link` does `apps/wpan`, and drives its `probe` shell:

| Command | Board | What it does |
|---|---|---|
| `probe f` | both | tunes and prints `esp_sdr_set_freq()`'s return code |
| `probe tone on`/`off` | S3 | loops a tone (loop playback, no gaps) |
| `probe rx 8192 0` | C6 | dumps 8192 samples at 80 MS/s as base64 |

- The pytest does the analysis: a Welch average of eight 1024-point FFTs, each bin compared with
  the median of the bins within 1 MHz, so the C6's uneven noise floor does not count as a tone.
- It reports the strongest tone and the level at the expected offset.
- Every filter is open: receive and transmit low-pass code 0.
- The C6 receives at fixed gain index 40 on its U.FL port, and the S3 transmits at its weakest
  step: on this desk the tones arrive 35 to 51 dB above the floor without clipping.
- Another RF path (an attenuator, antennas) may need other `RX_GAIN` and `TX_STEP` in
  `pytest/test_lo_tuning.py`.
- Before any other check the scenario measures the link at 2412 MHz, a Wi-Fi channel tuned the
  vendor's way, and stops with a "no usable link" message if the tone is missing there.

#### Pass rule

- A tone sent at TX LO + 3 MHz must show up at (TX LO - RX LO) + 3 MHz.
- It must stand at least 20 dB above the local floor and be the strongest one in the capture,
  within 150 kHz (the two crystals plus an FFT bin).
- Equal LOs alone would miss two PLLs that both land on the same wrong frequency. So each
  frequency is also checked with the boards 10 MHz apart, both ways round (the cross pairs).
- The sweeps of `tools/rfprobe.py` ([rfprobe](../../apps/rfprobe/README.rst)) pass a pair by the
  same rule.

#### Checks

- 1900 to 2610 MHz, in the divider band (PLL at 6/5) and directly tuned. Together with the pairs
  around 2210 MHz they keep the PLL between 2210 and 2651 MHz, the envelope upstream esp-sdr
  qualified on all chips.
- The switch at 2209/2210 MHz: divider on one board, direct tuning on the other.
- A 2000 MHz transmission is absent at 2400 MHz + 3 MHz, where it went out before the transmit
  path selected the divider.
- With the transmitter silent, nothing at the offset (the detector itself).
- `-ERANGE` on both boards at 1000, 1700, 3000 and 5000 MHz.
- No `-ERANGE` means the PLL locks. The S3's window edges are found by tuning MHz by MHz, from
  1880 MHz down and from 2610 MHz up, and must exist.
- At the last 3 MHz inside each end, wherever both boards tune without `-ERANGE` (the C6 8 MHz
  further in, or 16 if it reports `-ERANGE` itself), the tone must arrive. At least 2 of the 3 per
  end must tune.
- `-ERANGE` itself is conservative (`esp_sdr.h`) and is not checked for being reported too often.

The VCO window and the lock limits these checks rest on are in
[Tuning](../radio/tuning.md#vco-window-and--erange).

## Smoke tests

### Builds and console boot

- Every app builds for its boards (`apps/*/sample.yaml`); `apps/pcap` builds for both P4 boards.
- `apps/capture` also boots on the S3 and the C6 and must print `esp-sdr rx: ready` (console
  harness).
- `apps/multiapp` boots into an app from its file on the FAT volume (pytest,
  `sample.esp_sdr.multiapp.hil`, below).

### multiapp desk checks

Four `sample.esp_sdr.multiapp.*` scenarios check [multiapp](../../apps/multiapp/README.rst) on a
board on the desk. They use `scripts/twister-flash.py` and `tests/regression/hardware-map.yaml`
([Twister harness](#twister-harness)).

#### hil

`sample.esp_sdr.multiapp.hil` builds the multiapp host with `hello` as its autostart app
(`apps/multiapp/hil.conf`).

- `scripts/twister-flash.py` puts the image on the S3 with `west dfu` and, because the build has a
  FAT volume image, the volume with `west dfu --storage`.
- `apps/multiapp/pytest/test_multiapp.py` then checks that every app's file is on the volume and
  equals its built-in copy, that the boot ran `hello` from its file, and that hello's commands
  answer, its PSRAM atomics included.
- Through `CONFIG_MULTIAPP_MODULE_TESTS` it also checks calls across modules in PSRAM and internal
  RAM, a device in a module, and calls into the PHY's blobs as a module.

From the repository root, with the S3 attached:

```
west twister -T apps/multiapp -s sample.esp_sdr.multiapp.hil --device-testing \
    --hardware-map tests/regression/hardware-map.yaml \
    --flash-command "$PWD/scripts/twister-flash.py"
```

- About a minute, flashing included.
- It leaves the test image on the board: `west dfu` and `west dfu --storage` of a normal multiapp
  build afterwards.

#### wifi.hil

`sample.esp_sdr.multiapp.wifi.hil` is the same with `wifitest` as the autostart app
(`apps/multiapp/hil-wifi.conf`, `pytest/test_wifi.py`).

- The Wi-Fi module loads from its file, on the host's interface.
- `wifi scan` lists access points, so it needs one in range.

#### micropython.hil

`sample.esp_sdr.multiapp.micropython.hil` has `micropython` as the autostart app
(`apps/multiapp/hil-micropython.conf`, `pytest/test_micropython.py`).

- It has no built-in copy, and it takes the CDC-ACM port for its REPL. So the test works through
  that REPL and reaches the host's commands with `esdr.shell()`.
- It boots from its file with only `phy` and `sdr` (the modules it loaded are checked), USB on
  CPU 0, a heap of the spare PSRAM.
- A file is written and read back on `/FLASH:`.
- A ring runs at 250 kS/s with 0 gaps.
- A soft reset keeps the heap, and nothing crashes.
- On a board with a display it also runs `spectrum` from the launcher and checks that it draws
  with the ring gapless.

#### s31.hil

`sample.esp_sdr.multiapp.s31.hil` is the ESP32-S31-Function-CoreBoard-1 in its own configuration,
booting into `osmosdr_hs` (`pytest/test_multiapp_s31.py`). It checks that:

- every file on the volume equals its built-in copy;
- the boot ran `osmosdr_hs` from its file with the vendor class;
- the radio module's IRAM_ATTR code is in internal RAM and the rest in PSRAM;
- the last reset was clean.

It needs the board's USB HS port J4 on the PC through a cable with VBUS cut, and its hardware map
entry (id `1C2904D19880`, the board on this desk). The same command with
`-s sample.esp_sdr.multiapp.s31.hil` runs it; it leaves the board as it should be.

## Continuous integration

### Boards and jobs

`.github/workflows/build.yml` runs on every push and pull request. It builds every app and test
for each board it allows, one job per board:

| Board | Chip |
|---|---|
| `xiao_esp32s3/esp32s3/procpu` | ESP32-S3 |
| `communicator_badge/esp32s3/procpu` | ESP32-S3 |
| `xiao_esp32c6/esp32c6/hpcore` | ESP32-C6 |
| `why2025_badge/esp32c6/hpcore` with `why2025_badge/esp32p4/hpcore` | ESP32-C6 and ESP32-P4 |
| `waveshare_esp32c5_zero/esp32c5/hpcore` | ESP32-C5 |
| `esp32s31_function_coreboard/esp32s31/hpcore` | ESP32-S31 |
| `ues31s043h800v480c_u/esp32s31/hpcore` | ESP32-S31 |
| `mch2022/esp32/procpu` | ESP32 |
| `mch2022_rp2040/rp2040` | RP2040 |

- It also builds and runs the unit tests on `native_sim` and `native_sim/native/64`.
- A GitHub runner has no boards, so the integration and regression tests and the console smoke
  test are only built there; run them on the desk as above.
- The twister reports of each job are kept as artifacts.

### Workspace and caches

- `.github/actions/zephyr-setup` sets up each job's workspace, SDK and ccache as Zephyr's own setup
  action does, and caches the blobs too.
- It leaves out Zephyr's compliance requirements: nothing here runs them, and one comes from
  codeberg.org, which timed out under a dozen parallel jobs.
- Only pushes to main save caches; branches, tags and pull requests restore main's.

### Coverage reports

- The unit test job also measures code coverage ([Coverage](#coverage)): its summary page shows a
  table per source file, and the `coverage` artifact holds gcovr's HTML report.
- The "Host tools" job runs the host tests under coverage the same way: a table on its summary
  page, the HTML report in `host-coverage`.

### Git hooks

`.pre-commit-config.yaml` runs the same checks locally with [pre-commit](https://pre-commit.com)
(in the nix shell); `pre-commit install` once sets up both hooks. Each check runs only when its
files are staged or pushed:

| Hook | Check | When |
| --- | --- | --- |
| `svd2md` | the register pages match `doc/registers/*.svd` | commit |
| `svdmerge` | those SVDs merge into Espressif's | commit |
| `host-tests` | the host tests of the Python tools, with coverage | commit |
| `soapy` | the SoapySDR module builds (in `build-soapy/`) | commit |
| `docs` | Doxygen and Sphinx with warnings as errors | push |
| `unit-tests` | twister on `native_sim/native/64` (the shell has no 32-bit glibc) | push |
| `build-boards` | twister `--build-only` for the boards | by hand |

- `pre-commit run --hook-stage manual <hook> --all-files` runs one on demand; `build-boards` only
  runs this way.
- `SKIP=<hook>[,<hook>] git commit` (or `git push`) skips some, for example offline.
- `docs` fetches Zephyr's intersphinx inventory, and `svdmerge` downloads Espressif's SVDs into
  `.cache/esp-sdr-zephyr/svd` in the west workspace the first time.

## Other tools

Measuring tools, not pass/fail gates; the sweep tables are under [Sweep tables](#sweep-tables):

- `scripts/place/`: where code and data went and how hot it is: the build report, `place_diff.py`,
  `prof_map.py`, `hot_text.py` and `icache.py` ([Code placement](code-placement.md#tools)).
- `apps/rfprobe` and its `tools/rfprobe.py`: LO pair sweeps with a tone between two boards, raw
  PLL sweeps, PLL register scans, analog register dumps and diffs, the receive noise floor.
- `apps/wpan/tools/interop.py` and `iq_dump.py`: two-board interop runs and raw captures.
- The pcap bench check: `apps/pcap/tools/esdr_pcap.py -w` on the badge, then tshark, with 802.15.4 from
  `apps/wpan` on an S3 (`wpan on`, `wpan set chan 25`, `wpan tx 400 20 20`). The test frames'
  bytes follow from their counter, so every wrong symbol shows.

<!-- nav-start -->
Previous: [Code placement](code-placement.md) · Up: [Reference](index.md) · Next: [ZigBit reference](zigbit.md)
<!-- nav-end -->
