# Paced streams

<!-- nav-start -->
Previous: [Transmit](transmit.md) · Up: [Samples in and out](index.md) · Next: [Host transports](transports.md)
<!-- nav-end -->

Where no engine path fits, or the rate is low, the CPU paces register reads and writes at the
sample instants. Receive reads the modem's IQ estimator, transmit writes the TX DC value register;
both are introduced in [Samples in and out](index.md#hardware-that-moves-samples). The chain-on
steps for transmit are in [Transmit](transmit.md#turning-the-chain-on).

`esp_sdr_rx_paced_*()` and `esp_sdr_tx_paced_*()` (`esp_sdr_paced.h`) run on the ESP32-C6 and
ESP32-C5 in both directions, and on the ESP32-S31 for transmit.

## How pacing works

- The CPU paces both directions with interrupts locked and serves USB between samples, never so
  late that a sample instant moves.
- Pacing on absolute targets turns stalls into jitter, not lost samples. On the C5 both transmit
  loops see stalls of 3 to 36 us now and then, even with interrupts locked (cause unknown).

### Keep the timed loop in IRAM

Measured on the S31 (VIEWE board):

- rfprobe's loops and input callback run from flash and see late writes now and then, up to 2255
  cycles (7 us) with interrupts locked. The block path's first rfprobe run had the same
  (late_max 2170 to 2556) although its poll is in IRAM: the `tx dc` loop and `dc_input` were still
  in flash, the fill core's fetches sharing that cache. `tx dc` now runs both from IRAM
  (`tx_dc_run`, `dc_input`); not yet re-measured.
- The same loop in IRAM shows none in 40 s.
- The integration test's IRAM stream keeps every rate with a worst lateness of 27 cycles.
- The C5's unexplained stalls came from flash code as well; that they have the same cause is
  untested.

## Receive: IQ estimator sums

The CPU reads the estimator's running sums at the sample rate and sends their differences: each
sample is the sum of 80 MHz / rate inputs, a boxcar decimator. The rates a host can ask for are in
[osmosdr_serial](../../apps/osmosdr_serial/README.rst).

### Boxcar decimation and rates

- At 2 MS/s the C6's CPU reads the sums every 80 cycles, each difference a boxcar of 40 inputs, one
  802.15.4 chip.
- What each link carries is in [Host transports](transports.md).

### Block fill at 2 MS/s

- Above 250 kS/s the items are 8 bit, filled a block at a time by a tight loop
  (`esp_sdr_rx_paced_fill_iq8()`): 59 CPU cycles a sample of the 80 there are at 2 MS/s.
- The digital gain is a power of 2 there.
- The link's turn between blocks makes the next poll a few samples late. The fill gives each sample
  that poll missed an equal share of its sums, so that the samples stay on their grid.
- 2.5 MS/s leaves 3 % of the samples late; 3.2 MS/s does not fit.

### Late polls and the 255 us glitch

This is how the C6's paced receive behaved in the channel mode of pcap
(`lib/esp_sdr/src/esp_sdr_paced.c`) before the block fill above.

- Between calls of `esp_sdr_rx_paced_fill_iq8()`, osmosdr_serial turns to its link (the SDIO DMA
  chain, requests), which outlasts the loop's slack of some 20 cycles.
- The next poll came late, so one sample spanned several windows: a spike of about three times the
  level.
- The loop then caught up with polls back to back, windows of 0.75, skewing some 32 samples
  (16 us at 2 MS/s).
- Filled a quarter block a call, that was a glitch every 255 us. It was found in raw samples
  dumped from the badge: 158 of the gaps between spikes were 255 us.
- It broke one 802.15.4 symbol in about seventy (frames of the S3's software radio: 24 to 29 %
  decoded at any gain, the rest with one or two symbols wrong at a high LQI) and most Bluetooth LE
  packets.

Now the link's turn comes once a block (1.02 ms), and a poll whole periods late gives each sample
it missed an equal share of its sums:

- 97 and 98 % of the S3's frames decode.
- Three times the 802.15.4 frames of a Zigbee network in range arrive.
- Five to seven times the Bluetooth LE packets arrive.

## Transmit: TX DC register

- The host's samples go to the TX DC value register
  ([Samples in and out](index.md#tx-dc-value-register)).
- On the C6 and C5 they pass an 8-tap-per-phase FIR; 250 kS/s goes to the DAC as it is.
- The C6 and C5 tune whole MHz on this path.

### DAC rates per chip

| Chip | DAC rate | Tuning range | Power steps |
|---|---|---|---|
| ESP32-C6 | up to 250 kS/s (125 kS/s x 2) | 2400 to 2500 MHz | 0 to 15, 2 dB each ([osmosdr_serial](../../apps/osmosdr_serial/README.rst)) |
| ESP32-C5 | 500 kS/s (125 kS/s x 4) | 2400 to 5900 MHz | 0 to 15, 2 dB each ([osmosdr_serial](../../apps/osmosdr_serial/README.rst)); on the C5-Zero bench the vendor tone's power step changed nothing on either C5 path ([Gain](../radio/gain.md#level-per-chip)) |
| ESP32-S31 | DAC writes up to 1 MHz per-word, 2 MHz with the SMP block path (option, below): the rate times the interpolation factor (8, 4, 2 or 1, the largest that fits) must divide 320 MHz | the S31's range ([Tuning](../radio/tuning.md)) | the samples carry the level (below) |

### Store cost and rate ceilings

| | ESP32-C5 | ESP32-S31 |
|---|---|---|
| A store to the TX DC register | 15 CPU cycles | 48 CPU cycles at 320 MHz |
| A load from it | 18 cycles | 48 cycles (TCM 6 and 5, the cycle counter 8) |
| The library's poll | about 177 cycles per sample (its loads from `.bss`, code from flash): keeps the rate up to 1.35 MS/s | per-word path: about 220 cycles a write at x8, at -O2 from IRAM; block path: the store, the pop and the swap (about 46 + 7 beyond the store), the rest on the other core (below) |
| A loop that only stores prepacked words | exactly up to 8 MS/s (ceiling about 8.8) | 2 MS/s exactly, 5 MS/s with a few late writes; at 8 MS/s a store no longer fits its 40 cycles |

On the S31 the per-word path (the default) keeps 250 kS/s x4 (320 cycles a write) and 125 kS/s x8
at most; 250 kS/s x8 falls behind (181k inputs a second).

#### Block path (S31, SMP, `CONFIG_ESP_SDR_PACED_TX_BLOCK=y`)

`esp_sdr_tx_paced_poll()` only stores a DAC word and takes the next from a block of
`CONFIG_ESP_SDR_PACED_BLOCK_WORDS` (128) words. A thread on the other core computes the words
(`esp_sdr_paced_blk.c`: the input callback and the FIR) into a ring of `CONFIG_ESP_SDR_PACED_BLOCKS`
(4) blocks. Why not one core: the fill costs 78 to 91 cycles a word (128 units a step) up to 162 to
208 (one unit) on top of a store of 52 and a poll of 46 cycles; a write every 160 cycles (2 MHz) does
not hold both, and at 1 MHz or less the single-core block path was no better than the per-word one
(worst lateness 76 to 111 cycles against 27). See [Hot loops](../dev/hot-loops.md), item 6.

- **Cores.** `esp_sdr_tx_paced_start()` creates the fill thread (cooperative, no kernel call in its
  loop) pinned to the core the caller is not on: CPU 1 for a caller on CPU 0 (a micropython burst
  runs on the interpreter thread, CPU 0, so its fill is on CPU 1), CPU 0 for one on CPU 1. It spins
  there until `esp_sdr_tx_paced_stop()`, which waits for it to end; a start after a stop is clean, a
  start while one runs gives `-EBUSY`. It outranks every thread on its core: those are frozen for the
  stream's length, so the caller must not need a mutex or a service of such a thread between start
  and stop. Start and stop sleep: not with interrupts or the scheduler locked.
- **The caller stays on its core** from the start to the stop (pin it; `CONFIG_SCHED_CPU_MASK_PIN_ONLY`
  does). A caller moved onto the fill core would not run again before the stop;
  `esp_sdr_paced_sync()` returns `-EPERM` if it finds itself there.
- **Sync can fail.** `esp_sdr_paced_sync()` returns `-ETIMEDOUT` when the blocks do not come within
  1 s (an input callback that blocks, a held fill core). Stop the stream then: until a successful
  (second) sync the poll replays the idle word, counts `starved`, and `esp_sdr_tx_paced_samples()`
  stays 0, so a loop that waits on it needs its own bound (esdr_local's burst has one).
- **No fall back.** If there is no second core, the pin is refused or the core does not run the
  thread within 200 ms, start returns `-ENODEV` and touches no hardware. The per-word path is
  another build (the option off), not a silent degradation.
- **Sync is required.** The thread takes the first two blocks of inputs in `esp_sdr_paced_sync()`,
  while the caller waits (busy, interrupts may be locked), then fills the free blocks ahead: the
  input callback runs on that thread, up to `CONFIG_ESP_SDR_PACED_BLOCKS` blocks (at x8, 64
  inputs for four) ahead of the DAC. `samples` and `events` count what has played.
- **Locks.** Lock interrupts around the poll loop with `arch_irq_lock()`: under SMP `irq_lock()`
  takes the global lock, which a kernel call on the other core would wait for. The input callback
  must take neither, and must not block. Its data is shared by two cores: set it up before the
  start (or the sync), read its results after the stop.
- **Between the cores.** A block's state (free, ready, playing) is the only shared word, written
  with release and read with acquire; words, miss maps and history belong to one side at a time.
  Internal RAM has no cache between the cores. A block not ready at the swap (`starved`) is played
  again; four blocks absorb a stall of the fill core of up to three blocks of playing time.
- Memory (internal RAM, in the image): the ring and the history 2.9 KB, the thread's stack 1.7 KB
  (`CONFIG_ESP_SDR_PACED_FILL_STACK_SIZE` 1024 plus the kernel's guard), its thread object 0.3 KB;
  IRAM: the poll 148 B, the fill code about 2 KB. The stop logs the stack left (with `INIT_STACKS`).
- Tests: `tests/unit/paced_blk` (native_sim, rings of 2 and 4 blocks, bit exact against the
  per-word path) and `tests/integration/esp_sdr_tx_paced_api` scenario `.s31.smp`
  (`test_block_rates`: 250 kS/s x8, 500 kS/s x4, 400 kS/s x4, 125 kS/s x8 asserted, a ladder up
  to 3.2 MHz of writes printed only).
- Measure with rfprobe (built with SMP and the option): `tx dc 250000 31250 2000 8000 8` and
  `tx dc 500000 62500 2000 8000 4` print late, late_max and starved.

### S31 measurements

Measured on the VIEWE board (rfprobe and `tests/integration/esp_sdr_tx_paced_api`), heard on a XIAO
ESP32-S3 a few cm away (`tmp/s31/tx/paced4.py` to `paced6.py` in the workspace).

#### Clock and sign

- The level follows the samples only ([Gain](../radio/gain.md#level-per-chip)).
- A 100 kHz tone at 2 MS/s lands 100.08 kHz above the S31's own LO line in the same capture, on the
  + side. A 300 MHz clock would put it at 93.75 kHz.
- So the clock and the sign check out: I and Q need no swap.

#### Interpolation images

The images the interpolation leaves, for a 31.25 kHz tone at 8000 of 32767:

| Product | Level (dBc) | Note |
|---|---|---|
| Input-rate images | -17 at x1; -53 to -57 from x4 | |
| DAC-rate hold images (1 MHz at 250 kS/s x4 and 125 kS/s x8) | -30 | as a zero-order hold predicts |

The I/Q image, the LO leakage and the third order of the same tone are in
[Impairments](../radio/impairments.md#transmit-per-chip); where the transmitter compresses is in
[Gain](../radio/gain.md#transmitter-compression).

#### Buffered bursts

MicroPython's `esdr.tx_start(buf=...)` plays a buffer once through this path, as the C5 does. Its
limits (buffer size, interrupt lock, statistics, cost per call) are in
[micropython](../../apps/micropython/README.rst).

#### No hardware pacing

Pacing the TX DC store from the CPU is the only way found: ETM has no task that writes a register
value.

#### Panel coexistence

- The panel takes interrupts locked for a burst's length: 50 bursts of 50 ms beside the waterfall
  app left no underrun, and the stream it shows went on after each.
- Its frame counter (an interrupt) misses the VSYNCs meanwhile, about 1.9 of the 2.9 a burst spans.
- The DMA keeps scanning out the picture (MicroPython, `tmp/s31/tx/mp9.py` in the workspace).

## Registers per chip

| Chip | IQ estimator | TX DC value register |
|---|---|---|
| ESP32-C6 | on [ESP32-C6 registers](../dev/registers-esp32c6.md) | on [ESP32-C6 registers](../dev/registers-esp32c6.md) |
| ESP32-C5 | the same as the C6 (0x24 lower) | `0x600a0c08`; bit 26 enables the injection |
| ESP32-S31 | none: transmit only | `0x20100c08` (I 9:0, Q 19:10) |

<!-- nav-start -->
Previous: [Transmit](transmit.md) · Up: [Samples in and out](index.md) · Next: [Host transports](transports.md)
<!-- nav-end -->
