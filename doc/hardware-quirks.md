# ESP32-S3 SDR: hardware quirks and how they are handled

Behaviour of the ESP32-S3 radio, its undocumented capture/DAC engine and the
toolchain that matters when you build on the `esp_sdr` library, found while
developing `apps/link` and `apps/sdr_stream`. Each entry says what happens,
how it shows up, and where the code deals with it. Measurements are from two
XIAO ESP32S3 boards cabled through a 30 dB attenuator.

## Radio

### The forced receive gain gets lost

**What happens.** A manual receive gain (`esp_sdr_rx_set_gain()`, forced
through the vendor blob's `force_rx_gain()`) is occasionally lost after
minutes of traffic, on either board. The Wi-Fi AGC then takes over. With
nothing on air it winds the gain up, and the quiet noise floor rises by up
to 30 dB.

**How it shows up.** Carrier sense reads a busy channel all the time. A
sender waiting for an ACK never transmits again. A receiver clips strong
frames and decodes bursts of corrupted headers. It looks like hardware
damage, but re-applying the gain cures it at once. The cause is unknown,
and comparing the AGC force register (`0x6001c02c`) does not detect it:
`force_rx_gain()` writes other registers too.

**How it is handled.** The library re-forces the gain at a capture when the
last force is more than 20 ms old (`gain_refresh()` in
`lib/esp_sdr/src/esp_sdr_rx.c`, `GAIN_REFRESH_MS`). Each re-force costs 14 to
51 us. `esp_sdr_get_stats()` counts them, and `link status` prints them on
its engine line. A turnaround back to receive also forces the gain.

When debugging this kind of fault: any `link set` in `apps/link` re-applies
the whole radio configuration, which hides it.

### 40 MHz mode widens the analog filters

The PHY's channel bandwidth mode (`esp_sdr_set_channel_bw()`, the second
argument of the blob's `set_chanfreq()`) selects the analog filter. In its
40 MHz mode (2) the TX and RX path passes +-18 MHz within +-3 dB end to end:
a 36 MHz OFDM frame decodes error free. The 20 MHz mode (0) cuts it off.
`apps/link` defaults to `CONFIG_APP_CBW=2`. The spectrum edge of a
single-carrier signal is a misleading measure of this, since it mixes the TX
filter, the RX filter and the pulse shaping. Measure the channel per
subcarrier instead (`ofdm rx -v`).

### The transmitter compresses before the DAC words clip

Wide multicarrier signals distort in the analog TX path long before the
10-bit DAC words reach full scale. At an RMS amplitude of 120 DAC units, 116
subcarriers drop to 16 dB MER; at 60 to 90 they reach 26 to 29 dB. The
digital clip counter stays at 0, and lowering the RX gain does not help,
which places it in the transmitter. The OFDM default is `amp 90`.

### Receive gain headroom

Through the 30 dB attenuator at TX step 4, RX gain 32 is the working point
and 44 saturates the receiver (MER 4 dB).

### MER ceilings

| Signal | MER |
|---|---|
| Single carrier QAM, 20 Mbaud, 80 MS/s | about 31.5 dB, with or without attenuation |
| OFDM 16 MHz, DAC at 80 MS/s | about 31 dB |
| OFDM, DAC at 40 MS/s, 16 / 22 / 26 / 30 MHz | 27 / 25 / 24 / 23 dB |

The MER ceiling rules out 256-QAM (about 33 dB needed) on these boards.

### Carrier and sample clock offset

The two boards differ by about 19 to 21 kHz at 2412 MHz (about 8 ppm), and
the sample clocks by the same ratio. Within a frame the estimate can be off
by a few kHz, so phase tracking must follow a frequency error, not only a
phase. `apps/link/src/ofdm.c` takes the residual offset from the phase step
between its two pilot symbols and tracks it with a second order loop
(`track()`, `RATE_GAIN`). The QAM receiver derives the sample clock offset
from the carrier offset.

### Looped frames are not phase coherent across copies

Phase noise over one loop period makes copies of a looped frame differ in
phase. Decode one contiguous copy (OFDM), or measure the phase step at the
wrap from the overlap of two copies (QAM, `j->r.wrap` in `qam.c`).

### Receiving at 40 MS/s

`ESP_SDR_RATE_40MSPS` captures go through the radio's decimation filter,
with the same MER as 80 MS/s and half the samples to process. The filter
spreads each symbol by about +-8 samples, so an OFDM FFT window that starts
only a quarter of the prefix after its beginning picks up the previous
symbol. `ofdm_rx_begin_period()` measures the window position from the
pilots' phase slope across the subcarriers and moves the window to the
middle of the prefix ("fine timing").

The DAC also runs at 40 MS/s (`esp_sdr_tx_loop_begin(ESP_SDR_RATE_40MSPS)`),
at about 4 dB less MER than 80 MS/s.

## Capture and DAC engine

The capture and DAC playback use the Wi-Fi MAC's undocumented dump engine
(layout from upstream esp-sdr, see `lib/esp_sdr/src/esp_sdr_priv.h`).

- **A bank is off limits while the engine owns it.** The engine writes or
  reads one 64 KiB SRAM bank, selected one-hot in `MAC_DUMP_USAGE`. While it
  does, the CPUs cannot reach any of that bank. Nothing else may live there.
  `esp_sdr_bank_tx.ld` reserves the capture bank (0x3fcd0000) and, with
  `CONFIG_ESP_SDR_BANK1`, the transmit bank (0x3fcc0000), and lowers
  `_heap_sentry` below both. A link error "DRAM image overlaps the transmit
  bank" means the DRAM image grew into it.
- **Short captures go unreported.** The engine can stop early without an
  error. `capture_locked()` writes a sentinel into the first and last word
  and fails the capture if either survives. Checking every word cost 1.2 ms
  for a full bank.
- **Capture length.** At most 16380 samples (`ESP_SDR_SAMPLES_MAX`, a 14-bit
  count). A looped frame must fit twice into one window so that a whole copy
  is always present. That caps the bytes per received window: what matters
  is the payload bits per captured sample.
- **Circular capture.** With `DUMP_CTRL` 0x24000 the engine writes a
  16384-pair ring continuously and exposes its write index at 0x60033d60;
  switching the bank select while it runs splits the stream into gapless
  units. `lib/esp_sdr/src/esp_sdr_ring.c` (from upstream) rotates through
  banks 0 to 2 and proves continuity with sentinel windows: the switch must
  come within 2000 pairs (125 us at 16 MS/s) of its threshold, and a bank
  must be filtered before the engine comes round to it again.
- **The Wi-Fi MAC interrupt must not run inside the ring.** The ring masks
  interrupts on its CPU and opens short windows for the kernel timer and
  IPIs (`irq_window()`). CPU interrupt line 0 is the Wi-Fi MAC's
  (`ETS_WMAC_INUM`), and it fires for every packet the Wi-Fi receiver decodes
  at the tuned frequency, hundreds of times a second on busy air. Its handler
  is far longer than a window, so the ring abandoned units until the run
  failed: the receive path then lost whole frames, in bursts that came and
  went with the neighbours' traffic and flipped with unrelated code changes.
  `esp_sdr_ring_run()` now disables line 0 on its CPU for the run and serves
  the interrupt afterwards. `ring irq windows` and `lines` in `wpan status`
  show how many windows ran and which lines were pending (0x40 is the tick
  timer; 0x41 would be the MAC). With the fix, abandoned units stay at 0 even
  with the C6 sending back to back.
- **Loop playback wraps only while the trigger is held.** `esp_sdr_dac_loop()`
  holds the trigger bit; pulsing it plays one pass. Retriggered bursts
  (`esp_sdr_tx_play_for()`) leave a hole of about 1.35 us each.
- **Turnaround.** The default TX/RX switch with a retune takes about 3 ms.
  `esp_sdr_set_turnaround()` with 50 us and no retune works for packet
  links.
- **Analog I2C must take the PHY's lock.** The vendor PHY serializes its
  analog I2C traffic with `regi2c_enter_critical()`. Every `rom_chip_i2c_*`
  sequence in the library takes it too, because on two cores the Wi-Fi PHY
  code may run one at the same time.

## Memory

On the S3 the engine's 64 KiB banks and the `ESP_SDR_HIGH_RAM` region sit at
the top of internal DRAM, laid out by the linker snippets
`lib/esp_sdr/esp_sdr_bank*.ld` (picked in `lib/esp_sdr/CMakeLists.txt`). Bank 2
(0x3fcd0000) is always the capture bank. `CONFIG_ESP_SDR_BANK1`, which
`ESP_SDR_TX_DAC` and `ESP_SDR_RING` select, adds bank 1 (0x3fcc0000) as the
second DAC and capture bank, and the ring takes banks 0 to 2 (from
0x3fcb0000). Each snippet lowers `_heap_sentry` to its lowest bank, so the libc
heap stays below it, and fails the link if the DRAM image grows into a bank
(see "A bank is off limits" above). The SRAM between the capture bank and the
bootloader's loader segment is what `ESP_SDR_HIGH_RAM` places buffers in.

- **Internal RAM is the scarce resource.** Besides the banks, the region
  above the capture bank (`.esp_sdr_high`, `ESP_SDR_HIGH_RAM`) holds `apps/link`'s
  QAM transmit context and OFDM receive context, with about 128 bytes left.
  "high buffers beyond user DRAM" means it is full. Large buffers that are
  not timing critical go to PSRAM (`EXT_RAM_BSS_ATTR`).
- **The high region reaches 0x3fce9704.** Zephyr's `user_dram_end`
  (0x3fce4f00) only protects MCUboot's loader while the image loads; the
  ROM's download-mode buffers above it are free at runtime, so NOLOAD
  buffers can use the whole range (`DRAM_USER_END`, about 38 KiB).
- **Three banks for the ring** move the DRAM limit to 0x3fcb0000.
  `apps/osmosdr` fits by trimming the network stack (only the Wi-Fi driver
  needs it), and putting sample blocks, filter buffers, thread stacks and
  the DAC backend's buffers (`CONFIG_ESP_SDR_TX_DAC_HIGH_RAM`) in the high
  region. The Wi-Fi driver needs about 15.5 KiB of kernel heap.
- **Not zeroed at boot.** Neither `.esp_sdr_high` nor PSRAM `.bss` is
  cleared. `apps/link` clears what needs it at init (`link_mac_init()`).
- **PSRAM is slow for strided or per-sample work.** RS units and payload
  bytes may live there. Hot buffers (FFT work buffers, the RS codeword being
  decoded, which goes on the stack) should not.

## CPU and toolchain

- **No hardware float divide.** A float `/` calls `__divsf3`. In hot loops,
  compare ratios by cross multiplication (`sc_scan()` in `ofdm.c`). An
  integer `%` by a constant that is not a power of two is a real division
  too; use a power-of-two size and a mask.
- **`lrintf()` is a library call**, out of line in flash. Two per sample in
  the OFDM transmit path cost about 7.5 ms per frame. Round inline (`emit()`
  in `ofdm.c`).
- **Many accumulators spill.** Loops with four or more running sums get
  them spilled to the stack (about 100 cycles per step for the sliding
  Schmidl-Cox sums). Do less work per step instead: the OFDM search scans
  coarsely first.
- **Contraction is off.** Zephyr's ISO C mode disables fused multiply-adds.
  `apps/link` turns them on for its DSP files (`-ffp-contract=fast`).
- **Hot code belongs in IRAM** (`IRAM_ATTR`): flash code goes through the
  cache, which the other core and PSRAM traffic contend for.
- **esp-dsp's S3 FFT kernel assembles empty without the target macro.**
  `apps/link` builds only `dsps_fft2r_fc32_aes3_.S` from the `esp-dsp` west
  project, with `-DCONFIG_IDF_TARGET_ESP32S3=1` (hal_espressif's
  `sdkconfig.h` lacks it), and its own twiddle table in esp-dsp's layout (cos,
  sin, bit reversed; one table built for N serves every smaller size). From
  flash it is still 1.76 times faster than the same algorithm in C from IRAM
  (85.7 against 150.6 us for 256 points). The S3 mask ROM has the twiddle
  table, but no FFT code.
