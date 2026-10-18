# Testing

Every pass/fail test of the firmware is a twister test; all but the S3 variant of the integration
test also run end to end under twister (that one is deployed by hand, see below). The Python tools
have host tests of their own (pytest). Five tiers:

| Tier | Where | Runs on | Covers |
| --- | --- | --- | --- |
| Unit | `tests/unit/` | host (`native_sim/native/64`), no board | Portable code as ztest suites: the 802.15.4 PHY (`ieee154_phy`), the driver's signal processing (`ieee154_dsp`), link RS(255,223) (`link_rs`), BCH (`link_bch`) and Hamming(128,120) (`link_hamming`), the link's QAM and OFDM modems (`link_qam`, `link_ofdm`: frames through a simulated channel and decoded, as the host simulators in `apps/link/tools` do), the `stream` app's I/Q packing (`iq_pack`), the diagnostic-bus ring's box16 and CIC decimator (`diag_dsp`, bit-exact against naive references), the shared signal processing helpers of `esp_sdr_dsp.h` (`dsp`: windows, sinc, the FFT against a direct DFT, whole-cycle tones), the channelized burst capture's filter bank, decimator bank and detector (`chancap_dsp`, against a direct floating point model), the Ubertooth app's USB requests, clock and queue (`ubertooth_core`), its GFSK modulator and packet builders (`ubertooth_tx`), its spectrum analyser (`ubertooth_specan`), its BLE packet finder and connection follower (`ubertooth_le`, against golden vectors of `tools/le_ref.py`), the Zigbee stack of `lib/zigbee` (`zigbee_crypto` and `zigbee_frame` against independent implementations, `zigbee_znp`, `zigbee_mac`, `zigbee_stack`). With `IEEE154_TABLES` set, `ieee154_phy` and `ieee154_dsp` also print their sweep tables (`scripts/unit-tables.sh`). |
| Host | `tests/host/` | host (CPython), no board | The Python tools that run in builds and CI: multiapp's build steps (`mkfat.py`, read back by a FAT reader written from the spec; `apptrail.py`, `appconfig.py`, `buildid.py`, `exports.py`), the MicroPython app tools (`mkegg.py`, `eggs_index.py`, `check_capture.py`), the register page tools, `twister-flash.py` and `coverage-summary.py`; the MicroPython app's packet modem (`test_fsk.py`: the Python reference through a noisy channel, the C modem against it) and its Data link egg, the unmodified export on the gnuradio stand-ins against a simulated peer, also 24 kHz off (the receiver follows it) and with a receive ring that loses input (`test_datalink.py`); chancap's BLE pcap tool on the ubertooth_le golden packets, GFSK modulated with noise and a carrier offset at 2 and 4 samples a symbol (`test_ble_pcap.py`). |
| Integration | `tests/integration/` | one real board | ztest on the radio itself. `esp_sdr_rx_api`: the receive API's contract (error codes, bounds, one real capture per supported rate); on the S3, C6 and C5. `esp_sdr_diag_api`: the C5's gapless diagnostic-bus ring (argument contract, a 10 s run's byte rate and integrity, every step of the decimation ladder, stop and restart). Need no antenna, peer or host tool and never transmit. `esp_sdr_replay_api`: the C5's DAC-engine replay (argument contract, pass time at 80 and 40 MS/s, repeat count, restarts, receive afterwards); plays all-zero words, so only the LO leaks out: keep the board on its coax link. `esp_sdr_chancap_api`: the S3's channelized burst capture (start contract and LO plan, the channelizer's timing on synthetic data with the vector kernels checked bit for bit against the C reference, short live runs on whatever is on the air); deployed by hand like the S3 variant of `esp_sdr_rx_api`. |
| Regression | `tests/regression/` | two real boards | pytest through twister's multi-board harness. `ieee802154_link`: the C6 native radio and the S3 software radio exchange `apps/wpan` test frames both ways on channels 11, 15, 20 and 25, plus ACK requests. Guards against the two bugs that only ever showed as lost frames between real boards (`doc/hardware-quirks.md`). `lo_tuning`: the S3 sends tones to the C6 at overlapping LO frequencies, through the 5/6 divider and directly tuned; guards the transmit divider and `-ERANGE`. `stream_gapless`: the C6 and the C5 on a coax link stream a tone to each other through `apps/osmosdr_serial` (receive by the CPU-paced estimator or by the C5's diagnostic-bus ring); no gap may be found in the recording. |
| Smoke | `apps/*/sample.yaml` | build, and one board | Every app builds for its boards; `apps/capture` also boots on the S3 and the C6 and must print `esp-sdr rx: ready` (console harness); `apps/multiapp` boots into an app from its file on the FAT volume (pytest, `sample.esp_sdr.multiapp.hil`). |

