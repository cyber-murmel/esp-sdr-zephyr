# pcap internals

The design and measurement log of [pcap](../../apps/pcap/README.rst): the WHY2025 badge's ESP32-P4
demodulates and decodes what its ESP32-C6 radio receives and streams it over USB as pcapng for
Wireshark. Design note, 2026-10-26; implemented and measured on the badge on 2026-10-27.

The README has the usage, the modes and the output formats; this page follows it in
[Applications](index.md).

The signal chain is told in the guides:

- the C6's gapless 80 MS/s receive, pruned to the bursts, in [Gapless receive](../samples/rings.md);
- the channel mode's paced receive in [Paced streams](../samples/paced.md);
- the detectors, decoders and the generic analysis in [Burst detection](../modem/bursts.md).

This page has the P4's pipeline, the follow mode, USB, the measurements on the badge, and the same
engine on MicroPython boards.

## Goal and scope

### In scope

- Bursts in the 2.4 GHz band as the badge receives them.
- Best-effort demodulation and decoding on the P4.
- pcapng written by the firmware.
- A Wireshark extcap that only copies the stream and sets the badge up.
- A Lua dissector for the burst record.

### Out of scope

- Hopping beyond the survey's span (80 MHz, or 40 at 40 MS/s).
- Following connections across hops.
- LE Coded.
- Bluetooth BR/EDR decoding: its bursts come as burst records.
- Transmit.

## The P4 pipeline

The P4 has two cores at 360 MHz. The pipeline's threads and their CPUs:

```
C6 --SDIO--> sdio_reader (2, CPU 0) --blocks--> stream thread (5, CPU 1)
                                          survey: esdr_pfbq -> esdr_detq -> assembler
                                          channel: power detector (narrow.c)
                                              |  closed bursts (PSRAM pool)
                                              v
                                          decoder (8, CPU 0): BLE, 802.15.4, wideband
                                              |  unclaimed bursts      \
                                              v                         v
                                          generic (14, CPU 0)      packets, metadata
                                              |                         |
                                              v                         v
                                          pcapng (output.c) -> USB bulk IN (7, CPU 0)
host: tools/esdr_pcap.py (extcap) sets up, starts, copies; Wireshark, tools/esdr_burst.lua
```

### Threads and cores

- The WHY2025 badge (`boards/why2025_badge_esp32p4_hpcore.conf`) runs Zephyr SMP with pin-only
  scheduling, so a thread nobody pins stays on CPU 0.
- CPU 1 runs the stream thread and, at the lowest priority, a second generic analysis worker that
  takes the time the stream leaves (`CONFIG_APP_PCAP_GEN_WORKERS`).
- CPU 0 runs the reader, USB, the decoder, the generic analysis, the control queue, the shell and
  logging, and takes every interrupt and the tick (`CONFIG_APP_PCAP_STREAM_CPU`,
  `CONFIG_APP_PCAP_DECODE_CPU`).