- **A CPU with interrupts masked for seconds.** The ring runs on CPU 1
  under `arch_irq_lock()`: on SMP, `irq_lock()` takes a global lock and
  would stall the other CPU. Nothing may be pinned to that CPU meanwhile,
  no flash writes may happen (code from flash would stall), and the vector
  registers it uses are not part of any thread context.
- **The ring filter's budget** at 16 MS/s is 15 cycles per pair (240 MHz):
  unpack 3.0, the fs/4 mix 1.6, stage 1 4.5, stage 2 2.6 (decimation 64),
  the app's packing about 1. The multiply-accumulate chain on the single
  `accx` accumulator, not the instruction count, limits stage 1.
- **RS(255,223) decoding cost.** About 49 us for a clean codeword, 220 us
  with 2 errors and 400 us with 10. With many corrections, RS decoding
  dominates a receiver.

## Building and flashing

- `apps/link` and `apps/sdr_stream` need `west build --sysbuild` (MCUboot,
  USB DFU). A plain build of an existing sysbuild directory fails with a
  cached-source mismatch.
- Kconfig values passed with `-D` stay in the CMake cache. An old override
  (for example `-Dlink_CONFIG_APP_SPS=8`) persists until set again.
- Update over USB DFU with `west dfu -d <build dir> [-s <usb serial>]`.
  The signed image grows in steps of 64 KiB (flash MMU page alignment), so
  its size is no evidence of a new build.
- Host udev rules match USB product IDs: a build with a new PID cannot be
  opened (or DFU-detached) until the rule covers it. The apps share
  2fe3:0005; `west dfu` detaches whatever PID runs.
- A shell port serves one process: a host tool holding it (such as a running
  `link_perf.py run`) makes another tool's open of the same board fail.
