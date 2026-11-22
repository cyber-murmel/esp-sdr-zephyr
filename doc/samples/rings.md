# Gapless receive

<!-- nav-start -->
Previous: [Burst capture](bursts.md) · Up: [Samples in and out](index.md) · Next: [Transmit](transmit.md)
<!-- nav-end -->

A burst ([Burst capture](bursts.md)) leaves a gap between one capture and the next. Two pieces of hardware give a
stream without gaps: the dump engine in its circular mode, which a CPU turns between banks, and
the modem diagnostic bus looped into a GDMA ring.

Both are introduced in [Samples in and out](index.md#hardware-that-moves-samples). This page
describes the ring of each chip.

| Ring | Chip | Header | Kconfig | Rate into memory | Sample bits | CPU |
|---|---|---|---|---|---|---|
| Dump engine over three banks | ESP32-S3 | `esp_sdr_ring.h` | `CONFIG_ESP_SDR_RING` | 16 MS/s | 10+10 | CPU 1, interrupts masked but for short windows |
| Dump engine over two blocks | ESP32-C6 | `esp_sdr_gapless.h` | `CONFIG_ESP_SDR_GAPLESS` | 80 MS/s, or 40 | 10+10, sent as int8 | the only core, interrupts off |
| Dump engine over two TCM groups | ESP32-S31 | `esp_sdr_dumpring.h` | `CONFIG_ESP_SDR_DUMPRING` | 20 MS/s or lower | 10+10 | turns a group a unit; the other core waits during a turn |
| Diagnostic bus | ESP32-C5 | `esp_sdr_diag.h` | `CONFIG_ESP_SDR_DIAG` | 40 MS/s | 4+4 | GDMA collects it; the CPU decimates |
| Diagnostic bus | ESP32-S31 | `esp_sdr_diag16.h` | `CONFIG_ESP_SDR_DIAG16` | 40 MS/s and lower | 8+8 | GDMA collects it |

## ESP32-S3 ring

### Circular capture

- With `DUMP_CTRL` 0x24000 the engine writes a 16384-pair ring continuously and exposes its write
  index at 0x60033d60.
- Switching the bank select while it runs splits the stream into gapless units.
- `lib/esp_sdr/src/esp_sdr_ring.c` (from upstream) rotates through banks 0 to 2 and proves
  continuity with sentinel windows.
- The switch must come within 2000 pairs (125 us at 16 MS/s) of its threshold.
- A bank must be filtered before the engine comes round to it again.

### Three banks and the decimating FIR

The dump engine runs in circular mode through three SRAM banks at 16 MS/s, and a two-stage
decimating FIR on the vector unit filters every bank (`esp_sdr_ring_run()`, ported from upstream
esp-sdr). The output rates and item sizes a host gets are in
[osmosdr](../../apps/osmosdr/README.rst).

### Filter budget

At 16 MS/s the ring CPU has 15 cycles per pair (240 MHz). The filter spends them as follows at
decimation 64:

| Step | Cycles per pair |
|---|---|
| Unpack | 3.0 |
| fs/4 mix | 1.6 |
| Stage 1 | 4.5 |
| Stage 2 | 2.6 |
| The app's packing | about 1 |

- The multiply-accumulate chain on the single `accx` accumulator, not the instruction count,
  limits stage 1.
- What osmosdr measured (12.7 to 13.3 cycles) is in [osmosdr](../../apps/osmosdr/README.rst).

### Interrupts inside the ring

- The ring runs on CPU 1 under `arch_irq_lock()`: on SMP, `irq_lock()` takes a global lock and
  would stall the other CPU.
- Nothing may be pinned to that CPU meanwhile, and no flash writes may happen (code from flash
  would stall).
- The vector registers it uses are not part of any thread context.
- The ring masks interrupts on its CPU and opens short windows for the kernel timer and IPIs
  (`irq_window()`).

#### The Wi-Fi MAC on line 0

- **Problem.** CPU interrupt line 0 is the Wi-Fi MAC's (`ETS_WMAC_INUM`). It fires for every packet
  the Wi-Fi receiver decodes at the tuned frequency, hundreds of times a second on busy air, and its
  handler is far longer than a window.
- **Symptom.** The ring abandoned units until the run failed. The receive path then lost whole
  frames, in bursts that came and went with the neighbours' traffic and flipped with unrelated code
  changes.
- **Fix.** `esp_sdr_ring_run()` disables line 0 on its CPU for the run and serves the interrupt
  afterwards.
- **Check.** `ring irq windows` and `lines` in `wpan status` show how many windows ran and which
  lines were pending (0x40 is the tick timer; 0x41 would be the MAC). With the fix, abandoned units
  stay at 0 even with the C6 sending back to back.

#### Scheduler IPIs and esp_timer

- Without `CONFIG_IPI_OPTIMIZE`, Zephyr sends the other CPU an IPI for every thread it makes ready,
  even one pinned away from it.
- With the Wi-Fi driver, CPU 1's IPI happened to get line 0 and waited out each run with the MAC.
  Without the driver it got line 1.
- Then wpan's ring opened some 1700 windows a second (its PHY thread sleeps 100 us at a time: worst
  lateness 110 pairs instead of 7, a few lost pairs), Ubertooth's 32 (2026-11-15).
