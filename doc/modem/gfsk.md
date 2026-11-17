# GFSK and Bluetooth LE

<!-- nav-start -->
Previous: [IEEE 802.15.4](ieee802154.md) · Up: [Modulation and coding](index.md) · Next: [Burst detection](bursts.md)
<!-- nav-end -->

This page covers three of the tree's GFSK receivers in turn:

- the [Ubertooth demodulator](#ubertooth-demodulator), on the ESP32-S3 ring's 4 MS/s stream, the
  largest part of the page;
- [Bluetooth LE from channelizer captures](#bluetooth-le-from-channelizer-captures), on the host;
- the [MicroPython burst receiver](#micropython-burst-receiver).

This page follows [IEEE 802.15.4](ieee802154.md): the Ubertooth demodulator's discriminator is
the same phase-difference idea as that PHY's. The decoders of the pcap app, with their Bluetooth
LE gates, are in [Burst detection](bursts.md), the next page.

The 2-GFSK parameters and the frame of the MicroPython data link are in
[micropython](../../apps/micropython/README.rst).

## Ubertooth demodulator

[ubertooth](../../apps/ubertooth/README.rst) runs the device side of an Ubertooth One on the
ESP32-S3. The software demodulator replaces the CC2400's GFSK demodulator; the rest of the app
(USB, the sample ring, the instruction cache, its measurements) is in
[ubertooth internals](../apps/ubertooth-internals.md).

### Signal at the demodulator

The demodulator reads the ring's 4 MS/s stream, decimated by the ring's sums of 4
([Decimation by four](../samples/rings.md#decimation-by-four)), from an 8-bit sample ring. Since
2026-11-12 the wanted channel sits at DC in that stream: the LO is 4 MHz below it
([Tuning](../radio/tuning.md#lo-placement), [The channel at DC](#the-channel-at-dc)).

#### Stages

1. **Mid-step read.** The demodulator reads each 8 bit sample at the middle of the floor's step
   that stored it, x + 1/2: in the sum of 4 a constant 2; the reference gets (x << 6) + 32. So no
   bias sits on the channel.
2. **Filter.** A plain sum of 4 samples, in the reference, its C and the vector kernels alike. It
   puts nulls on every multiple of 1 MHz from the channel: for BR the neighbours at ±1 MHz fall
   into them, for LE those at ±2 MHz.
3. **Discriminator.** Im(z conj z[n-2]) over 2 samples. The cross product weighs each phase step
   by the power.
4. **Sample loop and groups.** The sample loop keeps the running sum and the group power. The per
   group logic holds the squelch and the gate for the burst edge (`s_gate`, set by
   `gfsk_demod_set_squelch()`). Both are explained further down: [Squelch floor](#squelch-floor),
   [Squelch gate after a retune](#squelch-gate-after-a-retune) and
   [Burst edge in an open squelch](#burst-edge-in-an-open-squelch).
5. **Slicer.** Once a burst is acquired, it keeps the levels and the early/late sums and samples
   at the nearest of 4 sampling instants a symbol.
6. **Output.** Symbols (time, bit, power), and the level, noise floor and horizon.

#### Why the mid-step read

- The floor alone left half a step of DC in the stream: the mean -27 of 8192, a spur 12 dB over
  the floor at 0 Hz. That was harmless 1 MHz off the channel, and on it the first try's undoing.
- Rounding in the sink removed the bias too, but gave the ring a zero level. On a channel at DC
  the noise between packets lies below a step even at gain 40 (the stream's noise is not white:
  at DC some 6 dB under the bands 1 to 2 MHz off).
- Rounding left it all zeros: the modes' floor (the 2nd percentile of the symbols' power) read
  pwr 7 and the squelch never closed.
- Read at the middle of its step no sample is zero: the floor reads pwr 99 at gains 40 and 60
  (the channel at +1 MHz: 97).
- Every 4th pair (`ubt rx sum off`) is gone, as it would fold the LO's leakage onto a channel at
  DC; `ubt rx ring` keeps the unit and LO overrides.

### Loss against the genie

The simulator (`tools/gfsk_sim.c`) prices the demodulator against an ideal receiver. The results
below were taken on 2026-11-12, with the channel at +1 MHz (the LO 5 MHz below it), before it
moved to DC that night.

#### Boxcar against every 4th pair

When the ring's boxcar replaced taking every 4th pair, the simulator agreed with the board. Its
front end got `SUM`, `LOOFF` and the app's 8 bit ring, `RING8`:

| Front end | LE, BER 1e-3 (Eb/N0) | BR, BER 1e-2 |
| --- | --- | --- |
| Boxcar (sums of 4) | 14.6 dB | 16.1 dB |
| Every 4th pair | 18.7 dB | 19.7 dB |
| Every 4th pair, the LO 5 MHz below | 18.6 dB | |

The LO's side alone changes nothing, so a mix by +4 MHz with the LO back above the channel would
not add anything worth the ring's change.

#### Distance to the ideal receiver

- The demodulator then sat some 2 to 2.5 dB from the simulator's ideal receiver (floating point at
  16 MS/s, genie timing: 1e-3 at about 12.7 dB), where it was 6 before.
- Its parameters stay where they were: disc, win, dd_k, toff, nseed, seed_skip and el_shift
  swept, none moves the 1e-3 point by more than the simulation's 0.3 dB.
- The conclusion then was that what is left would take another structure (a filter matched to
  GFSK, say), and with it the bit for bit reference. The next section, which splits the gap,
  showed it would not.

#### Where the last 2 dB go

Measured 2026-11-12, afternoon, with the channel at +1 MHz. The filter row ("4 taps after the
derotation") describes the demodulator of then; since the move to DC its filter is the plain sum
of 4.

The simulator's genie (floating point at 16 MS/s before the analog filter, a Gaussian filter, the
phase steps summed over each symbol, the exact carrier offset and symbol timing) got a twin on the
ring's 4 MS/s stream (`REF4=1`). The twin's filter, discriminator and timing switch to the
demodulator's one at a time (`REF4F`, `REF4D`, `REF4T`).

On the same streams, the SNR at a bit error rate of 1e-3 (LE) and 1e-2 (BR), 1.5 million bits a
point:

| Receiver | LE | BR |
| --- | --- | --- |
| Genie at 16 MS/s | 12.89 dB | 13.30 dB |
| Genie on the 4 MS/s ring stream | 13.87 | 14.20 |
| ... with the demodulator's filter (4 taps after the derotation) | 14.32 | |
| ... and its discriminator (Im(z conj z[n-2]) over 2 samples) | 13.85 | 14.27 |
| ... and the nearest of 4 sampling instants a symbol | 14.25 | 14.69 |
| The demodulator, behind the modes' squelch | 14.65 | 15.13 |

#### The four contributions

The rows of that table split the gap between the genie and the demodulator:

- **The front end, 1 dB:** the 4 MS/s stream. The boxcar's nulls are points, so the noise of the
  bands around 4 MHz still folds in partly; the 8 bit ring is 0.07 dB of it (`RING8=0`). A better
  decimation filter would have to fit CPU 1's 3 cycles a pair left.
- **The structure, nothing:** the 4 taps and the cross product, which weighs each phase step by
  the power, do as well as a Gaussian filter and the phase. A demodulator structure matched to
  GFSK would not gain anything, nor be worth losing the bit for bit reference.
- **The timing, 0.4 dB:** the sampling instant up to an eighth of a symbol off. The genie's exact
  rows interpolate linearly between the samples; the demodulator would need the fraction from the
  preamble.
- **The estimation, 0.4 dB in LE and 0.45 in BR:** the sampling phase chosen, the levels from the
  preamble.

#### BR bursts in the simulator

- The simulator's BR bursts had the preamble's last bit equal to the first sync bit, where it
  alternates into the sync word. That cost the demodulator 1 dB it does not lose on air.
- Found the same day: the first version of this analysis had BR losing 1.6 dB in its estimation,
  0.85 of it to when the acquisition starts (`seed_skip`), which with the bursts right makes no
  difference.
- BR figures in the sections before this one (in time) are about 1 dB pessimistic for the same
  reason.
- `test_br_noise_behind_squelch` covers BR in noise behind the squelch.

#### Always emitting

- Always emitting, the demodulator acquires few bursts at low SNR (0.06 a burst at 12 dB, 0.26 at
  13): the only edge there is a 6 dB step.
- Its curve falls off sooner (LE 1e-2 at 13.27 dB against 11.47 behind the squelch); above 14 dB
  both agree.

### The channel at DC

2026-11-12, night. With the channel at +1 MHz after the mix, decimating by 4 folded the bands 3
and 5 MHz off onto it, where the sums of 4 reach only -10 and -13 dB: their nulls lie at 4 and
8 MHz.

#### Decimation filters compared

`tools/gfsk_sim.c` got a switchable decimation (`DECIM`: `box`, `cic2` and `cic3` the sums
convolved with themselves, `lpN` an N tap low pass) and more 8 bit ring modes (`RING8`: 2
rounded, 3 the floor read at the middle of its step). LE at a bit error rate of 1e-3, behind the
modes' squelch:

| Decimation | The demodulator | The genie at 4 MS/s |
| --- | --- | --- |
| The sums of 4, the channel at +1 MHz | 14.62 dB | 13.89 dB |
| Sums of second and third order | 14.86, 15.89 | 13.30, 13.40 |
| A 24 tap low pass | 13.75 | 13.13 |
| The sums of 4, the channel at DC | 13.61 | 13.00 |

- The higher order sums lose for the demodulator: their passband falls 2 to 3 dB across a channel
  at +1 MHz, the tones come out unequal, and its discriminator weighs each phase step by the
  power.
- A flat low pass gains about as much, for 2 to 3 of CPU 1's cycles a pair.
- With the LO 4 MHz below the channel the aliases fall onto the sums' nulls for nothing.

#### The move

- The channel sits at DC now (`UBT_RX_LO_OFFSET_MHZ`), and the demodulator's filter is the plain
  sum of 4, in the reference, its C and the vector kernels alike.
- The discriminator's sign is as before; the levels and the carrier offset estimate stay.
- The unit tests' generator puts its bursts at DC.

In the simulator, behind the squelch:

| | Channel at +1 MHz | Channel at DC |
| --- | --- | --- |
| LE, BER 1e-3 | 14.62 dB | 13.61 dB |
| LE at the board's -31 dBFS | 15.00 dB | 14.01 dB |
| BR, BER 1e-2 | 15.11 dB | 14.33 dB |
| LE, BER 1e-3, carrier offset +100 kHz | not reached by 17 dB | 16.7 dB |

With a carrier offset of -100 kHz the channel at DC reaches that point 0.3 dB later.

#### On the air

The C6 far from the S3 (the PC's adapter heard it at -84 to -89 dBm), its good advertisements on
channel 39 in 10 s at the modes' gain of 60.

- **Whole images.** Alternating whole images every three minutes, the far link's fading over
  minutes (rounds of 6 to 18 and of 90 to 100) left it open: the channel at DC was ahead in 7 of
  10 rounds with the sink rounding, behind in 2 of 3 read at mid-step.
- **One image, switched.** One test image had both, switched between runs (`ubt rx dc on|off`, the
  +1 MHz FIR in C; not kept), alternating every 10 s, the order flipped every pair:

| 20 pairs of 10 s runs | The channel at DC | At +1 MHz |
| --- | --- | --- |
| Good advertisements | 1829 | 1303 |
| Runs ahead | 19 of 20 | 1 |

- 40 % more, 26 a run (t = 5.9): the simulator's 1 dB at a knee this steep.
- **Close to the S3**, the edge reached by lowering the gain instead, it did not show. At gains 27
  and 30 the two tied (105 against 104, 131 against 128, the sink rounding); at 24 the channel at
  DC lost (9 against 22, one round).
- There the receiver's noise lies below a step of the 8 bit ring (rounded, the floor falls 6.5 dB
  at gain 24): a signal a step or two above zero, which at DC turns slowly through its steps. The
  simulator, whose noise always spans a step, models that regime poorly.
- The modes run at 40 to 60.

#### Strong signals

At a quarter of the ADC's rate the products of its clipping land on the channel itself, and the
channel at DC takes them worse than +1 MHz did: the mechanism and the simulated losses are in
[ADC clipping products](../radio/impairments.md#adc-clipping-products), the gain control that
avoids them in [Clip guard](../radio/gain.md#clip-guard).

#### Following

The C6 far, alternating the images, two rounds of five connections at each gain, the order flipped
in the second:

| Ten connections each | +1 MHz, gain 40 | DC, gain 40 | +1 MHz, gain 60 | DC, gain 60 |
| --- | --- | --- | --- | --- |
| CONNECT_INDs followed | 10 | 9 | 8 | 7 |
| Events missed | 29 % | 33 % | 19 % | 14 % |
| The PC's packets on channel 39 with a bad CRC | 0 of 200 | 3 of 223 | 5 of 207 | 7 of 161 |
| The C6's advertisements decoded | 0 | 59 | 640 | 904 |
| Stream lost | 0.004 % | 0.052 % | 0.005 % | 0.014 % |

- With the C6 far both miss a fifth to a third of the events (close by, 1 %).
- At gain 40 the channel at DC heard the C6's advertisements, +1 MHz none.
- The PC's packets fail somewhat more often at DC, too few to tell apart (another run at gain 60
  lost 5 of 39).

### Squelch floor

From 2026-11-11, afternoon. The modes set their squelch from a noise floor. How the level and
the noise floor follow the gain is in [Gain](../radio/gain.md).

- That floor was the demodulator's tracker after 30 ms of the stream, which follows the mean.
- A measurement that fell into a run of Wi-Fi bursts read up to 25 dB high and kept the mode
  nearly deaf (6 advertisers instead of 44).
- It is now the 2nd percentile of the symbols' power over the measurement, offset to the
  tracker's reading of plain noise (`ubt_floor_hist`, unit tests with bursts 91 % of the time).

### Squelch gate after a retune

2026-11-12, night.

#### Symptom

- Following caught fewer CONNECT_INDs than sniffing the same connections: 143 of 224 (64 %) over
  the night against 37 of 40.
- The C6's advertisements right before a missed one were decoded, and the finder never saw the
  PC's CONNECT_IND, not even with a bad CRC.
- Following with le_conn left out (the same code, nothing followed) caught 14 of 16.
- The first connection of every run was caught, and on the advertising channel nothing reset the
  finder or changed its configuration.

#### The reset

The demodulator's reset runs at every tune and every restart of the ring. It zeroed the gate for
the burst edge (`s_gate`) along with the stream's state, though `gfsk_demod_set_squelch()` sets
it. BR sets its squelch after every hop; the BLE modes set it once, after their first tune.

So from a follow's first retune on (and in promiscuous mode, or after a restart of the ring)
every group passed the gate:

- each burst's edge fell on the first group of the 40-sample replay, and its sampling phase and
  slicing levels were taken from the noise before it;
- a second burst while the squelch stayed open was not acquired anew.

Sniffing kept its first tune and was not affected.

#### Fix and result

Now the reset restores the gate from the threshold, in the frozen reference too, which had the
same reset. The bench compare and the differential tests stay identical, and
`test_reset_keeps_squelch` checks that a stream after a reset decodes as it does with the squelch
set right after one.

Interleaved, four pairs of 8 connections (gain 40, the C6 at -15 dBm on channel 39):

| | Gate zeroed (before) | Gate kept |
| --- | --- | --- |
| CONNECT_INDs caught | 25 of 32 | 29 of 32 |
| Connection events followed, missed | 1334, 9.7 % | 1531, 0.9 % |
| Followed connections whose end was missed (`timeout`) | 9 of 25 | 0 of 29 |

- Two more runs with the clean image caught 15 of 16.
- The events they missed (4 %) came almost all from three connections whose end the follower
  missed (the LL_TERMINATE_IND or the PHY update): it then follows the connection to its
  supervision timeout.

### Burst edge in an open squelch

2026-11-12, morning. The CONNECT_INDs still missed (about 9 %) matched sniffing's, and the C6's
advertisements, 25 dB weaker, were missed as often (10 to 13 %).

#### The simulator's additions

To look for the demodulator's share, `tools/gfsk_sim.c` got:

- pairs of bursts (`PAIR`: every second burst 150 us after the one before, `PAIRDB` stronger,
  with a carrier offset of its own within `PAIRCFO`);
- the squelch as the modes set it (`SQAUTO`: the floor tracked while always emitting over `LEAD`
  us of noise, plus the margin);
- packets counted as the finder takes them: the access address exactly in the emitted bits, then
  every payload bit. The time stamp comparison counts a sampling instant half a symbol late as
  errors, which the finder never sees.

#### Lumpy noise

- At the board's levels at gain 40 (the C6's advertisements at about -31 dBFS from their RSSI,
  the noise about 20 dB under them, near the 8 bit ring's step) the squelch opens on noise now
  and then: the ring's steps make the noise lumpy.
- A burst that began while it was open was lost when two groups of noise above the gate came
  right before it.
- The gate's run had then passed its second group, the only one where the step up was looked at,
  and the burst was sliced with the phase and levels left by the noise.

#### Fix

Now a group more than 4 times (6 dB) over the smoothed power before it starts the gate's run
over, so the next group decides on an edge as after a pause (`restep` in the reference, in
`groups_c()` and `gfsk_groups.S`; `test_step_up_in_open_squelch`).

#### Result

The simulator over six seeds, LE at -31 dBFS behind the modes' squelch:

| | Before | Step up |
| --- | --- | --- |
| Single bursts, AA not found, 21 / 24 dB | 1.16 / 1.31 % | 0 / 0 |
| An advertisement and a CONNECT_IND 25 dB stronger, 21 dB: first, second lost | 6.67 %, 1.67 % | 0.05 %, 0 |
| The same at 24 dB | 6.82 %, 1.62 % | 0, 0 |
| Acquisitions a burst | 1.1 to 1.35 | 1.2 to 1.5 |

- Nothing changes at 18 dB, where bit errors dominate, or above the ring's steps (-24 dBFS), or in
  the curves always emitting (LE 1e-3 at 14.65 dB, BR at 20.58).
- The bench times it on the same capture within a cycle (always 51 both, behind the squelch 10 to
  11 both).
- On air, sniffing interleaved (four pairs of 8 connections): CONNECT_INDs 29 and 28 of 32, the
  C6's advertisements missed 10.0 and 10.2 %, good packets 12517 against 11996. The gain the
  simulator predicts there (1 to 2 %) is below what 32 connections resolve.
- Following with it caught 15 of 16 and missed 0.6 % of the events, every end of a connection
  caught.

### Cost against the 60-cycle budget

Measured 2026-11-11 with `ubt rx bench` on the XIAO ESP32-S3 with panel (A4CB8FD1D880). The
budget at 4 MS/s is 60 cycles a sample.

- **Full path** (squelch open): 75 cycles a sample with the ring stopped, 77 with it running (no
  memory contention): 23 in the sample loop, 21 in the per group logic, 26 in the slicer (a build
  with `GFSK_PROF`).
- **In the modes** about 100: other threads and interrupts in between, and the start of each
  opening.
- **Behind the squelch** the modes average 20 to 35 cycles a sample, but a busy channel keeps the
  squelch open for long stretches (9 to 16 % of the time on channels 37 and 38 here), longer than
  the 2 ms ring absorbs.
- **IRAM.** The slicer, the per group logic and the sample loop run from IRAM. Moving the first
  two there took the full path from 137 to 103 cycles a sample in the modes; 1.4 KB of internal
  RAM below the ring banks are left.

### Fitting the budget

From 2026-11-11, evening: the demodulator is to fit its 60 cycles a sample without changing a
single decision.

#### Bit-exact reference

- `src/gfsk_ref.c` is the demodulator of the following measurements, frozen
  (`CONFIG_APP_UBERTOOTH_GFSK_REF`, default on; in flash, its state in PSRAM).
- `gfsk_demod.c` has to emit the same symbols (time, bit, power) and keep the same level, noise
  floor and horizon for the same input pushed in the same pieces.
- `ubt rx bench <mhz>` runs both on 4 ms of the stream, always emitting and behind a squelch
  (floor + 14), in pushes of 256 samples and of random sizes, and reports the first difference.
- `tests/unit/ubertooth_demod` does the same on synthetic streams ([Testing](../dev/testing.md)).
- Changes to the algorithm come later, one at a time, priced with `tools/gfsk_sim.c`.

#### The reader

The ring keeps 8 bits a sample, and the session's reader widened every chunk to int16 for the
demodulator, some 22 cycles a sample that no log counted.

- `ubt_radio_rx_read8()` copies the 8 bit planes as they are, with 48 samples of history in front
  for the demodulator's filter, which reads them directly.
- The copy is a word loop, not the S3's ROM `memcpy` ([Chip notes](../dev/chips.md)).

| | Before | After |
| --- | --- | --- |
| Reader, bench | 22 cycles a sample | 5 |
| Reader, in the modes | 30 | 9 |
| Full path, bench | 75 to 77 | 75 to 77 |
| Samples lost sniffing on channels 37 / 38 (15 s runs, the images alternating, two rounds) | 45 / 49 % | 38 / 42 % |
| Packets with a good CRC, same runs | | 40 % more |

#### Rewrites in C

- What did not help: the per group logic and the slicer rewritten in C (their state in locals,
  two or three passes over a batch, a slicer working in segments). All bit-exact, none faster.
- The code runs at about one instruction a cycle (some 95 instructions a group in the per group
  loop): the cost is the number of instructions, not stalls or spills.
- The next step inside the demodulator was the vector unit for its front end, or assembly.
- The bigger cost was outside the demodulator, in USB polls and the instruction cache:
  [ubertooth internals](../apps/ubertooth-internals.md).

#### The vector unit

2026-11-11, night. The channel then sat at +1 MHz, and the front end's FIR was the derotating
filter described here; since 2026-11-12 the vector kernels compute the plain sum of 4
([The move](#the-move)).

- **Reader.** The reader's copy runs on the unit: two 128-bit loads and stores a step, interrupts
  locked for its 0.2 us, as for the spectrum analyser's FFT. 1 cycle a sample, from 5 on the bench
  and 6 in the modes.
- **Cycle counter.** The session's statistics read the CPU's own cycle counter (the mode thread is
  pinned to CPU 0) instead of `k_cycle_get_64()`, which reads the system timer under a spinlock,
  some 1.7 % of CPU 0.

#### Front-end planes

- **Planes.** The demodulator's front end works on planes computed ahead of the paths
  (`src/gfsk_pie.S`; the same in C in `gfsk_demod.c` for the host tests and for an input not laid
  out as the unit needs).
- **FIR plane.** The FIR's output z for a stretch of the push, in two passes: u = x - x[n-2] per
  plane, then z_i = u_i - u_q[n-1] and z_q = u_q + u_i[n-1].
- **Discriminator plane.** While the full path runs, the discriminator x 256, its 32-bit products
  joined from the low and high halves of `EE.VMUL.S16`.
- **What stays scalar.** The sample loop keeps the running sum and the group power; the closed
  path reads z instead of the FIR of every fourth sample. The running sum stays scalar: the unit's
  adds saturate, the sum wraps.
- **History.** The input history grew to 80 samples (the plane's block holding the replay's first
  sample, and the block before it the unit reads).
- **Check.** `ubt rx bench` checks the planes against C on 8 random and extreme blocks (0 samples
  differ) and still finds every symbol of the reference.

| | Sample loop, bench | Full path, bench | In the modes, open / closed |
| --- | --- | --- | --- |
| Before | 23 cycles a sample | 75 to 77 | 74 / 12 |
| The FIR on the unit | | 68 to 70 | 67 / 10 |
| The discriminator too | 8 | 65 to 67 | 62 / 10 |

#### Slicer in assembly

The slicer, once a burst is acquired, went to assembly (`src/gfsk_slice.S`; C keeps the
acquisition and the flushes).

- The C kept every value in memory around the symbol stores (char stores alias everything) and
  spilled, some 100 instructions a symbol.
- The assembly keeps the levels and the early/late sums in registers and reads the ring window
  off one address; the one symbol in 32 whose window wraps the ring's end masks each index.
- It aligns the cursor to the sampling phase only when the phase changed.
- `ubt rx bench` runs it against the C on 4000 random states (cursor, levels, early/late sums and
  count, rings that wrap 32 bits, a full output): none differs.
- The slicer went from 26 to 19 cycles a sample, the full path from 67 to 59 on the bench (within
  the 60 for the first time), in the modes from 62 to 57 to 59 open.

#### Group logic in assembly

The per group logic then went to assembly too (`src/gfsk_groups.S`, reading the demodulator's
fields in place at offsets `gfsk_demod_off.h` lists and `gfsk_demod.c` checks).

- Both kernels were tuned: zero-overhead loops, the configuration in a register of flags, the
  slicer's state in place, the common path falling through.
- `ubt rx bench` runs each against its C on 4000 random states: none differs.

| | Sample loop | Per group logic | Slicer | Full path, bench | In the modes, open |
| --- | --- | --- | --- | --- | --- |
| Before the vector unit | 23 | 21 | 26 | 75 to 77 | 74 |
| Now | 8 | 17 | 16 to 17 | 50 to 52 | 53 to 55 |

In a quieter room (16 to 29 % of the samples open on channels 37 and 38, 45 to 105 advertisers)
no sample was lost; in the office before, with the copy and the FIR on the unit, 5 to 10 %.

#### Long runs

Long runs showed what short ones did not: in long stretches of open squelch the full path, the
finder and USB together came to a little over the 60 cycles. The measurements, and the two changes
that halved the loss (the slicer's assembly before acquisition among them), are in
[ubertooth internals](../apps/ubertooth-internals.md).

### Status

- The demodulator's full path fits CPU 0's budget: about 53 of 60 cycles a sample open, 10
  closed.
- With the channel at DC the demodulator sits 0.6 dB from the simulator's genie (LE); the gap is
  its timing and estimation, not its structure.
- Not done: the demodulator's timing and estimation.
- The status of the app as a whole is in [ubertooth internals](../apps/ubertooth-internals.md).

## Bluetooth LE from channelizer captures

`tools/ble_pcap.py` of [chancap](../../apps/chancap/README.rst) decodes the captures of the BLE
grid ([Channelizer](channelizer.md)) on the host:

- it demodulates LE 1M;
- it finds the advertising access address, or the one given with `--aa`, with `--crc-init`;
- it dewhitens with the channel's index;
- it checks the CRC, using `apps/ubertooth/tools/le_ref.py` for the link layer bits.

The output (link type and radio header fields) and the command lines are in the chancap README.

## MicroPython burst receiver

The packet flowgraphs of [micropython](../../apps/micropython/README.rst) receive GFSK bursts
with `digital/_fsk.py`, in C in `cmod/esdr_fsk.c`.

- **Discriminator.** A discriminator and its sum over a symbol.
- **Access code search.** Per sampling phase, a search for the access code's last 32 bits against
  the mean of that phase's last 32 symbols.
- **Phase choice.** Around a hit, the phase that correlates best with the whole code wins.
- **Offset and deviation.** The code gives the carrier offset and the deviation and is sliced
  again against that offset (at most `threshold` errors).
- **Tolerance.** Carrier offsets of the order of the deviation are fine (tens of kHz between two
  boards' crystals). Real GNU Radio's `gfsk_demod` wants the offset well inside the deviation.
- **No tracking.** No lock-in time beyond the code, no timing tracking (frames up to 255 bytes).

How to choose the access code and pad packets for `gfsk_mod`, and the data link's 2-GFSK
parameters and frame, are in the micropython README.

<!-- nav-start -->
Previous: [IEEE 802.15.4](ieee802154.md) · Up: [Modulation and coding](index.md) · Next: [Burst detection](bursts.md)
<!-- nav-end -->
