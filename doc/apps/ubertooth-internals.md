# ubertooth internals

The design and measurement log of [ubertooth](../../apps/ubertooth/README.rst), which runs the
device side of an [Ubertooth One](https://github.com/greatscottgadgets/ubertooth) on the ESP32-S3's
software radio, so the unmodified upstream host tools work.

The README is the user's view, with the modes' status and the hardware limits that shape the app.
This page follows it in [Applications](index.md).

The signal processing is told in the guides:

- the GFSK demodulator, its cycle budget and its sensitivity in
  [GFSK and Bluetooth LE](../modem/gfsk.md);
- the ring's decimation by four in [Gapless receive](../samples/rings.md);
- the retune time in [Tuning](../radio/tuning.md);
- the gain steps and the clip guard in [Gain](../radio/gain.md);
- the noise floor and DC spur against gain, and the ADC's clipping products at DC, in
  [Impairments](../radio/impairments.md).

## What replaces the Ubertooth One

| Ubertooth One | Here |
| --- | --- |
| LPC1758 firmware, USB bulk IN 0x82 with 64 byte packets, vendor requests on EP0 | `src/ubt_usb.c`, `src/ubt_ctrl.c`, `src/ubt_queue.c` |
| CC2400 radio: GFSK demodulator with bit clock, carrier sense, RSSI, sync word detector | software on the 4 MS/s ring stream: `gfsk_demod`, `ubt_rssi`, `ubt_le` |
| CC2400 transmit (GFSK modulator, packet engine) | `gfsk_mod`, `ubt_tx` on the DAC |
| Spectrum analyser: retune per MHz, read RSSI | wideband burst captures and an FFT: `ubt_specan` |
| Timer0: CLKN 3.2 kHz, CLK100NS | one timeline from the CPU cycle counter: `ubt_clock` |
| LPC IAP serial number, part number | the chip's MAC in the serial, a fixed part id |

- The USB protocol is `src/upstream/ubertooth_interface.h` (unchanged, GPL-2.0-or-later).
- The host tools check `bcdDevice`, open interface 0 and use bulk IN 0x82. The API version and the
  USB IDs they accept are in [ubertooth](../../apps/ubertooth/README.rst).

## Signal path

- The app takes the ring's 16 MS/s as 4 MS/s through the boxcar-4 setup of `esp_sdr_ring_run()`
  (`shift_fs4`), which [Gapless receive](../samples/rings.md) describes.
- The LO sits 4 MHz below the wanted channel ([Tuning](../radio/tuning.md)). So the channel appears
  at DC in the stream, and the LO leakage, mixed to -4 MHz, on one of the boxcar's nulls.
- How the demodulator reads the 8-bit samples, and where its own sums put their nulls, is in
  [GFSK and Bluetooth LE](../modem/gfsk.md).

### Earlier placements

- Until 2026-11-12 the channel sat at +1 MHz: the LO 5 MHz below, the demodulator's sum after a
  derotation by 1 MHz. There the bands 3 and 5 MHz off folded in past the nulls.
- Before the sums, the ring took every 4th pair, with the LO 3 MHz above.

## The sample ring

- The ring's sink (CPU 1) keeps the stream in a ring of 8192 samples (2 ms) in internal RAM, 8 bits
  each.
- A sample is the int16 stream >> 6: the ADC's 10 bits without the lowest two, far below the noise
  at gain 60.
- The mode thread (CPU 0) reads it in chunks of 256 through `src/ubt_rx_sess.c`, the session the
  demodulating modes share: tune, demodulate, gaps and ring restarts.
- The session keeps one 64 bit timeline of the samples (`ubt_clock`) for le_conn, the promiscuous
  mode and the packets' time stamps.
- A ring in PSRAM was tried. Its stores cost the masked ring CPU some 13 cycles a pair (the ring
  went from 5 to 18.6 cycles a pair, over its 15), and it abandoned most units.

### Holes and laps

Measured 2026-11-12, afternoon.

#### A queue of holes

- The ring's sink records each hole in the stream (the start of a run, an abandoned unit) before it
  publishes the samples after it. The reader delivers what precedes a hole, then skips it.
- There was one record. A second hole before the reader reached the first replaced it, and the
  first hole's slots went out as samples: the stream of one lap before, at the start of a run the
  previous channel's.
- The records are a queue of 8 now (`src/ubt_sring.h`, a header so that the hot reader inlines its
  common path). A reader more holes behind than that drops the stream up to the writer.
- `ubt rx stats` counts since boot the holes skipped with a later one recorded already (where the
  single record failed) and the times records were lost. In the runs below both stayed at 0.

#### The model test

- A unit test (`tests/unit/ubertooth_core/src/sring.c`) runs the bookkeeping against a model ring
  whose slots remember the index written to them, the sink and the reader interleaved at random
  over 400000 steps.
- It found an older flaw. The sink writes a call's samples (up to 80) before it publishes them;
  after a hole far beyond the published position, a copy of the oldest slots taken meanwhile passed
  the check against that position.
- The sink now publishes first how far it is about to write (`wmax`), and the reader checks against
  that.
- Each fix has a test that fails without it: with the single record a sample of the lap before is
  taken; checked against the published position, sample 36 holds 100.

#### Publishing without atomics

- The sink publishes with plain stores and the barriers they need. `atomic_set()` is a compare and
  swap loop through `xtensa_cas()`, the PSRAM atomics emulation, some 30 cycles a call.
- The ring costs 11.82 cycles a pair against 11.85 before (12.05 with a second `atomic_set()` for
  `wmax`).
- Both images abandoned 119 units in each 2 minute run, one a second, where the night before showed
  none in a minute: not this change, and not explained yet.
- They were gone with [the sink's 8-bit pack](#the-sinks-8-bit-pack) that evening.
  [Long runs](#long-runs) (night) gives units of 3072 as what ended the abandoned units.

#### The lap check

- The copy of `ubt_radio_rx_read8()` reaches 80 to 95 samples of history back from the read
  position, and the old lap check left that out.
- A reader that crept into the last 95 samples before a lap had its copy dropped and skipped to the
  writer, some 8100 samples, where a lap keeps three quarters of the ring and loses some 2000.
- By its samples a lap, about a quarter of the committed image's laps were such drops. The check
  now leaves room for the history.

#### Layout sensitivity

Sniffing channel 37 for 2 minutes at a time, alternating with the committed image (A):

- The new reader first lost six to seven times as much of the stream, with three to ten times the
  laps, whichever way a lap went (B, C).
- The cause was the hole handling out of line in flash: 200 B that moved the flash code after it,
  the USB stack among it, and with it what CPU 0's 16 KB instruction cache holds.
- Inline in the reader's IRAM, with the flash code where the committed image has it, the laps were
  A's again (D).

#### Laps by image

Laps here are the gaps less the ring's 119 abandoned units:

| Image | Runs | Stream lost | Laps a run | Samples a lap |
| --- | --- | --- | --- | --- |
| A, committed | 12 | 0.000 to 0.069 %, mean 0.024 | 1 to 83, mean 30 | 3900 |
| B, hole handling in flash, a lap keeps 3/4 of the ring | 4 | 0.13 to 0.36 %, mean 0.22 | 242 to 650, mean 408 | 2600 |
| C, in flash, a lap skips to the writer | 3 | 0.13 to 0.27 %, mean 0.20 | 74 to 156, mean 112 | 8400 |
| D, inline, skips to the writer | 3 | 0.002 to 0.093 %, mean 0.032 | 2 to 55, mean 20 | 8100 |
| E, inline, a lap keeps 3/4 (a quiet hour: 16 to 18 % open) | 3 | 0.000 to 0.002 %, mean 0.001 | 2 to 7, mean 5 | |

- With the layout as before, a lap ends a short stretch of load. In the one pair with laps (D 55,
  A 56) skipping to the writer lost 8100 samples a lap against A's 3200, so a lap keeps three
  quarters of the ring again (E).
- E's runs fell into a quiet hour and lost as little as A's between them.
- The queue and the hole handling take 264 B below the banks (1224 B free).
- The instruction cache is a lever of its own: 200 B of flash code moved made CPU 0 lap several
  times as often ([CPU 0 and the instruction cache](#cpu-0-and-the-instruction-cache)).

### The sink's 8-bit pack

Measured 2026-11-12, evening.

- The sink converted the ring's int16 output to the sample ring's 8 bits in a scalar loop: 15
  instructions a sample, with 4 pairs a sample some 3.75 of CPU 1's 11.8 cycles a pair.
- On the vector unit (`src/ubt_pack8.S`): unaligned loads of the source, >> 6 as a multiply by 1
  with SAR 6, the clamp at 127, the low bytes by `ee.vunzip.8`. The head up to the ring slot's 16
  byte boundary and the tail are scalar.
- The ring then costs 8.7 to 8.8 cycles a pair.
- The units it abandoned once a second (119 in each 2 minute sniff, the committed image too) are
  gone: 0, 0 and 1 in three sniffs alternating with the committed image, which abandoned 119 in
  each.
- A check at boot (`src/ubt_pack.c`) runs the kernel against C over 1600 cases: every alignment of
  the ring slot and of the source, lengths 0 to 99, the int16 range and the stream's. The sink
  falls back to C if one differs.

#### The flash layout kept

- None of it is flash code: the check runs from IRAM, and `ubt_radio.c`'s `pack()` keeps its C
  loop, starting where the vector unit stopped.
- The image's flash code is the committed one's byte for byte (every flash function's address and
  size), since code added in flash moved the flash code after it
  ([Holes and laps](#holes-and-laps)).
- Even with the instructions unchanged it moved 4 bytes: without `pack()`'s loop, GCC's debug
  scopes of the code inlined into `ring_thread_fn()` came out otherwise, and the assembler aligned a
  branch target in it differently.
- The price is IRAM: 504 B are free below the banks.

#### CPU 1's remaining budget

- The -fs/4 rotation and the sums of 4 are one bin of a 4 point DFT
  (I = (i0 - i2) + (q1 - q3), Q = (q0 - q2) - (i1 - i3)).
- They are done in two passes of the ring library (`rot_fs4`, then `sum4`), whose fusion would save
  some 1.5 cycles a pair more.
- The headroom could take a longer decimation filter, for part of the front end's 1 dB
  ([GFSK and Bluetooth LE](../modem/gfsk.md)).

## CPU 0 and the instruction cache

CPU 0 runs the mode thread, the demodulator and USB. How much of the stream it loses depends on
USB's share of it and on what its 16 KB instruction cache holds.

### USB polls

Measured 2026-11-11, evening.

- The bigger cost was outside the demodulator. On the air each of its parts took about 1.5 times its
  bench time, as the cycle counts include whatever preempts the mode thread.
- Thread run times (`CONFIG_THREAD_RUNTIME_STATS`) over a 20 s sniff: the mode thread 12.6 s,
  `usbd` 3.7 s, `usb_otg` 2.2 s, about 30 % of CPU 0 in USB.
- The host polls with `UBERTOOTH_POLL`, a control transfer, about 975 times a second (19510 polls,
  1606 of them with a packet).
- That is some 270 us of CPU 0 a poll, against 45 us with CPU 0 otherwise idle.

#### Cache eviction

- A profiler showed why: then the app's `ubt prof`, in which the ring sink on CPU 1 recorded CPU 0's
  program counter and thread once a sink call. The library's `CONFIG_ESP_SDR_CPU_PROF` and `prof`
  replace it ([Profiles of CPU 0](#profiles-of-cpu-0)).
- The instruction cache is 16 KB. A poll runs some 15 KB of the USB stack from flash (79 functions:
  the DWC2 driver's interrupt handler and thread, usbd's control transfers, the buffer pool's heap).
- The mode loop ran another 7 KB of flash code per chunk, so each evicted the other.

#### Two changes

| Change | USB threads, share of CPU 0 |
| --- | --- |
| Before | 26 % |
| The mode loop's per chunk code in IRAM (the reader, the session's poll, the demodulator's entry, the finder's feed and gap search: 4.3 KB) | 18 % |
| An empty POLL in the BLE modes waits up to 4 ms for a packet (`ubt pollhold`), so the polls come at the pace of the packets | 5 % |
| ... up to 16 ms (2026-11-12; the same packets in interleaved runs, none dropped) | 4 % (7 against 4 % in a profile of its own) |

#### The poll hold

- The wait relies on the host controller retrying the NAKed data stage by itself, without an
  interrupt on the device (the DWC2 driver does not enable the IN token interrupts).
- It changes nothing the tools see: a packet goes out as soon as it is queued, a burst of them back
  to back.

On the air (one image, the wait off and on alternating, three rounds of 20 s on channels 37 and
38):

| Wait | Samples lost | Packets with a good CRC (mean of 6) | Demodulator, open / closed |
| --- | --- | --- | --- |
| off | 19 to 25 %, mean 22 | 1525 | 90 to 92 / 15 cycles a sample |
| 4 ms | 14 to 17 %, mean 16 | 1638 | 75 / 12 to 13 |

#### The kernel's atomics

- What is left of the USB cost is mostly the kernel's: every atomic operation is a call
  (`xtensa_cas`, out of line since atomics in PSRAM are emulated), some 4 % of CPU 0.
- Inlining its fast path would take several KB of IRAM.

#### Long runs

Measured 2026-11-12, night.

- Over 5 minutes of sniffing on channel 37 (some 20 % of the samples open) 0.23 % of the stream was
  lost in 953 gaps, two thirds of them found with the squelch open.
- In long stretches of open squelch (Wi-Fi keeps it open too) the full path, the finder and USB
  together come to a little over the 60 cycles, and the 2 ms ring laps.
- Two changes halved it (0.10 %, 599 gaps). The slicer's assembly now also runs before a burst is
  acquired, up to where the C acquires it: the C slicer had cost some 2.6 cycles a sample of the
  open time, Wi-Fi and noise moving the burst edge again and again.
- The other change: an empty POLL waits up to 16 ms instead of 4 (USB from 7 to 4 % of CPU 0).
- The rest of the gaps (about one a second) were the ring's abandoned units of 2048 pairs, gone in
  units of 3072.

### Layout and stream loss

#### CPU 0 on a busy channel

Measured 2026-11-12, night, after the channel moved to DC:

- Following, every run with the channel at DC lost more of the stream than every run at +1 MHz, 3
  to 12 times as much (at most 0.06 %).
- Sniffing channel 37 at gain 40 for 60 s lost 0.39, 0.20 and 0.12 % against 0.00, 0.00 and
  0.17 %, with the squelch open 12, 9 and 8 % of the time against 4, 5 and 8 %.
- The demodulator's mean load is the same (15 to 16 cycles a sample at gain 40).
- First suspect: more bursts open the squelch. On channel 37 it decoded 5815, 5210 and 4561 packets
  against 4911, 4574 and 4871.
- Second suspect: the flash code moved, most of it 148 B down from 6f80572's (`ubt rx ring` and its
  literals, `ring_thread_fn()` without the every-4th branch, the int16 reader without its inlined
  `unpack()`). [Holes and laps](#holes-and-laps) found that layout to decide such losses.
- It was the layout ([The padded layout](#the-padded-layout)).

#### The padded layout

Measured 2026-11-13.

- To tell the layout from the code, 477b629 was padded back to 6f80572's flash layout (not kept):
  every flash function but the changed ones and every constant at 6f80572's address.
- The padding: `zp_fill_c()` in IRAM and its slot taken by `acquire()`, which it follows, with the
  literal the linker had shared between them; `cmd_rx_ring()` grown to `cmd_rx_sum()`'s size; a
  help text 7 characters shorter for the string pool.
- Following at gain 40, the three images rotating over three rounds, twice. The second time the
  cache's counters were read every 5 s, which costs CPU 0 some flash code of its own.

| Stream lost | 6f80572 | 477b629 | 477b629 on 6f80572's layout |
| --- | --- | --- | --- |
| Three rounds | 0.0023 % | 0.045 % | 0.0087 % |
| Three rounds, counters read | 0.013 % | 0.085 % | 0.0055 % |

- The same code lost 5 to 15 times less on the other layout.
- The demodulator's mean cost followed the layout too (14 to 16 cycles a sample against 12 to 13):
  its loop runs from IRAM, code it calls from flash.

#### The cache's counters

- `EXTMEM_IBUS_ACS_CNT` and `EXTMEM_IBUS_ACS_MISS_CNT` at 0x600C40CC and 0x600C40C8, their window
  at 0x600C40B4 and 0x600C40B8 set to the flash text, read with `devmem`.
- They read alike most of the time, some 8000 misses a second, but the moved layout once showed
  6.4 million in one second: the stream goes in bursts, when CPU 0's flash code thrashes.

#### Profiles of CPU 0

- `ubt prof` took 131072 samples each of following a connection, of scanning and connecting, and of
  sniffing channel 37.
- At one sample a sink call (up to 80 samples), a capture spanned about 2 s, not the 8.4 s first
  written here.
- CPU 0 was idle 43 to 66 % of the time, the mode thread 28 to 55 %, the USB threads 1.5 %.
- From flash it ran 84 functions, 21 KB, more than the cache's 16 KB: the USB device stack (the DWC2
  driver 5.7 KB, usbd's control path 2.7 KB, net_buf and sys_heap 1.9 KB), the mode loop `le_run()`
  (1.9 KB, the most samples), `acquire()`, `find_gap()`, the CRC table walk, `ubt_ctrl()`.
- The library's sampler that replaced `ubt prof` checks a fixed period in the ring's poll loop and
  before every sink call instead.
- Sniffing channel 37 it took 131072 samples at 64 us over 8.8 s, none missed, 3 gaps over 96 us
  (the longest 112 us).
- Its shares agree with the old captures' (`closed_scan()` 8 %, `le_run()` 1 %, the vector kernels
  alike), so the bursts did not bias them.

### The hot block

So that code has a place of its own now: a block of flash text at a fixed position in the cache's
ways. How the block works is in [Code placement](../dev/code-placement.md).

- The ESP32-S3 linker script takes TEXT_SECTIONS snippets ahead of its catch-all (a line in the
  Zephyr fork), as it takes RAMFUNC_SECTION ones in the IRAM text.
- `apps/ubertooth/hot_text_s3.ld` claims the profiles' functions behind `ALIGN(2048)`, the cache's
  way, densest first (samples per byte); the linker pools their literals at the block's start.
- Its lines keep their sets whatever comes before or after: built with and without an unrelated
  change (the clip guard's, 144 B of flash code), the block's functions kept their addresses and
  sizes.
- The block is lib/esp_sdr's since (`CONFIG_ESP_SDR_HOT_TEXT`, `esp_sdr_hot_text()`), and
  `hot_text_s3.ld` holds the list, which `scripts/place/hot_text.py` makes from `prof dump`
  captures.
- A function renamed falls back to the catch-all, and the link stops if the block outgrows its
  budget (`CONFIG_ESP_SDR_HOT_TEXT_BUDGET`, 12 KB).

#### Block size

Following at gain 40, three rounds each, the block in the working tree (with the clip guard's change
and the pack check's) and in 477b629 alone:

| Stream lost | Round 1 | Round 2 | Round 3 | All |
| --- | --- | --- | --- | --- |
| 6f80572 | 0.017 % | 0.0004 % | 0.0043 % | 0.0076 % |
| 477b629 | 0.054 % | 0.061 % | 0.071 % | 0.062 % |
| Block of 16 KB (69 functions) | 0.0004 % | 0.0009 % | 0.0005 % | 0.0006 % |
| The same in 477b629 | 0.0043 % | 0.0078 % | 0.0031 % | 0.0052 % |
| Block of 12 KB (66 functions) | 0.0005 % | 0.0004 % | 0.0011 % | 0.0007 % |
| The same in 477b629 | 0.0049 % | 0.0004 % | 0.0016 % | 0.0025 % |

- The block loses a tenth to a hundredth of 477b629's loss, and less than 6f80572's.
- At 16 KB the block filled nearly all 8 ways of every set, and the code outside it still made a
  factor of 9.
- At 12 KB (6 ways, 96 % of the flash samples) it made a factor of 3.5, so the block stays at
  12 KB.
- Flash code elsewhere no longer moves CPU 0's hot code, so the pack check at boot runs from flash
  now (`ubt_pack.c`, its fallback to C kept): 928 B free below the banks.

## Memory

### Freeing internal RAM

- `CONFIG_ESP_SDR_RING_FIR=n` (the ring's FIR stage, unused here) and the BLE finder's search loops
  back in flash freed 3.4 KB below the ring banks and 3.5 KB of the high region.
- The mode loop's code and the vector and slicer kernels took most of it again.
- 1488 B are free below the banks, 2.8 KB in the high region (the planes take 2.6 KB).
- A 32 KB instruction cache would need another 16 KB below the banks; the system heap is in use,
  15.5 of its 17 KB at the most.

### Without the Wi-Fi driver

`CONFIG_ESP_SDR_PHY_ONLY`, since 2026-11-13:

- The DRAM image ends 30 KB lower (31000 B free below the banks against 928), and the flash code is
  267 KB smaller.
- The system heap peaks at 2212 bytes, so it shrank to 4 KB.
- The noise floor (`ubt rx floor`, -47 dBFS per bin at gain 60, -65 at 40) and the demodulator's
  cycles (`ubt rx bench`, 53 a sample) came out the same.

Following at gain 40, three rounds each, alternating:

| | Without the driver | With it |
| --- | --- | --- |
| Stream lost per round | 0.0007, 0.0040 and 0.0008 % | 0.0046, 0.0012 and 0.0006 % |
| Stream lost over all | 0.0020 % | 0.0021 % |
| CONNECT_REQs caught | 13 of 15 | 14 of 15 |
| Events missed | 13.7 % | 18.4 % |

### The hot list remade

- The hot block's list stays the one made from the profiles with the driver.
- It was made again from three fresh profiles of the image without the driver
  (`scripts/place/hot_text.py`; CPU 0 idle 69 % of the time, flash code 1.7 % of its samples, 56
  functions in 12167 B).
- That list lost more in each of three alternating rounds: 0.0068, 0.0076 and 0.0019 % (9 to 27
  gaps) against 0.0010, 0.0009 and 0.0011 % (5 to 7 gaps).
- The new list traded the USB driver's functions for the heap's. The losses come in bursts, when
  the driver's code runs, and a profile of the average time underrates it.

### A 32 KB instruction cache

The 32 KB cache has 8 ways of 4 KB and takes 16 KB more below the banks, where 27944 B stay free.
Three alternating rounds each:

| Cache | Stream lost per round | Over all |
| --- | --- | --- |
| 16 KB with the block | 0.0030, 0 and 0.0003 % | 0.0010 %, as in the six rounds before |
| 32 KB with the block (aligned to the 4 KB way by itself) | 0.0005, 0.0025 and 0.0005 % | 0.0011 % |
| 32 KB without it | 0.0005, 0 and 0.0062 % | 0.0021 % |

- The losses come as rare bursts, a run with one of them reading ten times a run without, so three
  rounds tell a factor of two at best.
- The block still pays at 32 KB, the bigger cache alone does not, and the 16 KB stay free.

## Measurements

### Spectrum analyser and advertiser, 2026-10-14

On a XIAO ESP32-S3 (A4CB8FD1C700). The retune times, ring start, noise floor and DC spur measured
on the same board and day are in [Tuning](../radio/tuning.md) and
[Impairments](../radio/impairments.md).

#### Spectrum analyser

- `ubertooth-specan -l 2402 -u 2480` against the board: the serial version (retune, capture, 32
  frames, FFT per placement), two placements, 2428 and 2455 MHz.
- 504 complete sweeps in 6 s, ascending 2402 to 2480 MHz, about 84 sweeps/s.
- About 11.9 ms per sweep: two retunes of 3.8 ms, two captures of 0.2 ms, two times 32 frames of FFT
  and the rest.
- Overlapping the FFT with the next retune would reach about 135 sweeps/s.
- The levels are relative: the full scale reference of the byte is a guess (`ubt specanfs`) until a
  source of known power is available.

#### Advertiser

- `ubertooth-btle -s22:44:66:88:aa:cc -A38`: ADV_IND, flags 05, every 100 ms on 2426 MHz, LO at
  2422 MHz, TX gain step 6.
- The PC's Bluetooth adapter (`bluetoothctl scan on`) lists "Device 22:44:66:88:AA:CC (public)"
  with AdvertisingFlags 05.
- So the whitening, CRC-24, GFSK modulator, DAC and RF all work as a BLE advertiser.
- TX_TEST (PRBS9 loop on 2441 MHz) starts and stops; its output was not observed.

### Modes with the upstream tools, 2026-11-11

On a XIAO ESP32-S3 with panel (A4CB8FD1D880), in an office. The tools are the nixpkgs Ubertooth
host tools with the project USB ID (bench recipe in [ubertooth](../../apps/ubertooth/README.rst)).
Every mode logs on the shell's port when it ends, among other things the samples lost and the
demodulator's cycles per sample.

| Mode | Run | Result |
| --- | --- | --- |
| `ubertooth-btle -n -A38` | 15 s, twice | 1662 and 1845 packets (3 and 9 % bad CRC), 7 and 17 % of the stream lost |
| `ubertooth-btle -p` | 60 s on 2440 MHz | access address af9aa4d7 of a connection on the air, its CRCInit 3a6859; the interval stage did not settle (the channel was visited 890 to 1020 ms apart: not all 37 channels with selection #1) |
| `ubertooth-btle -f -A37` | 90 s | listened; no connection came along (two Apple advertisements with a damaged type field read as CONNECT_REQ by the host, rightly not followed: bad CRC) |
| `ubertooth-rx -z` | 22 s, sweep, the PC's adapter in inquiry | the GIAC LAP 9e8b33 hundreds of times on the inquiry channels, err 0 to 2, and a piconet in the office (LAP 015f99, UAP 68 recovered); 547 hops (25 Hz) |
| `ubertooth-afh -l 015f99 -u 68` | 25 s | the channel map built up and followed by the host |
| `ubertooth-specan -l 2402 -u 2480` | 6 s | 505 sweeps, 2402 to 2480 MHz ascending (the first packet stale) |
| `ubertooth-util -q80` | | the three Wi-Fi channels read every sweep; with the placeholder calibration nothing reaches -80 dBm ([Levels](#levels)) |

The demodulator's cycles in these modes are in [GFSK and Bluetooth LE](../modem/gfsk.md).

### Boxcar against every 4th pair, 2026-11-12

The ring's two decimations on the board, the night the boxcar replaced every 4th pair (why and at
what CPU cost: [Gapless receive](../samples/rings.md)). `ubt rx sum off` restored the earlier ring
for the comparison.

| | Every 4th pair (LO +3 MHz) | Boxcar (LO -5 MHz, units of 2048; 3072 since) |
| --- | --- | --- |
| Noise floor, `ubt rx floor`, 2480 MHz, gain 60 | -63.6 / -64.4 dBFS a bin | -67.2 / -67.4 |
| The C6 at -15 dBm on channel 39, gain 27 (the edge), good advertisements in 10 s, four runs | 20, 13, 16, 9 | 73, 74, 62, 78 |
| Gain 30 | 104 to 116 | 108 |
| Busy channels 37 and 38, gain 60, good packets in 4 x 15 s | 9528 | 10658 |
| Following: events, missed; CONNECT_INDs caught | 737, 12.8 %; 15 of 24 | 764, 10.6 %; 16 of 24 |

- Some 3 dB at the edge of sensitivity: the knee moved by about 3 gain steps (the low steps are not
  quite 1 dB each).
- More packets on busy channels, following as before.

### Levels

On the same board, 2026-11-11:

- The stream at gain 60 reads -52 dBFS a bin (in-channel -44.5), about 13 dB above the October
  numbers of the board without a panel.
- The noise floor the modes set their squelch from follows the channel: pwr 100 to 136 on busy
  advertising channels.
- The spectrum analyser's placeholder full scale puts the noise at -120 dBm and nearby Wi-Fi at
  -94 dBm, some 30 dB below the stream's placeholder (`UBT_RSSI_K_DEFAULT`).
- Neither is checked against a known source.

### Following connections, 2026-11-11

In the afternoon, in a busier office. The bench:

- a XIAO ESP32-C6 as a connectable peripheral that advertises on channel 39 only, at -15 dBm
  (scratch app, Zephyr's controller, no pairing);
- the PC's adapter (Intel, Linux) connecting to it again and again with `bluetoothctl`;
- the S3 running `ubertooth-btle -f -A39`, or `-n` to compare.

The C6 pins every second connection to the 1M PHY; left alone, the PC moves each to 2M.

| What | Result |
| --- | --- |
| A 1M connection (45 ms interval, CSA #2, channel map 22 to 36) | followed from the CONNECT_IND to its end, 25 s, 480 packets, a packet in 59 % of the events; LL_CHANNEL_MAP_IND and LL_PHY_UPDATE_IND (no change) decoded on the way |
| A connection moved to 2M | followed until the LL_PHY_UPDATE_IND, then `PHY change` and back to advertising (when the update was missed: `timeout`) |
| CONNECT_INDs caught, gain 40 | sniffing 55 of 70 (79 %); following 28 of 97 before the fix ([Bugs found](#bugs-found)), 1 to 5 of 6 per minute after it, as sniffing then |
| Long (37 byte PDU) against short (24) advertisements | 335 against 365 a minute at gain 40, 274 against 305 at gain 60: length costs some 10 % |

The gain against level and the noise floor measured on this bench are in [Gain](../radio/gain.md).

#### The modes' retune

Measured 2026-11-12, night:

- A retune of the modes (tune request to the stream's first sample) took 8.8 ms, of which the
  library's retune is 4.2: its 3 ms of settling and a calibration.
- The radio thread retuned twice: `esp_sdr_rx_set_gain()` retunes too, to apply the gain, which
  every retune applies already.
- The stop waited 1 ms for the ring before it asked it to stop.
- Now it sets the gain only when it changes and asks first: 5.8 to 6.0 ms (stop 0.3 to 0.4, retune
  4.2, the ring's start and the first unit the rest).
- The slowest of a follow's hops takes 5.1 ms against 10.
- The events a follow misses stayed at about 12 %, and it caught fewer CONNECT_INDs than sniffing.
  The cause was the squelch's gate after a retune, in [GFSK and Bluetooth LE](../modem/gfsk.md).

## Bugs found

All of these are fixed; the demodulator's fixes came with unit tests. The squelch floor that the
modes took from the tracker's mean is in [GFSK and Bluetooth LE](../modem/gfsk.md).

### Demodulator

#### Look-ahead after a gap

The demodulator sliced the look-ahead after a gap, up to the far side of the gap, out of stale ring
entries: some 250 made-up symbols a gap.

#### The noise floor tracker

- The tracker was seeded again by the first group after every gap. A Wi-Fi burst set the squelch
  up to 30 dB high; once, nothing passed.
- It could also wrap at full scale.

### Sample ring reader

#### Three reads of the writer's position

The reader read the writer's position three times when it dropped a damaged chunk. The gap it
reported and its new position disagreed, and the demodulator's time drifted.

#### A hole recorded in between

Found 2026-11-12, night:

- The reader read the writer's position before the record of a hole in the stream. A hole recorded
  in between put the reader ahead of the writer.
- The sink records one at the start of every run: its first unit begins a little after pair 0.
- The next read took that as a lap, a gap of some 2^32 samples. The stream's clock, and with it the
  follower's, was off by a thousand seconds, and once a follow never retuned again.
- It came with the hundreds of retunes of a follow. The follow runs on this page are after the fix.

### Threads and callbacks

#### Pinned too late

The mode thread was pinned after it had started, which fails silently: it could run on CPU 1.

#### The clock callback's context

The BLE finder's clock callback got the promiscuous mode's context: a division by zero.

#### le_conn's clock conversion

- In follow mode le_conn's clock conversion (two 64 bit divisions, some 800 cycles) ran on every
  poll, even without a packet or a connection.
- On the advertising channel that margin decided whether the ring overflowed during an
  advertisement and the CONNECT_IND behind it: samples lost 3.6 % sniffing, 11.7 % following;
  CONNECT_INDs 15 of 24 against 3 of 24, interleaved.
- It runs only when there is something for it now.

## Status and open points

- Every mode the radio can do runs on a board, connection following included (1M PHY, as an
  Ubertooth One).
- The ring's boxcar fits CPU 1's budget: 9 of 15 cycles a pair.
- Following catches as many CONNECT_INDs as sniffing (about 90 %) and misses about 1 % of a
  connection's events (the C6 close by).
- CPU 0's hot flash code sits in a block of its own at a fixed instruction cache position, and the
  stream it loses no longer follows the rest of the layout.
- The demodulator's place against its cycle budget and against the simulator's genie is in
  [GFSK and Bluetooth LE](../modem/gfsk.md).

### Not done

- A gain control for nearby transmitters that is measured working. The clip guard
  (`ubt rx gain guard`) holds longer for sources in bursts now; the channel at DC needs it more.
- The hot block's list from the BR and promiscuous modes.
- RSSI and spectrum level calibration with a source of known power.
- The demodulator's timing and estimation: [GFSK and Bluetooth LE](../modem/gfsk.md).