- `lib/esp_sdr/Kconfig` turns the option on for SMP builds with the ring, whose cooperative thread
  then draws none.
- The esp_timer interrupt lands on the CPU that starts the timer. Without the driver the first
  `esp_sdr_init()` did that (Ubertooth's main, on CPU 1: the PHY's PLL tracking opened a window a
  second), so lib/esp_sdr starts it at boot, on CPU 0.

### Decimation by four

The ring also delivers 4 MS/s, with the boxcar-4 setup of `esp_sdr_ring_run()` (`shift_fs4`),
built with `CONFIG_ESP_SDR_RING_BOX4`. It mixes by -4 MHz and sums 4 pairs on the vector unit, a
boxcar whose nulls fall on the bands 4 and 8 MHz off, which would otherwise fold onto the channel.
With the LO 4 MHz below the wanted channel, the channel appears at DC in the stream
([Tuning](../radio/tuning.md)).

#### Against every 4th pair

Taking every 4th pair instead is cheaper, but it folds the bands 4 MHz above and below the channel
onto it, with their noise, and puts a signal 4 MHz away on top of the wanted one. The comparison
was made in Ubertooth on 2026-11-12.

| | Every 4th pair | Boxcar of 4 |
|---|---|---|
| CPU 1, cycles a pair (budget 15) | 8.2 | 11.4 to 12.1 |
| Units of 1024 pairs (64 us) | | 90 % abandoned |
| Units of 2048 pairs (128 us) | | about one abandoned a second |
| Units of 3072 pairs (192 us, the default) | | none abandoned in a minute, at 11.7 cycles a pair |

- The boxcar had first been ruled out as too slow.
- Its passband is centred 4 MHz above the LO, so on 2026-11-12 Ubertooth's LO first moved from
  3 MHz above the channel to 5 MHz below it, the channel at +1 MHz; it now sits 4 MHz below, the
  channel at DC (above). Taking every 4th pair, the channel lands at +1 MHz from either side.
- The boxcar gained some 3 dB at the edge of sensitivity. The measurements are in
  [ubertooth internals](../apps/ubertooth-internals.md), the simulation in
  [GFSK and Bluetooth LE](../modem/gfsk.md), the 802.15.4 side in
  [IEEE 802.15.4](../modem/ieee802154.md).
- The -fs/4 rotation and the sums of 4 are one bin of a 4-point DFT
  ([ubertooth internals](../apps/ubertooth-internals.md)).

### Unit size and cost