- The other way round, with the decoders on CPU 1, the reader (which preempts the stream) and the
  stream overloaded CPU 0 at 40 MS/s: radio blocks dropped, USB starved, a third fewer Bluetooth LE
  packets ([Both cores](#both-cores-2026-10-27)).

#### PIE and pinning

- The stream thread is created suspended and pinned before it starts.
- PIE is switched on per core, and its vector, accumulator and loop registers are not saved on a
  context switch. A PIE thread that moved would trap on the other core or carry on with its
  registers; the rule is in [Chip notes](../dev/chips.md).

#### Shared cache and cycle counters

- The cores share the L1 data cache: bursts change cores without cache maintenance.
- `mcycle` is per core, so the cycle statistics come from pinned threads.
- The EV board stays on one core. MicroPython on the badge runs both
  ([ESP32-P4: two CPUs](#esp32-p4-two-cpus)).

### Burst pool and access addresses

#### The pool

- `CONFIG_APP_PCAP_BURSTS` buffers of 128 KiB in PSRAM: 128 on the badge (16 MiB), 32 on the EV
  board.
- The survey's 40 channels hold one each while open, and the channelizer can keep the decoder
  waiting for some 12 ms, during which a busy band opens a few dozen bursts.
- 48 buffers lost a quarter of the survey's bursts; 128 lose about 1 %.

#### Access addresses

The Bluetooth LE access addresses the decoder looks for:

- the advertising one;
- the host's (`PCAP_REQ_ADD_AA`);
- those of CONNECT_IND PDUs seen, with their CRCInit, the four newest.

So connections set up in view are followed while their hops land in view.

## Survey at 40 MS/s

The C6's side of the 40 MS/s survey (2026-10-27: spare staging, admission, the rate bit and the
31 MHz low-pass, [Filters](../radio/filters.md)) is in [Gapless receive](../samples/rings.md). On
the P4:

- The P4 runs half the channels (M = 20 for 40, [Burst detection](../modem/bursts.md)), so that
  they keep their 2 MHz spacing and 4 MS/s.
- 16 channels would be M = 8, which the bank does not have: the start is refused.
- The survey decoded Bluetooth LE for the first time: 139 to 241 packets in 15 to 20 s, every CRC
  good, against none at 80 MS/s.
- 802.15.4 data frames (over a millisecond) still arrive cut.
- The P4 is the limit there: the C6 keeps twice the pairs (2.2 M a second), the filterbank takes
  some 100 cycles a pair (62 % of the CPU), and 15 % of the bursts find no buffer.

## Following 802.15.4

The follow mode (`PCAP_MODE_F_FOLLOW`, `src/follow.c`, the survey's decoder in `src/decoder.c`)
takes the capture to the channels where the survey sights 802.15.4 preambles. How preamble symbols
make a sighting, and how often noise makes one, is in [Burst detection](../modem/bursts.md).

### Stays

- The capture starts in the survey.
- After `survey_ms` at least (a sighting wakes the control thread) it goes to the channel with the
  most sightings, the newest among equals.
- A channel other than the last one followed goes first, so several networks take turns.
- A stay ends `channel_ms` after the last burst that showed a preamble, or at 4 times `channel_ms`
  however busy the channel: the survey's turn.
- Both times default to a second.
- Bursts of the other mode still queued after a switch do not count.

#### Requirements

- It needs the 802.15.4 decoder and 40 channels: `PCAP_REQ_START` fails otherwise.
- It tunes by the survey it runs: +-40 MHz around the centre, the whole of 802.15.4 with the
  default 2440 MHz, or +-20 MHz at 40 MS/s.

### Measurements, 2026-11-04

On the badge, in the office, at 80 MS/s unless noted, the channel mode's rate 2 MS/s:

| Run | Result |
| --- | --- |
| No sender of ours, 30 s | 150 frames from the three networks in range (channels 11, 15, 25), where the plain survey decodes none; every stay brought frames |
| The S3 devkit sending 40 frames a second at -20 dBm, 300 on each of channels 15, 20, 12 and 22 in turn, default times, 45 s | 565 frames, 295 of them the S3's; channels 12 and 22 reached after 6 and 5 s, channel 20 missed altogether, while the networks in range had their turns |
| A stay that outlasted the S3 (the limit at 8 s then) | 291 of its 300 frames decoded |
| A sender right beside the badge, at 0 dBm | one frame in 50 made a whole sighting, three in 50 a half one |
| 180 s of quick switches (100 ms each) | 313 stays, no radio block dropped, no burst lost, the S3 on channel 20 decoded 3958 times |

- What is on the air competes for the same time: `channel_ms` trades a stay's length for how soon
  the next channel is seen.
- A sender beside the badge is harder: at 0 dBm the S3 opens six or more adjacent channels and goes
  out as a wideband record, which has no samples to look at.

## USB

- A vendor interface (class 0xFF) with one bulk IN endpoint, 512-byte packets at high speed
  (`src/usb_pcap.c`).
- The CDC-ACM shell and DFU of `apps/common/src/app_usb.c`.
- The USB IDs and product string are in [pcap](../../apps/pcap/README.rst). How the P4's FIFOs are
  sized so that two bulk IN endpoints come up is in [Host transports](../samples/transports.md).
- The image carries the repository's `VERSION`, as MicroPython does: MCUboot refuses a downgrade,
  so neither may fall behind the other.

### Vendor requests

- `esdr_proto.h`'s requests below 0x40 go to the C6. Frequency and gapless settings are kept and
  applied at the start; `ESDR_REQ_SET_CHANNELIZER` sets the bank.
- The app's own start at 0x40 (`src/pcap_proto.h`): `START`, `STOP`, `SET_TIME`, `SET_DECODERS`,
  `GET_STATS`, `ADD_AA`, `SET_MODE`.
- The host leaving (interface disabled) stops the capture.

## Measurements on the badge

The first build, then both cores, then a second analysis worker.

### The first build, 2026-10-26 and 27

Built on the badge (P4 v1.0 at 360 MHz, C6 running osmosdr_serial), measured with the host tool
and tshark, 15 s each.

#### Advertising channel 37

Channel mode, 2402 MHz, automatic gain:

- 2346 Bluetooth LE packets, every CRC good.
- 0 of 4568 bursts lost.
- 1015 bursts analysed, 1229 filling the channel.
- Before the paced receive's fix: 313 and 334 packets from some 25 advertisers.

#### 802.15.4 channel 25

Channel mode, 2475 MHz, an S3 on the desk sending 400 test frames 20 ms apart:

- 393 decoded.
- 158 frames of a Zigbee network in range: ACKs, data requests, ZigBee commands.
- tshark dissects them through the TAP header.

#### Survey at 2440 MHz

- 18 552 bursts, 220 lost (1.2 %).
- 6785 wideband, 8852 analysed, 3635 recorded unanalysed.
- 83 to 91 cycles a pair in the filterbank.
- No Bluetooth LE packet whole.

#### 40 MS/s survey

2440 MHz, the PHY's 20 MHz mode, 31 MHz low-pass, 20 s:

- 241 Bluetooth LE packets, every CRC good.
- 28 887 bursts, 4303 lost (15 %).
- 102 cycles a pair in the filterbank (M = 20), at twice the 80 MS/s survey's pairs.
- The PIE kernels give the C ones' outputs bit for bit at every M, 20 included (run on the badge).

### Both cores, 2026-10-27

Zephyr SMP; an S3 sending 802.15.4 frames on channel 25, 30 s per run, two runs of each image
alternated, the same sources and runtime statistics in each:

| survey | image | bursts | lost | BLE, CRC good | analysed | radio blocks dropped |
|---|---|---|---|---|---|---|
| 40 MS/s | one core | 32 560, 34 947 | 6325, 8008 | 314, 295 | 1303, 890 | 0 |
| 40 MS/s | decoders on CPU 1 | 46 891, 46 846 | 1085, 68 | 215, 205 | 17 216, 16 766 | 1880, 1796 |
| 40 MS/s | stream on CPU 1 | 39 935, 45 094 | 16, 0 | 353, 325 | 7170, 7307 | 0 |
| 80 MS/s | one core | 16 400, 26 699 | 43, 215 | 0 | 9721, 12 863 | 0 |
| 80 MS/s | stream on CPU 1 | 32 789, 33 504 | 0, 0 | 0 | 12 716, 13 428 | 0 |

#### Stream on CPU 1

- With the stream thread alone on CPU 1 the filterbank costs 78 cycles a pair (wall clock; on one
  core the reader's preemption made it 98 to 101).
- That leaves CPU 1 46 to 51 % idle at 40 MS/s, 77 % at 80 MS/s.
- CPU 0 carries the reader (29 %), the decoder (23 to 25 %) and the generic analysis in the rest.

#### Decoders on CPU 1

- CPU 0 had the stream (66 %) and the reader (28 %).
- The reader's preemption and the generic analysis' traffic through the shared L1 data cache took
  the filterbank to 141 to 145 cycles a pair (without the analysis 128).
- 5 % of the radio blocks were dropped and the USB threads starved (output records dropped): fewer
  packets decoded than on one core.

### A second analysis worker, 2026-11-03

A second generic analysis worker on CPU 1: the same overlay, ambient traffic, 30 s runs, one and
two workers alternated, at 40 MS/s:

| | One worker | Two workers |
| --- | --- | --- |
| Bursts analysed | 5874 and 6356 | 13 827 and 14 067 |
| Skipped | 35 105 and 27 950 | 22 365 and 22 311 |
| Filterbank, cycles a pair | 81 to 83 | 83 to 85 |
| CPU 1 idle | 46 to 58 % | 4 to 7 % |

- No burst was lost and no radio block dropped either way.
- At 80 MS/s one worker already analysed nearly everything: 655 to 2033 skipped, 213 to 1070 with
  two.

## On the board's own radio

`apps/micropython/eggs/pcap` (the Pcap section of [micropython](../../apps/micropython/README.rst))
runs the channel mode on a MicroPython board's own radio. It shows the packets in a table with
filters, groups and sorts; Save writes them as pcapng with this app's interfaces and records.

### The capture core

- `apps/micropython/cmod/esdr_pcap_core.c` is `src/narrow.c`'s detector on int16 samples and
  `src/decoder.c`'s order of decoders and gates for one channel.
- It runs at once on each closed burst, with no threads, queues or pool: the caller feeds samples
  and collects the call's packets.
- The decoders, `src/records.c` and `src/pcapng.c` are this app's own files, built into the
  MicroPython image.
- The generic analysis takes at most two bursts a feed, none while the capture is behind. The
  others go out as records of their metadata (`PCAP_BF_SKIPPED`), a band filling the channel as a
  wideband record, as in this app.

#### Boards with the engine

- The engine (below) runs on the S3 in multiapp (the XIAO, the Communicator), the S31 (the VIEWE
  board) and the P4 (the WHY2025 badge).
- Elsewhere the radio receives into a ring in the heap (`esdr.rx_start(wrap=True)`), and each pass
  of the display loop detects and decodes what came (`esdr.pcap_feed()`).
- The standalone S3 image has no room for the ring's decimation by 4, and no Pcap app.

### ESP32-S3: the ring stage

#### The first version

- The S3 has no filtered 2 MS/s: it receives every fourth pair of its 16 MS/s ring, two of those
  averaged (`esdr.PACKET_RATES`; multiapp only).
- Its first version read the samples from a ring in the MicroPython heap at every pass of the
  display loop, and the S3 kept only part of the stream.
- The ring lost 8 to 20 % of its input whenever the other core ran at all: the two cores share the
  cache in front of flash and PSRAM, and the ring's sink wrote PSRAM. Idle it lost none at 2 MS/s,
  and an eighth at the undivided 4 MS/s.
- The detector cost some 90 cycles a sample reading that ring, and with the display drawing the
  loop read about a tenth of the stream.

#### The engine

So on the S3 it runs as in this app, in threads (`esdr.pcap_start()`):

- The ring's sink writes SRAM bank 2 from IRAM, so that the radio's CPU touches no flash or PSRAM.
  That is `lib/esp_sdr`'s ring stage, in [Gapless receive](../samples/rings.md).
- A thread on CPU 0 above MicroPython runs the detector there and closes the bursts into a pool:
  5 ms buffers in PSRAM, 48, more where the heap leaves 6 MB besides, 128 at most, fewer where it
  has no room; a queue each way, one thread at each end.
- A second thread at MicroPython's priority decodes them into a ring of packets in the state
  (128 KiB), which the display loop collects (`esdr.pcap_take_pkts()`).
- Each packet is made a `pcap_view.Pkt` in C, dissected, its attributes in one allocation. Added
  one at a time from Python they grew the object's table eight times, each regrowth a search of a
  fragmented heap, 600 us a packet on the XIAO.

#### Keeping up

- The decoder thread yields after every burst. The time slices did not take the CPU from it, and
  with the pool full a take that made room left it decoding until the ring was full again.
- A burst no decoder takes goes over as its numbers alone when it fills the channel (a wideband
  record) or while half the pool is in use, and its buffer stays free.
- The generic analysis takes four of the others a second at most, some 15 ms each on the S3.
- While the ring of packets is half full, such records of numbers alone stay out of it (`shed` in
  `pcap_stats()`). They were most of what filled it, and a full ring stopped the decoders until the
  pool overflowed.
- The ring fills because the display loop takes packets slower than a busy band brings them: some
  650 records a second, 0.3 ms each on the XIAO.
- A pass of the loop takes packets for 150 ms at most.
- The detector reads a sample as one word, shifted to full scale in place: 35 cycles a sample,
  against 43 for an int16 loop with a shift pass and `memcpy()` calls.

### ESP32-S31: CIC on the dump ring

The S31 (the VIEWE board) has the same engine on its dump ring: a gapless 2 MS/s stream decimated
into the ring stage in the capture bank, from IRAM. The decimator, the choice of memory, the LO's
side and the VIEWE's counts are in [Gapless receive](../samples/rings.md).

### ESP32-P4: two CPUs

The P4 (the WHY2025 badge) has the engine too:

- The link's parser fills the receive ring in the heap (`esdr.pcap_start(state, ring)`,
  `esdr.PCAP_RING_MIN` samples at least), with a hole log and a published count as the stage has
  (`esdr_remote_rx_peek()`).
- The detector reads it in place at priority 9, the decoders below it.
- Fed from the display loop, the detector saw 68 % of the stream (the loop fell behind its ring 9
  times in 15 s); with the engine all of it.

#### Two CPUs

- On one CPU the two threads took some 74 % of it in a busy band, and a busy moment overflowed the
  pool.
- The badge's MicroPython image now runs both CPUs (SMP, pin-only, as apps/pcap): the detector and
  the decoders on CPU 1; MicroPython, the link's reader and parser and USB on CPU 0.
- The parser orders its stores of a block after the count it published last, and the detector its
  reads before its look at the count. So a block the parser is storing on the other CPU stays
  within the reader's slack.
- The gapless channelizer (`esdr_gl.c`, PIE) runs on CPU 1 as well, switching PIE on there itself:
  68 to 73 cycles a pair, nothing dropped at 8 MB/s kept.

#### One CPU against two

Alternated runs of 15 s, the same hour, ambient traffic:

| | One CPU | Two CPUs |
| --- | --- | --- |
| Bluetooth LE packets | 778 to 962 | 727 to 1221 |
| Bursts without a buffer | 0 to 1652 | none in 7 of 8 runs |
| CPU time of the threads | 8.3 to 10.1 s (preempted by the link: wall time) | 4.6 to 5.7 s of CPU 1 |

- One run (of 8) lost input (9 gaps) and stalled the loop for 1.1 s, unexplained.
- Flash writes during a capture cost some 15 gaps on either image.

### Measurements, 2026-11-09

#### XIAO ESP32S3

multiapp, an office full of Wi-Fi, channel 37, automatic gain. Bluetooth LE packets at each step:

| Step | Packets | Before |
| --- | --- | --- |
| The detector alone on its thread | 1207 in 15 s | 347 |
| The detector alone, the display drawing | 1551 in 20 s | 222 |
| The decoders on their thread | 2088 in 15 s | |
| The decoders on their thread, the display drawing | some 2400 in 20 s | |
| The multiapp image's PSRAM at 80 MHz, the display drawing | 2914 in 20 s | |
| The packets made in C | 3412 | |
| The records of numbers alone shed while the ring is half full | 3758 in 20 s | |

- The "before" counts are the [first version](#the-first-version)'s.
- Detector alone: the ring lost no input.
- Detector alone: the detector took 37 % of CPU 0 and the decoders 18 %.
- Detector alone, the display drawing: the table drew 3 frames a second.
- PSRAM at 80 MHz: 40 before; the modules, MicroPython and the decoders among them run from it.
- Packets made in C: the table redrawn 1.8 times a second while the loop spent a quarter of its
  time waiting.
- The packets were advertising, scan requests and responses, extended advertising and named
  devices.
- 802.15.4 decodes on 11, 15 and 25, the networks in range: acknowledgements, data frames and
  commands.

#### XIAO ESP32S3 buffers

The two accounts of the runs, side by side:

- One account: CPU 0 is the limit; in the busiest moments some 5000 bursts in 15 s find no free
  buffer (mostly the band's Wi-Fi edges, among them some packets).
- The other: with the decoders on their thread and the analyses capped, no burst was without a
  buffer (some 5000 in 15 s before).

A busier hour:

- The ring of packets filled: 1023 bursts without a buffer in 20 s, the decoders waiting 127 times.
- With the records of numbers alone shed (the table's last row): 272 of them shed.
- With them shed: no burst without a buffer and no wait.

#### Other boards

- The Communicator badge: 584 packets in 20 s with its display (detector thread only), more noise
  bursts at its receiver.
- The VIEWE board: 3530 to 3704 in 20 s with its display (800 by 480), no burst without a buffer,
  the stream without a gap.
- The WHY2025 badge's P4, no display: 1407 to 1635 in 15 s on one CPU, all of the stream seen, 12
  bursts without a buffer (228 before the shedding). On two CPUs see
  [ESP32-P4: two CPUs](#esp32-p4-two-cpus).