## Continuous integration

`.github/workflows/build.yml` runs on every push and pull request. It builds every app and test
for each board it allows (`xiao_esp32s3/esp32s3/procpu`, `xiao_esp32c6/esp32c6/hpcore` and
`waveshare_esp32c5_zero/esp32c5/hpcore`, one job each) and builds and runs the unit tests on
`native_sim` and `native_sim/native/64`. A GitHub runner has no boards, so the integration and
regression tests and the console smoke test are only built there; run them on the desk as below.
The twister reports of each job are kept as artifacts. The unit test job also measures code
coverage (see [Coverage](#coverage)): its summary page shows a table per source file, and the
`coverage` artifact holds gcovr's HTML report. The "Host tools" job runs the host tests under
coverage the same way: a table on its summary page, the HTML report in `host-coverage`.

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

`pre-commit run --hook-stage manual <hook> --all-files` runs one on demand (`build-boards` only
runs this way), and `SKIP=<hook>[,<hook>] git commit` (or `git push`) skips some, for example
offline: `docs` fetches Zephyr's intersphinx inventory and `svdmerge` downloads Espressif's SVDs
into `.cache/esp-sdr-zephyr/svd` in the west workspace the first time.

## Unit tests

```
esp-sdr-zephyr/scripts/run-unit-tests.sh
esp-sdr-zephyr/scripts/run-unit-tests.sh -s unit.esp_sdr.link_rs    # one suite
```

The script runs twister on `native_sim/native/64` with `NSI_OPT=-O2`. Plain `native_sim` is
32-bit and this nix shell has no 32-bit glibc. Without an optimization level, glibc's
`_FORTIFY_SOURCE` check warns inside native_sim's own runtime code, and `-Werror` makes that
fatal.

`tests/unit/link_rs` builds `apps/link/src/rs.c` with `-U__ZEPHYR__`, so its host branch is
compiled instead of the ESP-IDF one. The suites draw their random input from
`tests/common/include/test_rng.h` (xorshift64 and normal deviates), seeded before each case.

### Coverage

```
NIX_HARDENING_ENABLE= esp-sdr-zephyr/scripts/run-unit-tests.sh \
  --coverage --coverage-basedir . --coverage-formats html
esp-sdr-zephyr/scripts/coverage-summary.py esp-sdr-zephyr/twister-out/coverage_summary.json
```

Coverage builds without optimization. The nix shell's compiler wrapper defines
`_FORTIFY_SOURCE` regardless, and glibc's warning about that is fatal under `-Werror`; the empty
`NIX_HARDENING_ENABLE` turns the wrapper's hardening off for the run (Ubuntu's gcc, as in CI,
defines it only when optimizing).

Twister builds the suites with `CONFIG_COVERAGE`, runs them and has gcovr merge the results (the
script runs twister from the repository root): `twister-out/coverage/index.html` per file and
line, and `twister-out/coverage_summary.json` for `scripts/coverage-summary.py`, which prints the
Markdown table CI puts on the job's summary page. The test sources themselves (`tests/`) are left
out. Coverage counts only the files some unit test compiles; the script lists the C sources under
`lib/` and `apps/` that none does (the radio drivers and the app glue, which only the integration
and regression tests reach on the boards).

### Sweep tables

`ieee154_phy` and `ieee154_dsp` each hold one more test case that is skipped unless the
environment variable `IEEE154_TABLES` is set (to anything): the PHY's packet error rate over SNR
and carrier offset (`test_per_table`), and the decoded fraction of the driver's chain through each
decimator over SNR (`test_decimator_table`; with the variable set, `test_cfo_estimator_accuracy`
also prints its carrier offset estimates). These are the tables `doc/ieee802154.md` quotes, and
what tuning the PHY or a decimator needs.

```
esp-sdr-zephyr/scripts/unit-tables.sh          # both
esp-sdr-zephyr/scripts/unit-tables.sh phy      # the PHY error rate table
esp-sdr-zephyr/scripts/unit-tables.sh dsp      # the decimator table
```

The script runs the two tests with `IEEE154_TABLES=1` and prints their logs. On an idle machine
each table takes between half a minute and a minute, plus the build; concurrent builds can triple
that, hence the generous twister timeouts of these ids (300 s and 900 s). The table cases also
assert what the documentation claims (`phy`: no frame lost from 6 dB SNR up, none twice, none with
wrong content; `dsp`: each decimator column reads 1.000 from 2 dB above the SNR where it first
does), after printing the whole table, so a regression fails the run and still shows the numbers.
Without the variable the cases are skipped and the unit tests stay at a few seconds.

## Host tests

```
esp-sdr-zephyr/scripts/run-host-tests.sh              # all, then the coverage report
esp-sdr-zephyr/scripts/run-host-tests.sh -k mkfat     # some (pytest arguments)
```

pytest under coverage.py, under a minute (most of it the Data link simulation); the nix shell
has both, `tests/host/requirements.txt` pins them (with PyYAML, pyelftools and numpy)
elsewhere. The tools are loaded by path (`tests/host/conftest.py`). `test_exports.py` compiles
a small shared library with the host compiler and is skipped without one; so do the C parts of
`test_fsk.py` and `test_datalink.py` (`apps/micropython/cmod/esdr_fsk.c`, through ctypes). `tests/host/coveragerc` sets what counts: the build and CI
tools under `apps/multiapp/`, `apps/micropython/tools/`, `scripts/` and `doc/registers/`, files
no test imports included; the bench tools under `apps/*/tools` need boards and stay out.

## Integration tests (one board)

Each test has one id per board: `integration.esp_sdr.rx_api.c6`, `.c5` and `.s3`;
`integration.esp_sdr.diag_api.c5` (the SoCs with a gapless diagnostic-bus ring; another SoC adds
its id and `boards/<board>.conf` to that test); `integration.esp_sdr.replay_api.c5` (the same for
the DAC-engine replay). The C5 and the C6 both have a USB-Serial-JTAG
console, deployed the same way:

```
west twister -p waveshare_esp32c5_zero/esp32c5/hpcore -T esp-sdr-zephyr/tests/integration/esp_sdr_diag_api \
    --device-testing --flash-before \
    --device-serial-pty "python3 .claude/skills/esp32s3-console/s3pty.py <by-id port>" \
    --west-flash="--esp-device=<by-id port>" --outdir <dir> --runtime-artifact-cleanup pass
```

`esp_sdr_diag_api` runs about 25 s and drives 8 pads as outputs (toggling at up to 40 MHz): nothing
may be wired to the C5's GPIO 0, 1, 3, 4, 5, 7, 10 and 23. Its checks are the ring's contract, not
a signal: every byte of 40 MB/s arrives for the time of the run (to 2 %), no block is skipped, the
interrupt never falls behind, the byte count follows the system timer (a byte lost per ring wrap
would be -17 ppm; the bound is 5 ppm), the consumer's cost stays under the 6 cycles per byte the
CPU has at 240 MHz, the receiver's noise reaches the pads (the ring is not a constant), and each
step of the decimation ladder (250 to 15.625 kS/s) has the right rate, a gapless index and the
right count.

`esp_sdr_replay_api` runs about 10 s. It times the engine against the CPU cycle counter: a pass of
n words takes n x 3 cycles at 80 MS/s and n x 6 at 40 MS/s (240 MHz CPU) plus a start of under 400
cycles, and so do 255 passes in a row, which shows the passes follow back to back. Pointing the 40
MS/s bit at bit 15 instead of 17 fails the two 40 MS/s cases (146363 cycles for 96000).

- C6: flashed directly with twister's device testing. Opening the C6's USB-Serial-JTAG port
  resets the chip, and the suite finishes before a plain `--device-serial` attach after the
  flash would see it: open the console after the flash so the reset reruns the suite
  (`--flash-before`), or use a `--device-serial-pty` script that resets the chip once it is
  attached.
- S3: the board normally runs an app that owns the USB OTG port, so esptool cannot reset
  it into the ROM bootloader. The S3 variant therefore builds with sysbuild and MCUboot and
  has its own CDC-ACM shell and DFU. Deploy it with `west dfu -d <build dir> -s <usb serial>`
  and read its CDC-ACM console. The suite sleeps 10 s after boot so the host can enumerate the
  port first: CDC-ACM does not replay output printed before a reader attaches.

## multiapp desk check (one board)

`sample.esp_sdr.multiapp.hil` builds the multiapp host with `hello` as its autostart app
(`apps/multiapp/hil.conf`). `scripts/twister-flash.py` puts the image on the S3 with `west dfu`
and, because the build has a FAT volume image, the volume with `west dfu --storage`;
`apps/multiapp/pytest/test_multiapp.py` then checks that every app's file is on the volume and
equals its built-in copy, that the boot ran `hello` from its file, and that hello's commands
answer, its PSRAM atomics included. From the repository root, with the S3 attached:

```
west twister -T apps/multiapp -s sample.esp_sdr.multiapp.hil --device-testing \
    --hardware-map tests/regression/hardware-map.yaml \
    --flash-command "$PWD/scripts/twister-flash.py"
```

About a minute, flashing included. It leaves the test image on the board: `west dfu` and
`west dfu --storage` of a normal multiapp build afterwards.

## Regression tests (two boards)

Three scenarios, all pytest through twister's multi-board harness, each with its own pair of
images:

- `tests/regression/ieee802154_link`: 802.15.4 frames between the C6's native radio and the S3's
  software radio.
- `tests/regression/lo_tuning`: tones from the S3 to the C6 at overlapping LO frequencies.
- `tests/regression/stream_gapless`: a tone streamed between the C6 and the C5 through a coax
  link, both ways, with no gap in the recording.

Each has three ids: `regression.esp_sdr.<test>.c6` and `.s3` only build the two images
(`build_only`); `regression.esp_sdr.<test>` runs `pytest/` on both boards (`required_devices` and
`required_applications`). `stream_gapless` has the C6 and two C5 images (`.c5`: the receive paced
by the CPU through the IQ estimator, `.c5_diag`: from the diagnostic-bus ring) and a scenario for
each: `regression.esp_sdr.stream_gapless.c6_c5` and `.c6_c5_diag`.

Run them with both boards attached:

```
esp-sdr-zephyr/scripts/run-regression-tests.sh -O <output dir>
```

For each scenario it builds the two images (the scenario id builds none of its own), puts them on
the boards and runs the tests, a few minutes in all; `-s regression.esp_sdr.lo_tuning` picks one.
A relative `-O` is relative to the repository root. What is specific to a desk is in
`tests/regression/hardware-map.yaml` (change the serial numbers for another desk), apart from
the radio levels of `lo_tuning` (below). Four pieces make twister cope with these two boards:

- `scripts/twister-flash.py`, passed as `--flash-command`. Twister calls it once per board with that
  board's build directory and its id from the map. An MCUboot image (the S3) goes on with
  `west dfu -s <id>`, anything else with `west flash` to the port of that id. Twister's own `west
  flash` (esptool) cannot reset an S3 whose app owns the USB OTG port. Both boards flash at once.
  Afterwards `/tmp/esp-sdr-tty/<id>` points at the board's port, found by serial number alone;
  the map names the S3's port by this link, because its `/dev/serial/by-id` name carries the USB
  product string of whichever scenario's image is on it.
- `flash_before: true` in the map: the console is opened after the flash, because the C6 resets when
  its port is opened and the S3's port goes away and comes back during the DFU update.
- `pytest_dut_scope: module` and a generous `timeout` in each scenario: both boards are flashed
  once for all tests, and the flashing counts against the timeout.
- `tests/regression/conftest.py` presses Ctrl-C on each shell before twister looks for the
  prompt. Twister presses Enter with a bare LF, which a Zephyr shell swallows after a CR
  (`process_nl()` in `subsys/shell/shell.c`), and `west dfu` ends its lines with CR LF, so the S3
  would never show a prompt. Pytest loads this conftest for both scenarios because twister runs it
  from its own working directory (the repository root with the script), which with no pytest ini
  file in the tree becomes pytest's rootdir.

Twister compiles with `-Werror`, a plain `west build` does not. With CDC-ACM as console and shell,
`usbd_cdc_acm.c` turns its own logging off with a `#warning`, which is fatal under twister. Every
S3 app with a CDC-ACM shell therefore sets `CONFIG_USBD_CDC_ACM_LOG_LEVEL_OFF=y` itself.

### ieee802154_link

`tests/regression/ieee802154_link` builds `apps/wpan` from its own directory: `src/main.c` by
path, and `APPLICATION_CONFIG_DIR` and `KCONFIG_ROOT` point Zephyr's config lookup there, so
`prj.conf`, `boards/` and `Kconfig` are the app's own and cannot drift. Only `sysbuild.conf` is a
copy, because sysbuild looks it up before the image's `CMakeLists.txt` runs.

What passes: on channels 11, 15, 20 and 25, in both directions, 100 test frames of 100 bytes at 5 ms
spacing. Every frame the transmitter completed must arrive, none corrupt or twice. A transmitter may
fail up to 3 of the 100 itself: the S3's DAC generator fills a block late about once in 400 frames
and cuts the frame (`-EIO`), and the C6 now and then reports a tx error of its own. That is not a
receive loss, which is what the test is for (the Wi-Fi MAC interrupt, `doc/hardware-quirks.md`,
and the sample ring generation race, `lib/ieee802154/ieee154_esp_sdr.c`, both showed as frames
the sender had sent that never arrived), so it is bounded loosely and the receiver is held to what
the transmitter reports as sent. The ACK check sends 20 frames with an ACK request from the S3 to
the C6 and allows 2 to fail or go unanswered. When a check fails, the message carries `wpan
status` of both boards.

`apps/wpan/tools/interop.py --native <C6> --sdr <S3> --channels 11,15,20,25` runs a similar exchange
by hand, with its own defaults (200 frames of 16 bytes, 10 ms apart) and, when a direction fails
completely, the I/Q inversion fallbacks.

### stream_gapless

`tests/regression/stream_gapless` builds `apps/osmosdr_serial` from its own directory, as
`ieee802154_link` does `apps/wpan` (`diag.conf` of the app selects the ring for the `.c5_diag`
image). It needs the C6 and a C5 on a coax link with a 30 dB attenuator between their U.FL
ports, 2437 MHz. The pytest asks twister for the flashed boards, lets go of their consoles (the
stream is binary) and drives them with `apps/osmosdr_serial/tools/esdr_serial.py`.

For each direction (C6 to C5, C5 to C6) and for the receive rates 250 and 31.25 kS/s, one board
transmits a tone at 125 kS/s; after 15 s (the transmitter's tone drifts a few ppm while it warms
up) the other records 10 s. What passes:

- a usable link: the tone at least 30 dB over the median bin and inside the band;
- the samples the stream owes (98 %), no index jump, no overflow flag, no lost sample counted by
  the device (`rx_overflows`, `rx_ring_lost_pairs`);
- `apps/osmosdr_serial/tools/gapcheck.py` finds no gap in the tone: it tracks the tone segment by
  segment, demodulates, averages blocks of 64 samples and looks for a spike in the block phase (k
  lost samples are k * 360 * f / fs degrees) or a dip of the amplitude (zero-filled samples).
  Phase noise of the blocks is about 1 degree at the bench levels, the threshold 30 degrees;
- the transmitter played 99.5 % of what it was given (`tx_underruns`) and the host fed it in
  real time.

The levels are for this desk: C5 receive gain index 40 (about 10 dB below full scale; from index
48 up the C5 clips), C6 receive index 30, transmit step 2. Override them with
`ESP_SDR_STREAM_<C5|C6>_RX_GAIN` and `ESP_SDR_STREAM_<C5|C6>_TX_STEP`, and the recording time
with `ESP_SDR_STREAM_CAPTURE_S`. The diagnostic-bus scenario was checked to fail on the bug it guards: serving the USB FIFO once
per 8 KiB ring block instead of every 2 KiB failed `c6_to_c5` at 250 kS/s with 1.55 M of 2.5 M
samples, 758 index jumps and 761 receiver overflows, while the other three streams passed. A
failure carries the evidence in one line: sample count, flags,
index jumps, the tone's frequency and level, and the device's counters. `gapcheck.py` also runs
by hand on any recording (`esdr_serial.py <port> rx --out x.npy`, then `gapcheck.py x.npy --rate
250000`; `--selftest` checks the detector itself). Another SoC pair is another entry in the
pytest's `CHIPS` and the gains, and another set of ids in `testcase.yaml`.

### lo_tuning

`tests/regression/lo_tuning` is a small firmware of its own for both boards, with a `lo` shell
command: `lo f` tunes and prints `esp_sdr_set_freq()`'s return code, `lo tone on`/`off` loops a
tone on the S3 (loop playback, no gaps), and `lo rx` captures 8192 samples at 80 MS/s on the C6
and prints the strongest tone and the level at an expected offset. The C6 does the analysis
itself: a Welch average of eight 1024-point FFTs, each bin compared with the median of the bins
within 1 MHz, so the C6's uneven noise floor does not count as a tone. Every filter is open
(receive and transmit low-pass code 0). The C6 receives at fixed gain index 40 on its U.FL port
and the S3 transmits at its weakest step: on this desk the tones arrive 35 to 51 dB above the
floor without clipping. Another RF path (an attenuator, antennas) may need other `RX_GAIN` and
`TX_STEP` in `pytest/test_lo_tuning.py`. Before any other check the scenario measures the link at
2412 MHz, a Wi-Fi channel tuned the vendor's way, and stops with a "no usable link" message if the
tone is missing there.

A tone sent at TX LO + 3 MHz must show up at (TX LO - RX LO) + 3 MHz, at least 20 dB above the
local floor, the strongest one in the capture, within 150 kHz (the two crystals plus an FFT bin).
Equal LOs alone would miss two PLLs that both land on the same wrong frequency, so each frequency
is also checked with the boards 10 MHz apart, both ways round. The scenario checks:

- 1900 to 2610 MHz, in the divider band (PLL at 6/5) and directly tuned. Together with the pairs
  around 2210 MHz they keep the PLL between 2210 and 2651 MHz, the envelope upstream esp-sdr
  qualified on all chips.
- The switch at 2209/2210 MHz: divider on one board, direct tuning on the other.
- A 2000 MHz transmission is absent at 2400 MHz + 3 MHz, where it went out before the transmit
  path selected the divider.
- With the transmitter silent, nothing at the offset (the detector itself).
- `-ERANGE` on both boards at 1000, 1700, 3000 and 5000 MHz.
- No `-ERANGE` means the PLL locks. The S3's window edges are found by tuning MHz by MHz, from 1880
  MHz down and from 2610 MHz up, and must exist. At the last 3 MHz inside each end, wherever both
  boards tune without `-ERANGE` (the C6 8 MHz further in, or 16 if it reports `-ERANGE` itself), the
  tone must arrive; at least 2 of the 3 per end must tune. `-ERANGE` itself is conservative
  (`esp_sdr.h`) and is not checked for being reported too often.

## Other tools

Measuring tools, not pass/fail gates (the host tables are in the unit tests, above):

- `apps/rfprobe` and its `tools/rfprobe.py`: LO pair sweeps with a tone between two boards, raw PLL
  sweeps, PLL register scans, analog register dumps and diffs, the receive noise floor.
- `apps/wpan/tools/interop.py` and `iq_dump.py`: two-board interop runs and raw captures.