Measured on 2026-10-12 on the XIAO S3 against the C6, for the 802.15.4 ACK:

- At 1024-pair units the ring keeps up: 8.5 cycles per pair, subsampling sink included (budget 15).
- The worst switch came 60 to 137 pairs late (limit 896); no unit was abandoned over minutes.
- This needed a faster subsampling path. With an empty sink it took 9.3 cycles per pair before
  (every store reloaded the run state, the build does not assume strict aliasing) and 3.05 after.
- Why the 802.15.4 driver keeps to 1024-pair units is in
  [IEEE 802.15.4](../modem/ieee802154.md#sending-acks).

### Raw mode

- `ESP_SDR_RING_DECIM_RAW` (`CONFIG_ESP_SDR_RING_RAW`) gives the sink the unpacked 16 MS/s stream,
  in chunks of up to 512 pairs with 64 pairs of history in front.
- It costs the unpack's 3 cycles per pair on the ring CPU, plus about 1 for the history.
- An image that runs the ring only raw leaves the decimating FIR out (`CONFIG_ESP_SDR_RING_FIR=n`,
  about 1 KiB of IRAM).
- The [Channelizer](../modem/channelizer.md) takes this input.

### Spare bank and bursts from a run

With `esp_sdr_ring_cfg::spare_bank` and `esp_sdr_ring_burst_*()` a run can transmit without
stopping for long. The 802.15.4 ACK is the example ([IEEE 802.15.4](../modem/ieee802154.md)):

- The ring runs units of 1024 pairs and rotates through banks 0 and 1 only, so bank 2 is the CPUs'
  for the ACK waveform for the whole run.
- Armed with the frame's end pair, the run builds the rest of the waveform between its polls and
  stops at the first unit boundary at or after the end.
- It hands the last unit to the PHY and switches the front end to TX without the settle wait.
- It plays bank 2 once when CPU 0 fires (or drops it on a cancel or at the limit), switches back to
  RX and restarts the capture.
- The pair index keeps counting real time over the gap, so the PHY resets there as at any gap.

`spare_bank_app` is the same split for an app: the ring rotates through banks 0 and 1 and leaves
bank 2 (64 KiB of internal RAM) to the app. [Transmit](transmit.md#turnaround) links here.

### Ring stage

- `esp_sdr_ring_stage.h`: the ring's sink writes SRAM bank 2 from IRAM, with `spare_bank_app` and
  units of 2048 pairs; 16240 samples, 8 ms at 2 MS/s.
- The reader of the stage places itself from its own position after a lap. A place found against
  the written count alone lost the index once the newest hole was 2^31 samples old, 18 minutes at
  2 MS/s.
- MicroPython's Pcap engine uses it ([pcap internals](../apps/pcap-internals.md)).

## ESP32-C6 gapless 80 MS/s

`ESDR_MODE_GAPLESS` (`include/esp_sdr/esp_sdr_gapless.h`, `CONFIG_ESP_SDR_GAPLESS`, after
upstream's ring and h0m3us3r's eSpDR) receives the whole 80 MHz without a gap. How to build and
start it is in [osmosdr_serial](../../apps/osmosdr_serial/README.rst).

The C6's CPU is a 160 MHz rv32imac (no SIMD, no FPU). At 80 MS/s it has 2 cycles a sample and
cannot touch every sample, so nothing on it can channelize the stream. The IQ estimator's windows
are its only view of the band, and they see power, not frequency.

### Units, markers and the PHY-only image

- The dump engine runs as a ring that never stops while the CPU switches it between SRAM blocks 1
  and 2 every 12288 samples (154 us).
- Marker words in the next block prove that each unit starts where the last one ended.
- The ring takes two blocks, so the image must fit block 0: the radio comes up without the Wi-Fi
  driver ([Radio without the Wi-Fi driver](index.md#radio-without-the-wi-fi-driver)).
- The other modes (paced receive and transmit, triggered snapshots, bursts) stay in the image.
- The run keeps the PHY in its 20 MHz mode with the widest low-pass, unless the host set a
  bandwidth ([Filters](../radio/filters.md)).
- `lib/esp_sdr/src/esp_sdr_gapless.c` runs with interrupts off. Everything on its path must be in
  IRAM (paced code, `esp_sdr_place.h`), since a flash cache miss of thousands of cycles made
  switches late.

### Pruning by the IQ estimator

- The IQ estimator's 10 us windows over the full band decide what is kept: windows past a threshold
  over the quiet baseline, with one before and one after.
- A unit is readable for about one unit only, so kept windows are copied raw into staging at once,
  while their block can still be read.
- They are packed to int8 later, with a shift from the window's power and DC (the DC left in and
  reported), into `ESDR_BLK_SEG` blocks of segments (`esdr_seg.h`), and sent over SDIO.
- Every segment keeps its index on the run's axis (80 MS/s, 40 with `ESDR_GAPLESS_F_HALF`), across
  the runs a request or a ring error restarts.

The CPU's costs for this were measured twice; the sources give the setup of only one:

| Activity | Undated measurement | WHY2025 badge, 2026-10-26, from the P4's MicroPython |
|---|---|---|
| Copying a kept sample into staging | 2.5 cycles | about 3.6 cycles |
| Packing to int8 | 9 to 13 cycles a sample | about 20, with its slice overheads |
| Window readings with their decisions | 300 to 450 cycles | estimator windows 18 % of the CPU |
| Markers into the next block | 2000 cycles a unit | 9 % of the CPU |

### What limits a burst

Measured on the badge on 2026-10-26, from the P4's MicroPython:

- The link never limited: 1.4 to 4.2 MB/s of its 11 at thresholds from 1.25 to 5 times the
  baseline.
- The C6's own work did. A unit (154 us) keeps only what the CPU can copy before its block is
  reused, about 4000 samples (the per-unit cap).
- So a burst arrives whole up to some 50 us, 100 us when it straddles a switch. The survey's
  records were at most 91 to 104 us long, their median 26 to 32 us.
- The staging ring (64 KiB of raw words, 205 us at 80 MS/s, as built then) did not limit a single
  burst. Today `CONFIG_ESP_SDR_GAPLESS_STAGE_KB` defaults to 48 KiB (154 us).
- So only short packets arrived whole: a Bluetooth LE packet of up to some 25 bytes on air, no
  802.15.4 frame (an ACK lasts 352 us).
- In the office, with Wi-Fi, the survey decoded no Bluetooth LE packet at all; its bursts were
  nearly all shorter than 64 us.

### Staging and whole bursts

On 2026-10-27 the question was how to raise the maximum burst length.

#### Spare staging

- The ring uses the lower 64 KiB of SRAM blocks 1 and 2. Their upper halves are two more staging
  stores: with the staging ring in block 3 that is 176 KiB in all at the default 48 KiB ring
  (45056 samples, 563 us), 192 KiB with a 64 KiB ring.
- Canary words guard both ends of each half; a half is in service only after both blocks came back
  intact.
- A kept window goes to block 3 first, else to the upper half of its own unit's block, which the
  engine has just left.
- A job there is packed only while the engine writes the other block.

#### Whole bursts or nothing

- Admission counts only the staging a window can reach (block 3 and the smaller half), so what is
  admitted is never abandoned for want of room.
- A burst starts only with an eighth of that free; else it is refused.
- One cut for want of room drops its rest until a quiet window: one piece per transmission instead
  of scraps.
- A band that never rests starts anew after as many samples as that staging holds, once an eighth
  is free again.

#### Chasing the engine

- Reading a block while the engine writes it, as upstream's C3 port does, is impossible on the C6.
- The CPU reads stale data from a block the engine owns: 0.7 % of the words matched, 55 % were its
  own marker words.
- Further levers, from a survey of the code and not built: a faster copy on the C6, its GDMA, or
  the dump engine's four-slot mode.

### Threshold and gain servos

- Two servos keep 5 to 10 % headroom (`ESDR_REQ_SET_GAPLESS`).
- The threshold rises and falls so that what is kept fills the link's budget short of it.
- With automatic gain, the receive gain steps so that the loudest windows' peaks stay short of full
  scale.

#### Kconfig limits

The threshold servo's limits are Kconfig options (2026-11-04):

- `CONFIG_ESP_SDR_GAPLESS_THR_LINK_PCT`: how full the link may get (95 %).
- `CONFIG_ESP_SDR_GAPLESS_THR_LOST_PCT`: the share of the kept samples the CPU may lose in a period
  before the threshold rises 2 dB (5 %).
- That share used to be none: a single lost sample raised the threshold, so the CPU's limit held it
  long before the link's.

At 5 % against none, four 20 s surveys each, taking turns:

| | Samples kept | Abandoned | Bluetooth LE packets |
|---|---|---|---|
| 80 MS/s, none | 21.3 M | 0.98 % | 0 |
| 80 MS/s, 5 % | 37.4 M | 1.11 % | 0 |
| 40 MS/s, none | 42.0 M | 0.14 % | 237 (185 to 285) |
| 40 MS/s, 5 % | 46.1 M | 0.17 % | 289 (254 to 319) |

The link still carries only 34 % and 42 % of its budget: it does not limit, and the 95 % is never
reached.

### Half rate

- With `ESDR_GAPLESS_F_HALF` (`PCAP_MODE_F_HALF`, the pcap host tool's `--survey-rate 40`) the dump
  engine runs at 40 MS/s, by its rate bit (DUMP_CTRL bit 15).
- A unit then lasts 307 us, the CPU has twice the time to copy a sample, and every index, count and
  window of the run is on the 40 MS/s axis.
- A unit's cap is twice the samples, each twice as long, so a burst stays whole some three times as
  long as at 80 MS/s. Records got some three times longer (median 70 us against 28).

The engine drops every other sample without a filter: a sender at +25 MHz showed at -15 MHz at its
full level. So the run sets the low-pass to 31 MHz ([Filters](../radio/filters.md)):

- +-15 MHz stays flat; 28 MHz and narrower settings cut into +-15 MHz too.
- What lies beyond 20 MHz out folds in: a few dB down near 20 MHz, 15 to 25 dB down from 25 MHz on.
- The survey's bin at +-20 MHz, where both edges land, is left out.

### Width sorting

The C6 can replace wide signals by short records in place of their samples. That is described in
[Burst detection](../modem/bursts.md#width-sorting-on-the-c6).

### Measured

- On the badge the ring ran 120 s (9.6 G samples) without an error.
- The CPU packs some 1000 samples a unit while kept windows come in, about the link's 11 MB/s.

## ESP32-S31 dump-engine ring

`esp_sdr_dumpring_*()` (`include/esp_sdr/esp_sdr_dumpring.h`, `CONFIG_ESP_SDR_DUMPRING`) is a ring
without the diagnostic bus. The VIEWE board's panel uses 4 of the bus ring's signals
(`LCD_DATA_OUT_PAD_OUT0..3` carry Q[9:6]), so there this is the only gapless receive.

### Turning TCM groups

- The dump engine's writer runs without a stop in its circular mode. It writes whichever TCM group
  the ownership register gives it, from the same index on; upstream's continuous spectra turn it
  the same way.
- The run gives the writer group 2 for units of 1536 words, and the capture bank's group 3 only
  while it copies a unit out of group 2 ([SRAM bank ownership](index.md#sram-bank-ownership)).
- Sentinel words in group 2 find each unit's ends. Group 3 needs none: its words of a turn run
  from where one unit ended to where the next one starts.
- A first version marked and checked group 3 as well, and every end matched over 10000 turns. The
  marks and scans cost the CPU near 4 cycles of the 16 a word it has at 20 MS/s.
- 1 s at 20 MS/s: 19999813 of the 20000754 words its time allows (the rest the unread last unit and
  the settling start), 10800 turns.
- A turn takes up to 21 us, a fifth of the time. The other core waits in its stall interrupt
  meanwhile (the ROM's data is in group 3): a busy thread there keeps 74 % of its speed.

### TCM load costs

A load from TCM stalls its use some 5 cycles at 320 MHz.

| Copying 2048 words | Cycles a word |
|---|---|
| Unrolled, the loads first | 2.4 |
| `memcpy` | 2.7 |
| Through a volatile pointer with a masked index | 7 |
| Storing alone | 3 |
| A loop that uses each word as it loads it | near 10 |

### Low rates

On the VIEWE board, MicroPython streams into a buffer through this ring
([micropython](../../apps/micropython/README.rst)).

#### Up to 312.5 kS/s

- The dump ring runs on CPU 1, the image built for both cores with only the radio worker there.
- Its decimator in `cmod/esdr_local.c` carries its state from one call to the next.
- It captures at 20 MS/s as the bursts do, with PIE copies in the ring's turns. The decimator takes
  12 of the 14.6 CPU cycles a word it has at 250 kS/s.
- 250 kS/s and below ran 30 to 60 s without a gap. 312.5 kS/s restarts some 1.3 times a second,
  a gap each.
- Meanwhile CPU 0 waits about a tenth of the time (the ring's turns), and the panel keeps 59
  frames a second without an underrun.

#### 2 MS/s

- At 2 MS/s the ring captures at 10 MS/s. The decimator takes every word into a third-order CIC of
  ratio 5, straight to the rate, with no half-band.
- The CIC is 2.6 dB down at 500 kHz; what lies 1.5 MHz off folds in 30 dB down.
- The analog filter alone keeps out what lies 10 MHz from the centre: some 12 dB on the LO's side
  (its narrowest setting), 28 dB on the other.
- So the LO goes to the side of the band's nearer edge: above the centre from 2441.75 MHz on, where
  Bluetooth LE's channel 39 and 802.15.4's 25 and 26 then face out of the band.
- The exception is a crystal harmonic (every 40 MHz) that would fold in on that side. The
  decimator takes the mirrored band with I and Q swapped, at no cost.
- On the VIEWE board, Bluetooth LE 39 then saw 107 wideband bursts in 15 s against 2629 on 37.

#### PSRAM stalls

- Into a buffer the 2 MS/s stream ran 10 s without a gap while CPU 0 idled.
- CPU 0's PSRAM traffic stalls the decimator's stores into a PSRAM buffer: the run restarted 65
  times in 5 s with the Pcap detector reading it, and collapsed under PSRAM copies on CPU 0.
- So the Pcap engine has the decimator write the ring stage from IRAM, in the capture bank (TCM
  group 3) past the dump ring's words.
- Group 2 cannot hold it: during a run it is the engine's but for the turns (CPU reads give the
  engine's words, CPU writes are lost). Group 3 is the CPUs' but for the turns, which park the
  other core.

## ESP32-C5 diagnostic-bus ring

`ESP_SDR_DIAG` (`esp_sdr_diag.h`, `CONFIG_ESP_SDR_DIAG`) streams 40 MS/s without a gap. The method
is ESPsoup's (github.com/pit711/espsoup-firmware), itself after ESP-SDR's S31 streaming and
Twotoz/C5VRX. Every piece below was measured.

### Free-running engine

- The dump engine runs free: source 15, rate index 0, count `0x1ffff`.
- The counter is 17 bits, and counts above 65535 never reach DONE, so it never stops.
- It is started with RUN and a trigger pulse. Then the SRAM usage register is written back, so that
  the CPU owns block 1 again.
- The engine writes nothing into the block afterwards, but the diagnostic bus keeps carrying the
  dump word.

### Lanes through the GPIO matrix

- `MODEM_DIAG[9:0]` (signal index 106 + lane) is the Q field of the dump word and `[19:10]` the I
  field, as in the capture's words.
- Eight lanes, a window of 4 bits of each (`lsb` to `lsb + 3` and `10 + lsb` to `10 + lsb + 3`),
  go through the GPIO matrix to eight pads and straight back into `PARL_RX_DATA0..7` (signal
  index 48).
- The pads have output enabled by `esp_rom_gpio_connect_out_signal()` and input enabled. No wire is
  needed.
- The pads really toggle at up to 40 MHz, so nothing may be connected to GPIO 0, 1, 3, 4, 5, 7, 10
  and 23 (ESPsoup's pads). The board files here use none of them; the C5-Zero's schematic was not
  checked.

### PARLIO sampling

- PARLIO RX takes PLL_F240M / 6 = 40 MHz, 8 bits wide, rising edge, LSB first, with a soft
  delimiter and EOF at the ring length.
- It takes every second sample of the 80 MS/s bus, as a Q nibble (bits 3:0) and an I nibble
  (bits 7:4) per byte.
- The lane order was settled against the 10-bit capture: I + jQ gives a tone at +f, as the capture
  does.
- There is no filter before the sampling: narrow the low-pass to 20 MHz or less
  ([Filters](../radio/filters.md)).

### GDMA ring

- A GDMA RX channel (AHB, channel 0, trigger 9, owner check off) runs over a circular ring of 16
  descriptors of 3584 bytes (the length field holds 4095 at most).
- The ring sits in the lower half of block 1 (57344 bytes at 0x40820000).
- The hardware writes the received length back into every finished descriptor. The interrupt walks
  the ring from the last one seen and clears the length, so interrupts that merge lose nothing.
- The last 8 KiB of that half hold the descriptors and the consumer's buffers. The `osmosdr_serial`
  image has 144 bytes of RAM margin without the mode.

### Level and clipping

- The nibble window needs the level: a signal that exceeds `8 << lsb` counts wraps.
- At RX gain 60 on a hot link the front end clips and every window is useless.
- From index 48 up the C5 clips a tone that sits 10 dB below full scale at index 40.

### Ablation

With a tone from the C6 present:

- Only the lane word matters: `0x600a9018` = lanes 24 to 27 with bit 24 set. Without bit 24, or
  with other lane numbers, the bus carries no I/Q.
- The I/Q stays with RUN cleared after the start, with source 0 or 3 instead of 15, with either
  clock force (`0x600a9c04`, `0x600a0800` bit 2) cleared, or with `0x600a901c` zeroed.
- The selector pair `0x600a9404`/`0x600a9408` reads 0 on the working path.

### Measured

- 40 MB/s for 40 s, 0 skipped blocks, +0.15 ppm against the system timer. A byte lost per ring
  wrap would show as -17 ppm.
- box16 (a boxcar of 16, SWAR on four words) and a third-order CIC cost 4.1 to 4.4 CPU cycles per
  byte at 240 MHz (budget 6.0); with the USB service of `osmosdr_serial`, 5.7.
- `osmosdr_serial`'s 250 kS/s stream ran 60 s with no index jump.
- `tests/regression/stream_gapless` runs both receive paths against a C6 transmitter.
- How the USB-Serial-JTAG FIFO must be served to carry the stream is in
  [Host transports](transports.md#usb-serial-jtag).

## ESP32-S31 diagnostic-bus ring

`esp_sdr_diag16_*()` (`esp_sdr_diag16.h`, `CONFIG_ESP_SDR_DIAG16`):

- The modem's diagnostic bus carries I[9:2] and Q[9:2], looped through 16 pads into PARLIO, into
  an AXI GDMA ring in TCM.
- 60 s at each of 40, 20, 10 and 5 MS/s: no skips, no gaps, 0 ppm against the system timer.
- Lower rates are every n-th sample, so 10 and 5 MS/s alias under a 13 MHz floor of the low-pass
  ([Filters](../radio/filters.md)).

### diag16 into USB blocks

- The ring's nodes leave a 16-byte hole for a block header, with burst 16.
- A frame ends mid-ring: its length field holds 65535 bytes, less than the ring, and nothing is
  lost there.
- In [osmosdr_hs](../../apps/osmosdr_hs/README.rst) the nodes are blocks of the wire format with
  the header left out. The app writes the header into the hole in front of the samples and copies
  eight blocks at a time into the memory USB reads.

### Pads

- The pads are GPIO 20 to 25, 36 to 40, 42 to 44, 47 and 48.
- Upstream's GPIO 33 and 34 instead of 47 and 48 are USB-Serial-JTAG's, so that port would go away
  while streaming.
- Not 35, 45, 46 or 49: lines on them come out mixed with others, so Q's rms reaches 2.5 times I's
  whatever the phase.

### Sampling phase

Upstream does not handle this.

#### Problem

- PARLIO's clock comes from 160 MHz and the bus runs at 80 MHz, so about one start in two samples
  the bus on its edge, at every rate and on either clock edge.
- Some lines then take the next sample's bit, Q's bits 5 and 3 above all. Under signal Q's rms
  reaches 2.5 to 12 times I's, and a value crossing zero comes out around +-34 and +-42.
- Restarting the divider nearly always lands on the same phase. One period of the divider plus one
  comes 6.25 ns late, half a bus period, which moves it about every other time.

#### Detection

- `esp_sdr_diag16_run()` looks first with the receiver off (`0x20104ddc` bit 4 clear), where the
  ADC gives its noise alone, under a step rms, whatever is on the air.
- Only Q's zero crossings show the fault, about 85 % of them. At LO 2403, where the DC is near
  zero, 8160 pairs on a wrong phase show 1235 to 3289 values of 32 and up, the right one none, at
  every gain tried.
- The DC depends on the LO, though (-2.0 on I and +0.85 on Q at 2437), and a look with Q on one
  side of zero proves nothing.
- So it then looks with the receiver on, at gains where Q crosses, in moments with nothing on the
  air: a Wi-Fi frame hides the wrong values.

#### Decision rule

- 16 crossings with at most 2 large values decide.
- I's crossings do not count.

#### Results

- Over 15 LOs from 2300 to 2800 MHz the check took 5 of 16 starts. 4 passed; one ended in
  `stats.phase_bad` (`-EIO` with `cfg.phase_required`) and passed on osmosdr_hs's restart.
- On its own, as it was at first, the look on air gave up on 3 starts in 24 on a busy channel.

### VIEWE board

- GPIO 41 is not bonded.
- The library's default pads are the RS485 transceiver, the touch controller and the ADC keys here.
  With GPIO 47 and 48 (the amplifier's enable, the codec's MCLK) Q's top two bits come out late,
  1.6 % of them wrong, and no clean phase was found.
- GPIO 2 to 17, the panel's data lines, are clean: `esp_sdr_diag16_api` 11 of 11, 20 MS/s over USB
  without a gap, which leaves the panel dark.

#### The ring or the panel

- Every run resets the whole AXI GDMA group. That takes the panel's channel off the LCD, leaves the
  ring's pads undriven and, on these pads, drives the panel's data lines.
- The panel then gets no pixels until a reboot, so `esp_sdr_diag16.c` refuses to build with the
  panel driver.
- Lines 12 to 15 of the ring would also leave on `LCD_DATA_OUT_PAD_OUT0..3`, the panel's blue bits.
  Not tried.

<!-- nav-start -->
Previous: [Burst capture](bursts.md) · Up: [Samples in and out](index.md) · Next: [Transmit](transmit.md)
<!-- nav-end -->
