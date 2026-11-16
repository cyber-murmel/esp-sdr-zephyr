# Ubertooth One on the ESP32-S3 SDR

`apps/ubertooth` runs the device side of an [Ubertooth One](https://github.com/greatscottgadgets/ubertooth)
on the software radio, so the unmodified upstream host tools work. This page is the design and the
measurement log; the app's README is the user's view.

## What the original does, and what replaces it

| Ubertooth One | Here |
| --- | --- |
| LPC1758 firmware, USB bulk IN 0x82 with 64 byte packets, vendor requests on EP0 | `src/ubt_usb.c`, `src/ubt_ctrl.c`, `src/ubt_queue.c` |
| CC2400 radio: GFSK demodulator with bit clock, carrier sense, RSSI, sync word detector | software on the 4 MS/s ring stream: `gfsk_demod`, `ubt_rssi`, `ubt_le` |
| CC2400 transmit (GFSK modulator, packet engine) | `gfsk_mod`, `ubt_tx` on the DAC |
| Spectrum analyser: retune per MHz, read RSSI | wideband burst captures and an FFT: `ubt_specan` |
| Timer0: CLKN 3.2 kHz, CLK100NS | one timeline from the CPU cycle counter: `ubt_clock` |
| LPC IAP serial number, part number | the chip's MAC in the serial, a fixed part id |

The USB protocol is `src/upstream/ubertooth_interface.h` (unchanged, GPL-2.0-or-later). The host tools
check `bcdDevice` (the API version, 0x0107), open interface 0 and use bulk IN 0x82; they accept only the
USB IDs 1d50:6000, 1d50:6002 and ffff:0004.

## Signal path

The receive ring runs at 16 MS/s and delivers 4 MS/s through `esp_sdr_ring_run()` (boxcar-4 setup,
`shift_fs4`): it mixes by -4 MHz and sums 4 pairs, a boxcar whose nulls fall on the bands 4 and 8 MHz
off, which would otherwise fold onto the channel. The LO sits 4 MHz below the wanted channel, so the
channel appears at DC in the stream and the LO leakage, mixed to -4 MHz, on one of those nulls; the
demodulator reads each of the sample ring's 8 bit samples at the middle of the floor's step that
stored it, so no bias sits on the channel. In the demodulator a sum of 4 samples puts nulls
on every multiple of 1 MHz from the channel: for BR the neighbours at +-1 MHz fall into them, for LE
those at +-2 MHz. Until 2026-11-12 the channel sat at +1 MHz (the LO 5 MHz below, the demodulator's
sum after a derotation by 1 MHz), where the bands 3 and 5 MHz off folded in past the nulls ("The
channel at DC" below); before the sums the ring took every 4th pair, with the LO 3 MHz above.

The ring's sink (CPU 1) keeps the stream in a ring of 8192 samples (2 ms) in internal RAM, 8 bits
each (the int16 stream >> 6: the ADC's 10 bits without the lowest two, far below the noise at gain
60); the mode thread (CPU 0) reads it in chunks of 256 through `src/ubt_rx_sess.c`, the session the
demodulating modes share: tune, demodulate, gaps and ring restarts, and one 64 bit timeline of the
samples (`ubt_clock`) for le_conn, the promiscuous mode and the packets' time stamps. A ring in
PSRAM was tried: its stores cost the masked ring CPU some 13 cycles a pair (the ring went from 5 to
18.6 cycles a pair, over its 15), it abandoned most units.

## Hardware limits that shape the app

* A retune is `esp_sdr_set_freq()`: 3 ms of settling plus a calibration, and the ring has to be stopped.
  Bluetooth BR hopping (1600 hops/s) is out of reach, as is jamming (a reaction inside a packet);
  BLE connection following works for intervals that leave time for a retune and a ring start.
* One radio user at a time, half duplex.
* Internal RAM below the three ring banks is full: the app's stacks and USB buffers live in the high
  region (`ESP_SDR_HIGH_RAM`), the packet queue in PSRAM.

## Measurements (XIAO ESP32-S3, A4CB8FD1C700, 2026-10-14)

Retune, `ubt tune <a> <b> 20` (time of `esp_sdr_set_freq()`):

| Frequencies | min | mean | max |
| --- | --- | --- | --- |
| 2412 / 2417 MHz (Wi-Fi channel grid) | 3143 us | 3161 us | 3511 us |
| 2441 / 2442 MHz | 3143 us | 3473 us | 4175 us |
| 2402 / 2403, 2405 / 2408, 2426 / 2480 MHz (off grid) | 3766 us | 3784 to 3804 us | 4129 to 4240 us |

The 3 ms are a fixed busy wait in the library (`SDR_RETUNE_SETTLE_US`); an off-grid MHz adds a VCO
calibration of about 0.6 ms.

Ring start, `ubt rx start`: retune about 4.2 ms, the first samples about 4 ms after that. The stream
at 4 MS/s ran for minutes without a gap or lost pair.

Noise floor, DC spur and offset of the stream on 2430 MHz (`ubt rx floor`, Hann window, 256 points, dBFS
per bin of a full scale tone; ambient signals present):

| Gain index | Floor | DC spur | In-channel (+0.5 to +1.5 MHz) | Mean I / Q (LSB, full scale 8192) |
| --- | --- | --- | --- | --- |
| AGC | -46.7 | -37.5 | -43.8 | -127 / -256 |
| 0 | -72.8 | -60.9 | -73.0 | -3 / -18 |
| 20 | -70.5 | -59.6 | -70.8 | 21 / 3 |
| 40 | -62.3 | -44.2 | -61.5 | -39 / 30 |
| 60 | -57.3 | -57.8 | -57.6 | 6 / 1 |
| 82 | -40.4 | -34.0 | -39.6 | -235 / -360 |

On a busy channel (2426 MHz, BLE advertising channel 38) the in-channel band stood 7 dB above the floor.

Spectrum analyser, `ubertooth-specan -l 2402 -u 2480` against the board (serial version: retune,
capture, 32 frames, FFT per placement; two placements 2428 / 2455 MHz): 504 complete sweeps in 6 s,
ascending 2402 to 2480 MHz, about 84 sweeps/s (about 11.9 ms per sweep: two retunes of 3.8 ms, two
captures of 0.2 ms, two times 32 frames of FFT and the rest). Overlapping the FFT with the next retune
would reach about 135 sweeps/s. The levels are relative: the full scale reference of the byte is a
guess (`ubt specanfs`) until a source of known power is available.

Transmit, `ubertooth-btle -s22:44:66:88:aa:cc -A38` (ADV_IND, flags 05, every 100 ms on 2426 MHz,
LO at 2422 MHz, TX gain step 6): the PC's Bluetooth adapter (`bluetoothctl scan on`) lists
"Device 22:44:66:88:AA:CC (public)" with AdvertisingFlags 05, i.e. the whitening, CRC-24, GFSK
modulator, DAC and RF all work as a BLE advertiser. TX_TEST (PRBS9 loop on 2441 MHz) starts and
stops; its output was not observed.

## Measurements (XIAO ESP32-S3 with panel, A4CB8FD1D880, 2026-11-11, an office)

Tools: the nixpkgs Ubertooth host tools with the project USB ID (bench recipe in the app README).
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
| `ubertooth-util -q80` | | the three Wi-Fi channels read every sweep; with the placeholder calibration nothing reaches -80 dBm (below) |

The demodulator's full path (squelch open), `ubt rx bench`: 75 cycles a sample with the ring
stopped, 77 with it running (no memory contention): 23 in the sample loop, 21 in the per group
logic, 26 in the slicer (a build with `GFSK_PROF`). In the modes about 100 (other threads and
interrupts in between, and the start of each opening). The budget at 4 MS/s is 60. Behind the
squelch the modes average 20 to 35 cycles a sample, but a busy channel keeps the squelch open
for long stretches (9 to 16 % of the time on channels 37 and 38 here), longer than the 2 ms
ring absorbs. The slicer, the per group logic and the sample loop run from IRAM (moving the first
two there took the full path from 137 to 103 cycles a sample in the modes; 1.4 KB of internal RAM
below the ring banks are left).

Levels: the stream at gain 60 reads -52 dBFS a bin (in-channel -44.5) on this board, about 13 dB
above the October numbers of the board without a panel. The noise floor the modes set their
squelch from follows the channel: pwr 100 to 136 on busy advertising channels. The spectrum
analyser's placeholder full scale puts the noise at -120 dBm and nearby Wi-Fi at -94 dBm, some
30 dB below the stream's placeholder (UBT_RSSI_K_DEFAULT); neither is checked against a known
source.

Bugs found on the way: the demodulator sliced the look-ahead after a gap up to the far side of
the gap out of stale ring entries (some 250 made-up symbols a gap); its noise floor tracker was
seeded again by the first group after every gap (a Wi-Fi burst set the squelch up to 30 dB high,
once nothing passed) and could wrap at full scale; the sample ring's reader read the writer's
position three times when it dropped a damaged chunk (the gap it reported and its new position
disagreed, the demodulator's time drifted); the mode thread was pinned after it had started, which
fails silently (it could run on CPU 1); the BLE finder's clock callback got the promiscuous mode's
context (a division by zero). All fixed, the demodulator's with unit tests.

## Following (2026-11-11, afternoon, a busier office)

Bench: a XIAO ESP32-C6 as a connectable peripheral that advertises on channel 39 only, at -15 dBm
(scratch app, Zephyr's controller, no pairing), the PC's adapter (Intel, Linux) connecting to it
again and again with `bluetoothctl`, the S3 running `ubertooth-btle -f -A39` (or `-n` to compare).
The C6 pins every second connection to the 1M PHY; left alone, the PC moves each to 2M.

| What | Result |
| --- | --- |
| A 1M connection (45 ms interval, CSA #2, channel map 22 to 36) | followed from the CONNECT_IND to its end, 25 s, 480 packets, a packet in 59 % of the events; LL_CHANNEL_MAP_IND and LL_PHY_UPDATE_IND (no change) decoded on the way |
| A connection moved to 2M | followed until the LL_PHY_UPDATE_IND, then `PHY change` and back to advertising (when the update was missed: `timeout`) |
| CONNECT_INDs caught, gain 40 | sniffing 55 of 70 (79 %); following 28 of 97 before the fix below, 1 to 5 of 6 per minute after it, as sniffing then |
| Long (37 byte PDU) against short (24) advertisements | 335 against 365 a minute at gain 40, 274 against 305 at gain 60: length costs some 10 % |

Gain. At index 60 the PC's adapter (a metre away, some +10 dBm) and the C6 at its default power
arrive at full scale (the RSSI pins at -64 to -65 dBm on the placeholder scale): their packets come
out with bit errors or not at all, and no CONNECT_IND of the PC was caught in five tries. The C6's
level against the gain index (one transmitter, fixed power): 60, 56, 52, 48, 44, 40, 36, 32, 28
read 0, 6.1, 8.4, 11.4, 17.0, 21.1, 25.2, 28.6 and 32.7 dB lower, about 1 dB a step. The stream's
noise floor (`ubt rx floor`, 2480 MHz) falls only 4.5 dB from 60 to 50 and stays at -66 to -70
dBFS a bin below that, set by the receiver after the gain stage, not by the 8 bit ring (a ring
four times finer moved it 1 to 2 dB): below 50 every step down costs sensitivity. On ambient
advertisers (channels 37 and 38, interleaved 15 s runs) gain 60 heard 38 to 44 devices, gain 50
29 to 36. The default stays 60; `ubt rx gain` sets another for a connection close by. The
hardware AGC idled 24 dB above index 60 and halved the catch rate.

Bugs found: the noise floor the modes set their squelch from was the demodulator's tracker after
30 ms of the stream, which follows the mean: a measurement that fell into a run of Wi-Fi bursts
read up to 25 dB high and kept the mode nearly deaf (6 advertisers instead of 44); it is now the
2nd percentile of the symbols' power over the measurement, offset to the tracker's reading of
plain noise (`ubt_floor_hist`, unit tests with bursts 91 % of the time). In follow mode le_conn's
clock conversion (two 64 bit divisions, some 800 cycles) ran on every poll even without a packet
or a connection; on the advertising channel that margin decided whether the ring overflowed
during an advertisement and the CONNECT_IND behind it (samples lost 3.6 % sniffing, 11.7 %
following; CONNECT_INDs 15 of 24 against 3 of 24, interleaved). It runs only when there is
something for it now.

## The demodulator's budget (2026-11-11, evening)

The demodulator is to fit its 60 cycles a sample without changing a single decision.
`src/gfsk_ref.c` is the demodulator of the following measurements above, frozen
(`CONFIG_APP_UBERTOOTH_GFSK_REF`, default on; in flash, its state in PSRAM), and `gfsk_demod.c`
has to emit the same symbols (time, bit, power) and keep the same level, noise floor and horizon
for the same input pushed in the same pieces. `ubt rx bench <mhz>` runs both on 4 ms of the
stream, always emitting and behind a squelch (floor + 14), in pushes of 256 samples and of random
sizes, and reports the first difference; `tests/unit/ubertooth_demod` does the same on synthetic
streams (`doc/testing.md`). Changes to the algorithm come later, one at a time, priced with
`tools/gfsk_sim.c`.

The reader. The ring keeps 8 bits a sample, and the session's reader widened every chunk to int16
for the demodulator, some 22 cycles a sample that no log counted. `ubt_radio_rx_read8()` copies
the 8 bit planes as they are, with 48 samples of history in front for the demodulator's filter,
which reads them directly. The copy is a word loop: the ROM's `memcpy` moves bytes, and GCC turns
copy loops into calls to it unless built with `-fno-tree-loop-distribute-patterns`.

| | Before | After |
| --- | --- | --- |
| Reader, bench | 22 cycles a sample | 5 |
| Reader, in the modes | 30 | 9 |
| Full path, bench | 75 to 77 | 75 to 77 |
| Samples lost sniffing on channels 37 / 38 (15 s runs, the images alternating, two rounds) | 45 / 49 % | 38 / 42 % |
| Packets with a good CRC, same runs | | 40 % more |

What did not help: the per group logic and the slicer rewritten in C (their state in locals, two
or three passes over a batch, a slicer working in segments). All bit-exact, none faster. The code
runs at about one instruction a cycle (some 95 instructions a group in the per group loop): the
cost is the number of instructions, not stalls or spills. The next step inside the demodulator is
the vector unit for its front end, or assembly.

The bigger cost was outside it. On the air each of the demodulator's parts took about 1.5 times
its bench time, as the cycle counts include whatever preempts the mode thread. Thread run times
(`CONFIG_THREAD_RUNTIME_STATS`) over a 20 s sniff: the mode thread 12.6 s, `usbd` 3.7 s,
`usb_otg` 2.2 s, about 30 % of CPU 0 in USB. The host polls with `UBERTOOTH_POLL`, a control
transfer, about 975 times a second (19510 polls, 1606 of them with a packet): some 270 us of CPU 0
a poll, against 45 us with CPU 0 otherwise idle. A profiler showed why
(then the app's `ubt prof`: the ring sink on CPU 1 recorded CPU 0's program counter and thread once
a sink call; the library's `CONFIG_ESP_SDR_CPU_PROF` and `prof` replace it, see "The instruction
cache"). The instruction cache is 16 KB; a poll runs some 15 KB of the
USB stack from flash (79 functions: the DWC2 driver's interrupt handler and thread, usbd's control
transfers, the buffer pool's heap), and the mode loop ran another 7 KB of flash code per chunk, so
each evicted the other. Two changes:

| Change | USB threads, share of CPU 0 |
| --- | --- |
| Before | 26 % |
| The mode loop's per chunk code in IRAM (the reader, the session's poll, the demodulator's entry, the finder's feed and gap search: 4.3 KB) | 18 % |
| An empty POLL in the BLE modes waits up to 4 ms for a packet (`ubt pollhold`), so the polls come at the pace of the packets | 5 % |
| ... up to 16 ms (2026-11-12; the same packets in interleaved runs, none dropped) | 4 % (7 against 4 % in a profile of its own) |

The wait relies on the host controller retrying the NAKed data stage by itself, without an
interrupt on the device (the DWC2 driver does not enable the IN token interrupts), and changes
nothing the tools see: a packet goes out as soon as it is queued, a burst of them back to back. On
the air (one image, the wait off and on alternating, three rounds of 20 s on channels 37 and 38):

| Wait | Samples lost | Packets with a good CRC (mean of 6) | Demodulator, open / closed |
| --- | --- | --- | --- |
| off | 19 to 25 %, mean 22 | 1525 | 90 to 92 / 15 cycles a sample |
| 4 ms | 14 to 17 %, mean 16 | 1638 | 75 / 12 to 13 |

What is left of the USB cost is mostly the kernel's: every atomic operation is a call
(`xtensa_cas`, out of line since atomics in PSRAM are emulated), some 4 % of CPU 0, and inlining
its fast path would take several KB of IRAM.

The vector unit (2026-11-11, night). The reader's copy runs on it (two 128-bit loads and stores
a step, interrupts locked for its 0.2 us, as for the spectrum analyser's FFT): 1 cycle a sample,
from 5 on the bench and 6 in the modes. The session's statistics read the CPU's own cycle counter
(the mode thread is pinned to CPU 0) instead of `k_cycle_get_64()`, which reads the system timer
under a spinlock, some 1.7 % of CPU 0. The demodulator's front end works on planes computed ahead
of the paths (`src/gfsk_pie.S`; the same in C in `gfsk_demod.c` for the host tests and for an
input not laid out as the unit needs): the FIR's output z for a stretch of the push, in two
passes (u = x - x[n-2] per plane, then z_i = u_i - u_q[n-1] and z_q = u_q + u_i[n-1]), and while
the full path runs the discriminator x 256, its 32-bit products joined from the low and high
halves of `EE.VMUL.S16`. The sample loop keeps the running sum and the group power, the closed
path reads z instead of the FIR of every fourth sample. The running sum stays scalar: the unit's
adds saturate, the sum wraps. The input history grew to 80 samples (the plane's block holding the
replay's first sample, and the block before it the unit reads). `ubt rx bench` checks the planes
against C on 8 random and extreme blocks (0 samples differ) and still finds every symbol of the
reference.

| | Sample loop, bench | Full path, bench | In the modes, open / closed |
| --- | --- | --- | --- |
| Before | 23 cycles a sample | 75 to 77 | 74 / 12 |
| The FIR on the unit | | 68 to 70 | 67 / 10 |
| The discriminator too | 8 | 65 to 67 | 62 / 10 |

The slicer, once a burst is acquired, then went to assembly (`src/gfsk_slice.S`; C keeps the
acquisition and the flushes). The C kept every value in memory around the symbol stores (char
stores alias everything) and spilled, some 100 instructions a symbol; the assembly keeps the
levels and the early/late sums in registers, reads the ring window off one address (the one
symbol in 32 whose window wraps the ring's end masks each index) and aligns the cursor to the
sampling phase only when the phase changed. `ubt rx bench` runs it against the C on 4000 random
states (cursor, levels, early/late sums and count, rings that wrap 32 bits, a full output): none
differs. The slicer went from 26 to 19 cycles a sample, the full path from 67 to 59 on the bench
(within the 60 for the first time), in the modes from 62 to 57 to 59 open.

The per group logic then went to assembly too (`src/gfsk_groups.S`, reading the demodulator's
fields in place at offsets `gfsk_demod_off.h` lists and `gfsk_demod.c` checks), and both kernels
were tuned: zero-overhead loops, the configuration in a register of flags, the slicer's state in
place, the common path falling through. `ubt rx bench` runs each against its C on 4000 random
states: none differs.

| | Sample loop | Per group logic | Slicer | Full path, bench | In the modes, open |
| --- | --- | --- | --- | --- | --- |
| Before the vector unit | 23 | 21 | 26 | 75 to 77 | 74 |
| Now | 8 | 17 | 16 to 17 | 50 to 52 | 53 to 55 |

In a quieter room (16 to 29 % of the samples open on channels 37 and 38, 45 to 105 advertisers) no
sample was lost; in the office before, with the copy and the FIR on the unit, 5 to 10 %.

## The ring's decimation (2026-11-12, night)

With CPU 0 within its budget the front end came next. Taking every 4th pair of the 16 MS/s stream
folds the bands 4 MHz above and below the channel onto it, with their noise, and puts a signal 4
MHz away on top of the wanted one. The ring's boxcar (the mix and sums of 4, on the vector unit)
had been ruled out as too slow; it costs CPU 1 11.4 to 12.1 cycles a pair against 8.2 for every
4th pair, within the 15, but in the shortest units (1024 pairs, 64 us) 90 % of the units were
abandoned, in 2048 (128 us) about one a second, in 3072 (192 us, the default) none in a minute,
at 11.7 cycles a pair. The boxcar's passband is centred 4 MHz above
the LO, so the LO moved from 3 MHz above the channel to 5 MHz below it (taking every 4th pair, the
channel lands at +1 MHz from either side).

| | Today's ring (every 4th pair, LO +3) | Boxcar (LO -5, units of 2048; 3072 since) |
| --- | --- | --- |
| Noise floor, `ubt rx floor`, 2480 MHz, gain 60 | -63.6 / -64.4 dBFS a bin | -67.2 / -67.4 |
| The C6 at -15 dBm on channel 39, gain 27 (the edge), good advertisements in 10 s, four runs | 20, 13, 16, 9 | 73, 74, 62, 78 |
| Gain 30 | 104 to 116 | 108 |
| Busy channels 37 and 38, gain 60, good packets in 4 x 15 s | 9528 | 10658 |
| Following: events, missed; CONNECT_INDs caught | 737, 12.8 %; 15 of 24 | 764, 10.6 %; 16 of 24 |

Some 3 dB at the edge of sensitivity (the knee moved by about 3 gain steps; the low steps are not
quite 1 dB each), more packets on busy channels, following as before. `ubt rx sum off` restores the
earlier ring for comparison.

The simulator agrees (`tools/gfsk_sim.c`, its front end now with `SUM`, `LOOFF` and the app's 8 bit
ring, `RING8`): LE reaches a bit error rate of 1e-3 at 14.6 dB (Eb/N0) against 18.7 taking every 4th
pair, BR 1e-2 at 16.1 against 19.7. The LO's side alone changes nothing there (every 4th pair with
the LO 5 MHz below: 18.6 dB), so a mix by +4 MHz with the LO back above the channel would not add
anything worth the ring's change. The demodulator now sits some 2 to 2.5 dB from the simulator's
ideal receiver (floating point at 16 MS/s, genie timing: 1e-3 at about 12.7 dB), where it was 6
before; its parameters stay where they were (disc, win, dd_k, toff, nseed, seed_skip and el_shift
swept: none moves the 1e-3 point by more than the simulation's 0.3 dB). What is left would take
another structure (a filter matched to GFSK, say), and with it the bit for bit reference. (It
would not: "Where the last 2 dB go" below splits the gap.)

Bug found on the way: the sample ring's reader read the writer's position before the record of a
hole in the stream, and a hole recorded in between (the sink records one at the start of every
run: its first unit begins a little after pair 0) put the reader ahead of the writer. The next
read took that as a lap, a gap of some 2^32 samples: the stream's clock, and with it the follower's,
off by a thousand seconds, and once a follow that never retuned again. It came with the hundreds
of retunes of a follow; fixed, the follow runs above are after the fix.

Long runs then showed what short ones did not: over 5 minutes of sniffing on channel 37 (some 20 %
of the samples open) 0.23 % of the stream was lost in 953 gaps, two thirds of them found with the
squelch open: in long stretches of it (Wi-Fi keeps it open too) the full path, the finder and USB
together come to a little over the 60 cycles, and the 2 ms ring laps. Two changes halved it
(0.10 %, 599 gaps): the slicer's assembly now also runs before a burst is acquired, up to where
the C acquires it (the C slicer had cost some 2.6 cycles a sample of the open time, Wi-Fi and
noise moving the burst edge again and again), and an empty POLL waits up to 16 ms instead of 4
(USB from 7 to 4 % of CPU 0). The rest of the gaps (about one a second) were the ring's abandoned
units of 2048 pairs, gone in units of 3072.

A retune of the modes (tune request to the stream's first sample) took 8.8 ms, of which the
library's retune is 4.2 (its 3 ms of settling and a calibration): the radio thread retuned twice
(`esp_sdr_rx_set_gain()` retunes too, to apply the gain, which every retune applies already) and
the stop waited 1 ms for the ring before it asked it to stop. Now it sets the gain only when it
changes and asks first: 5.8 to 6.0 ms (stop 0.3 to 0.4, retune 4.2, the ring's start and the
first unit the rest), the slowest of a follow's hops 5.1 ms against 10. The events a follow misses
stayed at about 12 %, and it caught fewer CONNECT_INDs than sniffing; the next section has the
cause.

## The squelch's gate after a retune (2026-11-12, night)

Following caught fewer CONNECT_INDs than sniffing the same connections: 143 of 224 (64 %) over
the night against 37 of 40. The C6's advertisements right before a missed one were decoded, and
the finder never saw the PC's CONNECT_IND, not even with a bad CRC. Following with le_conn left
out (the same code, nothing followed) caught 14 of 16. The first connection of every run was
caught, and on the advertising channel nothing reset the finder or changed its configuration.

The cause was the demodulator's reset, which runs at every tune and every restart of the ring.
It zeroed the gate for the burst edge (`s_gate`) along with the stream's state, though
`gfsk_demod_set_squelch()` sets it. BR sets its squelch after every hop; the BLE modes set it once,
after their first tune. So from a follow's first retune on (and in promiscuous mode, or after a
restart of the ring) every group passed the gate:

- each burst's edge fell on the first group of the 40-sample replay, and its sampling phase and
  slicing levels were taken from the noise before it;
- a second burst while the squelch stayed open was not acquired anew.

Sniffing kept its first tune and was not affected. Now the reset restores the gate from the
threshold, in the frozen reference too, which had the same reset: the bench compare and the
differential tests stay identical, and `test_reset_keeps_squelch` checks that a stream after a
reset decodes as it does with the squelch set right after one. Interleaved, four pairs of
8 connections (gain 40, the C6 at -15 dBm on channel 39):

| | Gate zeroed (before) | Gate kept |
| --- | --- | --- |
| CONNECT_INDs caught | 25 of 32 | 29 of 32 |
| Connection events followed, missed | 1334, 9.7 % | 1531, 0.9 % |
| Followed connections whose end was missed (`timeout`) | 9 of 25 | 0 of 29 |

Two more runs with the clean image caught 15 of 16. The events they missed (4 %) came almost all
from three connections whose end the follower missed (the LL_TERMINATE_IND or the PHY update):
it then follows the connection to its supervision timeout.

## The burst edge in an open squelch (2026-11-12, morning)

The CONNECT_INDs still missed (about 9 %) matched sniffing's, and the C6's advertisements, 25 dB
weaker, were missed as often (10 to 13 %). To look for the demodulator's share,
`tools/gfsk_sim.c` got:

- pairs of bursts (`PAIR`: every second burst 150 us after the one before, `PAIRDB` stronger,
  with a carrier offset of its own within `PAIRCFO`);
- the squelch as the modes set it (`SQAUTO`: the floor tracked while always emitting over `LEAD`
  us of noise, plus the margin);
- packets counted as the finder takes them: the access address exactly in the emitted bits, then
  every payload bit. The time stamp comparison counts a sampling instant half a symbol late as
  errors, which the finder never sees.

At the board's levels at gain 40 (the C6's advertisements at about -31 dBFS from their RSSI, the
noise about 20 dB under them, near the 8 bit ring's step) the squelch opens on noise now and then:
the ring's steps make the noise lumpy. A burst that began while it was open was lost when two
groups of noise above the gate came right before it. The gate's run had then passed its second
group, the only one where the step up was looked at, and the burst was sliced with the phase and
levels left by the noise.

Now a group more than 4 times (6 dB) over the smoothed power before it starts the gate's run
over, so the next group decides on an edge as after a pause (`restep` in the reference, in
`groups_c()` and `gfsk_groups.S`; `test_step_up_in_open_squelch`). The simulator over six seeds,
LE at -31 dBFS behind the modes' squelch:

| | Before | Step up |
| --- | --- | --- |
| Single bursts, AA not found, 21 / 24 dB | 1.16 / 1.31 % | 0 / 0 |
| An advertisement and a CONNECT_IND 25 dB stronger, 21 dB: first, second lost | 6.67 %, 1.67 % | 0.05 %, 0 |
| The same at 24 dB | 6.82 %, 1.62 % | 0, 0 |
| Acquisitions a burst | 1.1 to 1.35 | 1.2 to 1.5 |

Nothing changes at 18 dB, where bit errors dominate, or above the ring's steps (-24 dBFS), or in
the curves always emitting (LE 1e-3 at 14.65 dB, BR at 20.58). The bench times it on the same
capture within a cycle (always 51 both, behind the squelch 10 to 11 both). On air, sniffing
interleaved (four pairs of 8 connections): CONNECT_INDs 29 and 28 of 32, the C6's advertisements
missed 10.0 and 10.2 %, good packets 12517 against 11996. The gain the simulator predicts there
(1 to 2 %) is below what 32 connections resolve. Following with it caught 15 of 16 and missed
0.6 % of the events, every end of a connection caught.

Memory: `CONFIG_ESP_SDR_RING_FIR=n` (the ring's FIR stage, unused here) and the BLE finder's
search loops back in flash freed 3.4 KB below the ring banks and 3.5 KB of the high region; the
mode loop's code and the vector and slicer kernels took most of it again. 1488 B are free below
the banks, 2.8 KB in the high region (the planes take 2.6 KB). A 32 KB instruction cache would need another
16 KB below the banks (the system heap is in use, 15.5 of its 17 KB at the most).

Without the Wi-Fi driver (`CONFIG_ESP_SDR_PHY_ONLY`, since 2026-11-13) there is room: the DRAM
image ends 30 KB lower (31000 B free below the banks against 928) and the flash code is 267 KB
smaller; the system heap peaks at 2212 bytes, so it shrank to 4 KB. Following at gain 40, three
rounds each, alternating, the stream lost 0.0007, 0.0040 and 0.0008 % without the driver against
0.0046, 0.0012 and 0.0006 % with it (0.0020 against 0.0021 % over all), caught 13 against 14 of
15 CONNECT_REQs and missed 13.7 against 18.4 % of the events; the noise floor (`ubt rx floor`,
-47 dBFS per bin at gain 60, -65 at 40) and the demodulator's cycles (`ubt rx bench`, 53 a sample)
came out the same.

The hot block's list stays the one made from the profiles with the driver. Made again from three
fresh profiles of the image without it (`scripts/place/hot_text.py`; CPU 0 idle 69 % of the time,
flash code 1.7 % of its samples, 56 functions in 12167 B), it lost more in each of three
alternating rounds: 0.0068, 0.0076 and 0.0019 % (9 to 27 gaps) against 0.0010, 0.0009 and
0.0011 % (5 to 7 gaps). The new list traded the USB driver's functions for the heap's: the losses
come in bursts, when the driver's code runs, and a profile of the average time underrates it.

A 32 KB instruction cache (8 ways of 4 KB; it takes 16 KB more below the banks, 27944 B stay free)
did not help either. Three alternating rounds each: 16 KB with the block lost 0.0030, 0 and
0.0003 % (0.0010 % over all, as in the six rounds before), 32 KB with the block (aligned to the
4 KB way by itself) 0.0005, 0.0025 and 0.0005 % (0.0011 %), 32 KB without it 0.0005, 0 and
0.0062 % (0.0021 %). The losses come as rare bursts, a run with one of them reading ten times a run
without, so three rounds tell a factor of two at best; the block still pays at 32 KB, the bigger
cache alone does not, and the 16 KB stay free.

## The ring reader's holes (2026-11-12, afternoon)

The ring's sink records each hole in the stream (the start of a run, an abandoned unit) before it
publishes the samples after it; the reader delivers what precedes a hole, then skips it. There was
one record: a second hole before the reader reached the first replaced it, and the first hole's
slots, the stream of one lap before (at the start of a run the previous channel's), went out as
samples. The records are a queue of 8 now (`src/ubt_sring.h`, a header so that the hot reader
inlines its common path); a reader more holes behind than that drops the stream up to the writer.
`ubt rx stats` counts since boot the holes skipped with a later one recorded already (where the
single record failed) and the times records were lost; in the runs below both stayed at 0.

A unit test (`tests/unit/ubertooth_core/src/sring.c`) runs the bookkeeping against a model ring
whose slots remember the index written to them, the sink and the reader interleaved at random over
400000 steps. It found an older flaw: the sink writes a call's samples (up to 80) before it
publishes them, after a hole far beyond the published position, and a copy of the oldest slots taken
meanwhile passed the check against that position. The sink now publishes first how far it is about
to write (`wmax`), and the reader checks against that. Each fix has a test that fails without it
(with the single record a sample of the lap before is taken; checked against the published
position, sample 36 holds 100).

The sink publishes with plain stores and the barriers they need: `atomic_set()` is a compare and
swap loop through `xtensa_cas()`, the PSRAM atomics emulation, some 30 cycles a call. The ring costs
11.82 cycles a pair against 11.85 before (12.05 with a second `atomic_set()` for `wmax`). Both images
abandoned 119 units in each 2 minute run, one a second, where the night before showed none in a
minute: not this change, and not explained yet.

The lap: the copy of `ubt_radio_rx_read8()` reaches 80 to 95 samples of history back from the read
position, and the old lap check left that out. A reader that crept into the last 95 samples before a
lap had its copy dropped and skipped to the writer, some 8100 samples, where a lap keeps three
quarters of the ring and loses some 2000. By its samples a lap, about a quarter of the committed
image's laps were such drops; the check now leaves room for the history.

Sniffing channel 37 for 2 minutes at a time, alternating with the committed image (A), the new
reader first lost six to seven times as much of the stream, with three to ten times the laps,
whichever way a lap went (B, C). The cause was the hole handling out of line in flash: 200 B that
moved the flash code after it, the USB stack among it, and with it what CPU 0's 16 KB instruction
cache holds. Inline in the reader's IRAM, the flash code where the committed image has it, the laps
were A's again (D). Laps here are the gaps less the ring's 119 abandoned units:

| Image | Runs | Stream lost | Laps a run | Samples a lap |
| --- | --- | --- | --- | --- |
| A, committed | 12 | 0.000 to 0.069 %, mean 0.024 | 1 to 83, mean 30 | 3900 |
| B, hole handling in flash, a lap keeps 3/4 of the ring | 4 | 0.13 to 0.36 %, mean 0.22 | 242 to 650, mean 408 | 2600 |
| C, in flash, a lap skips to the writer | 3 | 0.13 to 0.27 %, mean 0.20 | 74 to 156, mean 112 | 8400 |
| D, inline, skips to the writer | 3 | 0.002 to 0.093 %, mean 0.032 | 2 to 55, mean 20 | 8100 |
| E, inline, a lap keeps 3/4 (a quiet hour: 16 to 18 % open) | 3 | 0.000 to 0.002 %, mean 0.001 | 2 to 7, mean 5 | |

With the layout as before, a lap ends a short stretch of load: in the one pair with laps (D 55,
A 56) skipping to the writer lost 8100 samples a lap against A's 3200, so a lap keeps three
quarters of the ring again (E). E's runs fell into a quiet hour and lost as little as A's between
them. The queue and the hole handling take 264 B below the banks (1224 B free). The instruction
cache is a lever of its own: 200 B of flash code moved made CPU 0 lap several times as often.

## Where the last 2 dB go (2026-11-12, afternoon)

The simulator's genie (floating point at 16 MS/s before the analog filter, a Gaussian filter, the
phase steps summed over each symbol, the exact carrier offset and symbol timing) got a twin on the
ring's 4 MS/s stream (`REF4=1`) whose filter, discriminator and timing switch to the demodulator's
one at a time (`REF4F`, `REF4D`, `REF4T`). On the same streams, the SNR at a bit error rate of 1e-3
(LE) and 1e-2 (BR), 1.5 million bits a point:

| Receiver | LE | BR |
| --- | --- | --- |
| Genie at 16 MS/s | 12.89 dB | 13.30 dB |
| Genie on the 4 MS/s ring stream | 13.87 | 14.20 |
| ... with the demodulator's filter (4 taps after the derotation) | 14.32 | |
| ... and its discriminator (Im(z conj z[n-2]) over 2 samples) | 13.85 | 14.27 |
| ... and the nearest of 4 sampling instants a symbol | 14.25 | 14.69 |
| The demodulator, behind the modes' squelch | 14.65 | 15.13 |

- The front end, 1 dB: the 4 MS/s stream. The boxcar's nulls are points, so the noise of the bands
  around 4 MHz still folds in partly; the 8 bit ring is 0.07 dB of it (`RING8=0`). A better
  decimation filter would have to fit CPU 1's 3 cycles a pair left.
- The structure, nothing: the 4 taps and the cross product, which weighs each phase step by the
  power, do as well as a Gaussian filter and the phase. A demodulator structure matched to GFSK
  would not gain anything, nor be worth losing the bit for bit reference.
- The timing, 0.4 dB: the sampling instant up to an eighth of a symbol off. The genie's exact rows
  interpolate linearly between the samples; the demodulator would need the fraction from the
  preamble.
- The estimation (the sampling phase chosen, the levels from the preamble), 0.4 dB in LE and 0.45
  in BR.

The simulator's BR bursts had the preamble's last bit equal to the first sync bit, where it
alternates into the sync word; that cost the demodulator 1 dB it does not lose on air. Found the
same day: the first version of this section had BR losing 1.6 dB in its estimation, 0.85 of it to
when the acquisition starts (`seed_skip`), which with the bursts right makes no difference. BR
figures in the sections before this one are about 1 dB pessimistic for the same reason.
`test_br_noise_behind_squelch` covers BR in noise behind the squelch.

Always emitting, the demodulator acquires few bursts at low SNR (0.06 a burst at 12 dB, 0.26 at
13): the only edge there is a 6 dB step. Its curve falls off sooner (LE 1e-2 at 13.27 dB against
11.47 behind the squelch); above 14 dB both agree.

## The ring sink's pack on the vector unit (2026-11-12, evening)

The sink converted the ring's int16 output to the sample ring's 8 bits in a scalar loop: 15
instructions a sample, with 4 pairs a sample some 3.75 of CPU 1's 11.8 cycles a pair. On the vector
unit (`src/ubt_pack8.S`: unaligned loads of the source, >> 6 as a multiply by 1 with SAR 6, the
clamp at 127, the low bytes by `ee.vunzip.8`; the head up to the ring slot's 16 byte boundary and
the tail scalar) the ring costs 8.7 to 8.8 cycles a pair, and the units it abandoned once a second
(119 in each 2 minute sniff, the committed image too) are gone: 0, 0 and 1 in three sniffs
alternating with the committed image, which abandoned 119 in each. A check at boot
(`src/ubt_pack.c`) runs the kernel against C over 1600 cases (every alignment of the ring slot and
of the source, lengths 0 to 99, the int16 range and the stream's); the sink falls back to C if one
differs.

None of it is flash code: the check runs from IRAM, and `ubt_radio.c`'s `pack()` keeps its C loop,
starting where the vector unit stopped. The image's flash code is the committed one's byte for
byte (every flash function's address and size), since code added in flash moved the flash code
after it (the section above). Even with the instructions unchanged it moved 4 bytes: without
`pack()`'s loop, GCC's debug scopes of the code inlined into `ring_thread_fn()` came out otherwise,
and the assembler aligned a branch target in it differently. The price is IRAM: 504 B are free below
the banks.

The rest of CPU 1's budget: the -fs/4 rotation and the sums of 4 are one bin of a 4 point DFT
(I = (i0 - i2) + (q1 - q3), Q = (q0 - q2) - (i1 - i3)), done in two passes of the ring library
(`rot_fs4`, then `sum4`), whose fusion would save some 1.5 cycles a pair more. The headroom could
take a longer decimation filter, for part of the front end's 1 dB.

## The channel at DC (2026-11-12, night)

With the channel at +1 MHz after the mix, decimating by 4 folded the bands 3 and 5 MHz off onto it,
where the sums of 4 reach only -10 and -13 dB: their nulls lie at 4 and 8 MHz. `tools/gfsk_sim.c` got
a switchable decimation (`DECIM`: `box`, `cic2` and `cic3` the sums convolved with themselves, `lpN`
an N tap low pass) and more 8 bit ring modes (`RING8`: 2 rounded, 3 the floor read at the middle of
its step). LE at a bit error rate of 1e-3, behind the modes' squelch:

| Decimation | The demodulator | The genie at 4 MS/s |
| --- | --- | --- |
| The sums of 4, the channel at +1 MHz | 14.62 dB | 13.89 dB |
| Sums of second and third order | 14.86, 15.89 | 13.30, 13.40 |
| A 24 tap low pass | 13.75 | 13.13 |
| The sums of 4, the channel at DC | 13.61 | 13.00 |

The higher order sums lose for the demodulator: their passband falls 2 to 3 dB across a channel at
+1 MHz, the tones come out unequal, and its discriminator weighs each phase step by the power. A
flat low pass gains about as much, for 2 to 3 of CPU 1's cycles a pair; with the LO 4 MHz below
the channel the aliases fall onto the sums' nulls for nothing. So the channel sits at DC now
(`UBT_RX_LO_OFFSET_MHZ`) and the demodulator's filter is the plain sum of 4, in the reference, its C
and the vector kernels alike, the discriminator's sign as before (the levels and the carrier offset
estimate stay); the unit tests' generator puts its bursts at DC. In the simulator, behind the
squelch: LE reaches 1e-3 at 13.61 dB instead of 14.62 (at the board's -31 dBFS 14.01 instead of
15.00), BR 1e-2 at 14.33 instead of 15.11; with a carrier offset of +100 kHz LE reaches 1e-3 at
16.7 dB, before not by 17, with -100 kHz 0.3 dB later.

The demodulator reads each 8 bit sample at the middle of the floor's step that stored it, x + 1/2
(in the sum of 4 a constant 2; the reference gets (x << 6) + 32): the floor alone left half a step
of DC in the stream (the mean -27 of 8192, a spur 12 dB over the floor at 0 Hz), harmless 1 MHz off
the channel, on it the first try's undoing (below). Rounding in the sink removed the bias too, but
gave the ring a zero level: on a channel at DC the noise between packets lies below a step even
at gain 40 (the stream's noise is not white: at DC some 6 dB under the bands 1 to 2 MHz off),
rounding left it all zeros, the modes' floor (the 2nd percentile of the symbols' power) read pwr 7
and the squelch never closed. Read at the middle of its step no sample is zero: the floor reads pwr
99 at gains 40 and 60 (the channel at +1 MHz 97). Every 4th pair (`ubt rx sum off`) is gone, as it
would fold the LO's leakage onto a channel at DC; `ubt rx ring` keeps the unit and LO overrides.

On air, the C6 far from the S3 (the PC's adapter heard it at -84 to -89 dBm), its good
advertisements on channel 39 in 10 s at the modes' gain of 60. Alternating whole images every
three minutes, the far link's fading over minutes (rounds of 6 to 18 and of 90 to 100) left it
open (the channel at DC ahead in 7 of 10 rounds with the sink rounding, behind in 2 of 3 read at
mid-step). One test image with both, switched between runs (`ubt rx dc on|off`, the +1 MHz FIR in
C; not kept), alternating every 10 s, the order flipped every pair:

| 20 pairs of 10 s runs | The channel at DC | At +1 MHz |
| --- | --- | --- |
| Good advertisements | 1829 | 1303 |
| Runs ahead | 19 of 20 | 1 |

40 % more, 26 a run (t = 5.9): the simulator's 1 dB at a knee this steep. Close to the S3, the edge
reached by lowering the gain instead, it did not show: at gains 27 and 30 the two tied (105 against
104, 131 against 128, the sink rounding), at 24 the channel at DC lost (9 against 22, one round).
There the receiver's noise lies below a step of the 8 bit ring (rounded, the floor falls 6.5 dB at
gain 24): a signal a step or two above zero, which at DC turns slowly through its steps, a regime
the simulator, whose noise always spans a step, models poorly. The modes run at 40 to 60.

Strong signals pay for it, in the simulator. At a quarter of the ADC's rate the products of its
clipping (I and Q limited apart: the phase times -3, 5, -7 and so on) land on the channel itself,
where the sum of 4 takes the strongest, the deviation mirrored and tripled, only 10 dB down; at +1
MHz they reached it through the ring's sums at 3 and 5 MHz, 10 to 13 dB further down. A packet
driving the ADC 6 dB past full scale comes through either way (30 dB SNR, behind the squelch); 8,
10 and 12 dB past it the channel at DC loses 5, 19 and 27 % of them (right behind a weak packet 1,
52 and 92 %), +1 MHz none. On the board the PC's adapter reads 18 to 22 dB below full scale at
gain 40, so near full scale at 60, 21 steps of about 1 dB up (the RSSI reads -65 to -68).

Following, the C6 far, alternating the images (two rounds of five connections at each gain, the
order flipped in the second):

| Ten connections each | +1 MHz, gain 40 | DC, gain 40 | +1 MHz, gain 60 | DC, gain 60 |
| --- | --- | --- | --- | --- |
| CONNECT_INDs followed | 10 | 9 | 8 | 7 |
| Events missed | 29 % | 33 % | 19 % | 14 % |
| The PC's packets on channel 39 with a bad CRC | 0 of 200 | 3 of 223 | 5 of 207 | 7 of 161 |
| The C6's advertisements decoded | 0 | 59 | 640 | 904 |
| Stream lost | 0.004 % | 0.052 % | 0.005 % | 0.014 % |

With the C6 far both miss a fifth to a third of the events (close by, 1 %). At gain 40 the channel
at DC heard the C6's advertisements, +1 MHz none. The PC's packets fail somewhat more often at DC,
too few to tell apart (another run at gain 60 lost 5 of 39).

The clip guard (`ubt rx gain guard`) holds twice as long each time it has to step down again
within a minute of stepping up, from 10 s up to 160 s, for sources that come in bursts. The board
did not put that to the test: sniffing channel 39 under the guard while the PC scanned 3 s in every
40, advertisers near the S3 (random addresses, printed -41 to -50 dBm, 16 to 25 dB past full
scale at gain 60) took either version down to the last level (gain 36) within seconds and held it
there while they did not pause for 10 s. The old one, given two such pauses, climbed and came back
down twice (7 changes and 225 packets at full scale in 4 minutes, against 3 and 12). Such sources
are the ones the channel at DC takes worst: at gain 60, without the guard, they reach the ADC that
far past full scale.

CPU 0 on a busy channel. Following, every run with the channel at DC lost more of the stream
than every run at +1 MHz, 3 to 12 times as much (at most 0.06 %); sniffing channel 37 at gain 40
for 60 s, 0.39, 0.20 and 0.12 % against 0.00, 0.00 and 0.17 %, with the squelch open 12, 9 and 8 %
of the time against 4, 5 and 8 %. The demodulator's mean load is the same (15 to 16 cycles a sample
at gain 40). Two suspects: more bursts open the squelch (on channel 37 it decoded 5815, 5210 and
4561 packets against 4911, 4574 and 4871), and the flash code moved, most of it 148 B down from
6f80572's (`ubt rx ring` and its literals, `ring_thread_fn()` without the every-4th branch, the
int16 reader without its inlined `unpack()`): the instruction cache layout that the section on the
ring reader's holes found to decide such losses. It was the layout (next section).

## The instruction cache (2026-11-13)

To tell the layout from the code, 477b629 was padded back to 6f80572's flash layout (not kept):
every flash function but the changed ones and every constant at 6f80572's address, `zp_fill_c()`
in IRAM and its slot taken by `acquire()`, which it follows, with the literal the linker had shared
between them, `cmd_rx_ring()` grown to `cmd_rx_sum()`'s size, a help text 7 characters shorter for
the string pool. Following at gain 40, the three images rotating over three rounds, twice (the
second time with the cache's counters read every 5 s, which costs CPU 0 some flash code of its
own):

| Stream lost | 6f80572 | 477b629 | 477b629 on 6f80572's layout |
| --- | --- | --- | --- |
| Three rounds | 0.0023 % | 0.045 % | 0.0087 % |
| Three rounds, counters read | 0.013 % | 0.085 % | 0.0055 % |

The same code lost 5 to 15 times less on the other layout, and the demodulator's mean cost followed
the layout too (14 to 16 cycles a sample against 12 to 13): its loop runs from IRAM, code it calls
from flash. The cache's counters (`EXTMEM_IBUS_ACS_CNT` and `EXTMEM_IBUS_ACS_MISS_CNT` at
0x600C40CC and 0x600C40C8, their window at 0x600C40B4 and 0x600C40B8 set to the flash text, read
with `devmem`) read alike most of the time, some 8000 misses a second, but the moved layout once
showed 6.4 million in one second: the stream goes in bursts, when CPU 0's flash code thrashes.

CPU 0 under load (`ubt prof`, 131072 samples each of following a connection, of scanning and
connecting, of sniffing channel 37; at one sample a sink call, up to 80 samples, a capture spanned
about 2 s, not the 8.4 s first written here): idle 43 to 66 % of the time, the mode thread 28 to 55
%, the USB threads 1.5 %. From flash it ran 84 functions, 21 KB, more than the cache's 16 KB: the
USB device stack (the DWC2 driver 5.7 KB, usbd's control path 2.7 KB, net_buf and sys_heap 1.9 KB),
the mode loop `le_run()` (1.9 KB, the most samples), `acquire()`, `find_gap()`, the CRC table walk,
`ubt_ctrl()`.
The library's sampler that replaced `ubt prof` checks a fixed period in the ring's poll loop and
before every sink call instead: sniffing channel 37 it took 131072 samples at 64 us over 8.8 s,
none missed, 3 gaps over 96 us (the longest 112 us), and its shares agree with the old captures'
(`closed_scan()` 8 %, `le_run()` 1 %, the vector kernels alike), so the bursts did not bias them.

So that code has a place of its own now. The ESP32-S3 linker script takes TEXT_SECTIONS snippets
ahead of its catch-all (a line in the Zephyr fork, as it takes RAMFUNC_SECTION ones in the IRAM
text), and `apps/ubertooth/hot_text_s3.ld` claims the profiles' functions behind `ALIGN(2048)`, the
cache's way, densest first (samples per byte); the linker pools their literals at the block's start.
Its lines keep their sets whatever comes before or after: built with and without an unrelated change
(the clip guard's, 144 B of flash code), the block's functions kept their addresses and sizes. The
block is lib/esp_sdr's since (`CONFIG_ESP_SDR_HOT_TEXT`, `esp_sdr_hot_text()`; `hot_text_s3.ld`
holds the list): `scripts/place/hot_text.py` makes the list from `prof dump` captures; a function
renamed falls back to the catch-all, and the link stops if the block outgrows its budget
(`CONFIG_ESP_SDR_HOT_TEXT_BUDGET`, 12 KB). Following at gain 40, three rounds each, the block in the
working tree (the clip guard's change and the pack check's below) and in 477b629 alone:

| Stream lost | Round 1 | Round 2 | Round 3 | All |
| --- | --- | --- | --- | --- |
| 6f80572 | 0.017 % | 0.0004 % | 0.0043 % | 0.0076 % |
| 477b629 | 0.054 % | 0.061 % | 0.071 % | 0.062 % |
| Block of 16 KB (69 functions) | 0.0004 % | 0.0009 % | 0.0005 % | 0.0006 % |
| The same in 477b629 | 0.0043 % | 0.0078 % | 0.0031 % | 0.0052 % |
| Block of 12 KB (66 functions) | 0.0005 % | 0.0004 % | 0.0011 % | 0.0007 % |
| The same in 477b629 | 0.0049 % | 0.0004 % | 0.0016 % | 0.0025 % |

A tenth to a hundredth of 477b629's loss, and less than 6f80572's. At 16 KB the block filled nearly
all 8 ways of every set and the code outside it still made a factor of 9; at 12 KB (6 ways, 96 % of
the flash samples) a factor of 3.5, so the block stays at 12 KB. Flash code elsewhere no longer
moves CPU 0's hot code, so the pack check at boot runs from flash now (`ubt_pack.c`, its fallback
to C kept): 928 B free below the banks.

## Status

Every mode the radio can do runs on a board, connection following included (1M PHY, as an
Ubertooth One). The demodulator's full path fits CPU 0's budget (about 53 of 60 cycles a sample
open, 10 closed), the ring's boxcar CPU 1's (9 of 15 cycles a pair); following catches as many
CONNECT_INDs as sniffing (about 90 %) and misses about 1 % of a connection's events (the C6 close
by). With the channel at DC the demodulator sits 0.6 dB from the simulator's genie (LE), its timing
and estimation, not its structure. CPU 0's hot flash code sits in a block of its own at a fixed
instruction cache position, and the stream it loses no longer follows the rest of the layout. Not
done: the demodulator's timing and estimation, a gain control for nearby transmitters that is
measured working (the clip guard, `ubt rx gain guard`, holds longer for sources in bursts now; the
channel at DC needs it more), the hot block's list from the BR and promiscuous modes, RSSI and
spectrum level calibration with a source of known power.
