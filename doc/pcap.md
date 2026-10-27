# Radio bursts as pcapng (apps/pcap)

Design note, 2026-10-26, implemented and measured on the WHY2025 badge on 2026-10-27: the badge's
ESP32-P4 demodulates and decodes what its ESP32-C6 radio receives and streams it over USB as
pcapng for Wireshark. Known protocols (Bluetooth LE, IEEE 802.15.4) arrive on link types
Wireshark dissects; every other burst arrives as a record of a custom link type with the
signal's metadata (frequency, modulation, symbol rate, power) and its demodulated bits. The app's
README (`apps/pcap/README.rst`) says how to build and use it.

## Goal and scope

- **In scope:** bursts in the 2.4 GHz band as the badge receives them, best-effort demodulation
  and decoding on the P4, pcapng written by the firmware, a Wireshark extcap that only copies the
  stream and sets the badge up, a Lua dissector for the burst record.
- **Out of scope:** hopping beyond the survey's span (80 MHz, or 40 at 40 MS/s), following
  connections across hops, LE Coded, Bluetooth BR/EDR decoding (its bursts come as burst
  records), transmit.

## The radio and its limits

The C6 (`lib/esp_sdr/src/esp_sdr_gapless.c`, `apps/osmosdr_serial`) receives the whole 80 MHz
without a gap and prunes it in time: the IQ estimator's 10 us windows over the full band decide
what is kept (a threshold over the quiet baseline, a window before and after), the kept windows
are copied into a staging ring, packed to int8 and sent over SDIO, and a servo holds the
threshold where what is kept fits the link (11 MB/s) or the CPU (samples lost raise it). The C6's
CPU is a 160 MHz rv32imac (no SIMD, no FPU): at 80 MS/s it has 2 cycles a sample and cannot
touch every sample, so nothing on it can channelize the stream; the estimator's windows are its
only view of the band, and they see power, not frequency.

The run keeps the PHY in its 20 MHz mode with the widest low-pass, which passes the whole band:
an 802.15.4 sender read within 7 dB of its level at +-15 MHz out to +-35 MHz (2026-10-27). The
40 MHz mode the run used before was 16 to 21 dB down at +-25 MHz and some 35 dB at +-35 MHz and
ignored the low-pass setting: the survey's edges, the advertising channels 37 and 39 among them,
reached the P4 30 to 50 dB down.

Measured on the badge (2026-10-26, from the P4's MicroPython):

- the link never limited: 1.4 to 4.2 MB/s of its 11 at thresholds from 1.25 to 5 times the
  baseline;
- the C6's own work did: copying costs about 3.6 cycles a kept sample, packing about 20 (with its
  slice overheads), estimator windows 18 % of the CPU, markers 9 %; a unit (154 us) keeps only
  what the CPU can copy before its block is reused, about 4000 samples (the per-unit cap), so a
  burst arrives whole up to some 50 us, 100 us when it straddles a switch: the survey's records
  were at most 91 to 104 us long, their median 26 to 32 us. The staging ring (64 KiB of raw
  words, 205 us at 80 MS/s) did not limit a single burst;
- so in the survey (below) only short packets arrive whole: a Bluetooth LE packet of up to some
  25 bytes on air, no 802.15.4 frame (an ACK lasts 352 us). In the office, with Wi-Fi, the survey
  decoded no Bluetooth LE packet at all; its bursts were nearly all shorter than 64 us.

**Longer bursts** (2026-10-27, the user's question "how can we increase the maximum burst
length?"):

- *Spare staging*: the ring uses the lower 64 KiB of SRAM blocks 1 and 2; their upper halves are
  two more staging stores (canary words at both ends, in service only after both blocks came back
  intact), 196 KB in all. A kept window goes to block 3 first, else to the upper half of its own
  unit's block, which the engine has just left; a job there is packed only while the engine
  writes the other block.
- *Whole bursts or nothing*: admission counts only the staging a window can reach (block 3 and
  the smaller half), so what is admitted is never abandoned for want of room; a burst starts only
  with an eighth of that free (else it is refused), and one cut for want of room drops its rest
  until a quiet window: one piece per transmission instead of scraps. A band that never rests
  starts anew after as many samples as that staging holds, once an eighth is free again.
- *Chasing the engine* (reading a block while it is written, as upstream's C3 port does) is
  impossible on the C6: the CPU reads stale data from a block the engine owns (0.7 % of the words
  matched, 55 % were its own marker words).
- *40 MS/s* (`ESDR_GAPLESS_F_HALF`, `PCAP_MODE_F_HALF`, the host tool's `--survey-rate 40`): the
  dump engine's rate bit (DUMP_CTRL bit 15) halves the rate, so a unit lasts 307 us and the copy
  has twice the time a sample. The engine drops every other sample without a filter: a sender at
  +25 MHz showed at -15 MHz at its full level, so the run sets the low-pass to 31 MHz, which keeps
  +-15 MHz flat and puts what folds in from +-25 MHz 15 to 25 dB down (28 MHz and narrower cut
  +-15 MHz too); near 20 MHz out the low-pass is only a few dB down, so the bin at +-20 MHz,
  where both edges land, is left out. The P4 runs half the channels (M = 20 for 40) so that they
  keep their 2 MHz spacing and 4 MS/s (16 channels would be M = 8, which the bank does not have:
  the start is refused); the C6 does not sort by width there. Records got some three times longer
  (median 70 us against 28), and the survey decoded Bluetooth LE for the first time: 139 to 241
  packets in 15 to 20 s, every CRC good, against none at 80 MS/s. 802.15.4 data frames (over a
  millisecond) still arrive cut. The P4 is the limit there: the C6 keeps twice the pairs
  (2.2 M a second), the filterbank takes some 100 cycles a pair (62 % of the CPU), and 15 % of
  the bursts find no buffer.
- Further levers (from a survey of the code, not built): a faster copy on the C6, its GDMA or the
  dump engine's four-slot mode, and a follow mode that parks the channel mode on the busiest
  channel for whole long frames.

**Width sorting** (the user's choice for the C6, `esp_sdr_gapless_sort.h`): the C6 judges each
chunk of kept samples (a window's length) by its correlations 4, 6 and 10 samples apart against
its power above the noise, at 50 points; all three below a half means a band wider than some
10 MHz (a flat 17 MHz band gives 0.17, 0.19, 0.06; a 2 MHz one 0.98, 0.96, 0.90; two equal
narrow tones keep one of the three above 0.5 at any spacing), and the chunk goes as a 4-pair
record (`ESDR_SEG_WIDE`, `struct esdr_seg_wide`: span, the lag-one correlation whose angle is the
signal's mean frequency, the three correlations) in place of its samples; the threshold servo
counts the saved bytes. It is opt-in (`ESDR_GAPLESS_F_WIDE`): at the desk it found few wide
windows and costs 5 to 12 % of the C6's CPU (a point 103 cycles), and since the link was not the
limit it could not lower the threshold the CPU holds. Its arithmetic is 32-bit throughout:
64-bit divisions went to libgcc in ROM and made the ring's switch late.

**The paced receive** (the channel mode, `lib/esp_sdr/src/esp_sdr_paced.c`): the C6's CPU
reads the IQ estimator's running sums every 80 cycles at 2 MS/s, each difference a boxcar of
40 inputs, one 802.15.4 chip. Between calls of `esp_sdr_rx_paced_fill_iq8()` osmosdr_serial
turns to its link (the SDIO DMA chain, requests), which outlasts the loop's slack of some 20
cycles: the next poll came late, one sample spanned several windows (a spike of about three
times the level) and the loop caught up with polls back to back, windows of 0.75, skewing some
32 samples. With a call a quarter block, that was a glitch every 255 us (found in raw samples
dumped from the badge: 158 of the gaps between spikes were 255 us). It broke one 802.15.4
symbol in about seventy (frames of the S3's software radio: 24 to 29 % decoded at any gain, the
rest with one or two symbols wrong at a high LQI) and most Bluetooth LE packets. Now the link's
turn comes once a block (1.02 ms), and a poll whole periods late gives each sample it missed an
equal share of its sums, so that the samples stay on their grid: 97 and 98 % of the S3's frames
decode, three times the 802.15.4 frames of a Zigbee network in range, five to seven times the
Bluetooth LE packets.

## Modes

- **Survey** (`PCAP_MODE_SURVEY`): the C6's gapless stream through the P4's fixed-point
  polyphase filterbank (`apps/common/esdr_pfbq.h`, PIE SIMD) at M = 40: 2 MHz channels at
  4 MS/s, the widest of the decoded protocols, and a burst detector per channel. It finds and
  describes bursts across the band and decodes the short packets that arrive whole. At 40 MS/s
  (`PCAP_MODE_F_HALF`) it covers 40 MHz with M = 20 and keeps bursts some three times longer.
- **Channel** (`PCAP_MODE_CHANNEL`): one channel streamed by the C6's paced receive at 2 MS/s
  (`ESDR_MODE_RX`, 8-bit items, the LO on the channel, the DC taken out on the C6) and a burst
  detector over it: complete Bluetooth LE 1M packets and 802.15.4 frames of that channel.
- **Alternating** between the two with dwell times (`struct pcap_mode_cfg`); a switch costs
  some 15 ms of the C6.

## The P4 (two cores, 360 MHz)

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

**Cores** (the WHY2025 badge, `boards/why2025_badge_esp32p4_hpcore.conf`): Zephyr SMP with pin-only
scheduling, so a thread nobody pins stays on CPU 0. CPU 1 runs the stream thread alone; CPU 0 runs
the reader, USB, the decoder, the generic analysis, the control queue, the shell and logging, and
takes every interrupt and the tick (`CONFIG_APP_PCAP_STREAM_CPU`, `CONFIG_APP_PCAP_DECODE_CPU`).
The other way round (decoders on CPU 1) the reader, which preempts the stream, and the stream
overloaded CPU 0 at 40 MS/s: radio blocks dropped, USB starved, a third fewer Bluetooth LE packets
(Implementation). The stream thread is created suspended and pinned before it starts: PIE is
switched on per core and its vector, accumulator and loop registers are not saved on a context
switch, so a PIE thread that moved would trap on the other core or carry on with its registers. The
cores share the L1 data cache: bursts change cores without cache maintenance. `mcycle` is per core,
so the cycle statistics come from pinned threads. The EV board and MicroPython stay on one core.

**Survey assembler** (`src/survey.c`): each channel's opening becomes a burst (`burst.h`): the
detector's frames (`esdr_detq_take()`), a short gap zero filled so that every sample keeps its
time; a hole in the C6's stream of two windows or more ends every open burst there
(`BURST_F_HOLE`): a channel held open over it would otherwise fill its burst with zeros (it did:
1.5 M cycles a burst and thousands lost, 44 000 after). A signal that opens six or more adjacent
channels at once is kept as one wideband burst without samples (its span, duration, power);
the C6's width records join into wideband runs. Bursts that never got loud after a hole go back
unseen.

**Channel detector** (`src/narrow.c`): the power of 16-sample blocks against a floor learnt from
the quietest of the first blocks and following the quiet ones, 64 samples of pre-roll, closing
after 6 quiet blocks; a burst with fewer than 3 loud blocks goes back unseen. The receiver's noise
made thousands of one-block blips a second (2026-10-27: 59 000 bursts in 15 s, 99.7 % shorter
than 64 us), and nothing of interest is that short. Whole blocks go at once from the 8-bit items
(power summed in integers), at -O2: sample by sample it took 54 % of the CPU at 2 MS/s, the
decoders the rest, and the pool ran dry; now 25 %.

**Gain in the channel mode**: automatic means gain 60; the C6's digital gain is 0 dB whatever
the analog gain (24 below its 8-bit default), the floor some 37 dB under full scale, still a
few steps of the 8-bit items. At the desk on channel 37 in 10 s: gain 30 decoded 16 packets of
one device (the noise floor at -20 dBFS), 40 48 packets of 7, 50 64 of 15, 60 161 to 214 of 23
to 30, 64 100 (the nearest transmitters clipped), the PHY's own gain control 74. With 18 dB of
digital gain an 802.15.4 sender on the desk clipped and decoded never; on channel 37, 0 dB
decoded 1937 Bluetooth LE packets in 15 s, 6 dB 1755, 18 dB 1691.

**Decoder thread** (`src/decoder.c`): per burst, in order:

1. Bluetooth LE (`src/dec_ble.c`) when the burst's frequency is a channel of the grid (within 500
   kHz) at 4 MS/s (LE 1M and 2M) or 2 MS/s (LE 1M), its loud part at least 64 us long and at
   most 2400 us (unless the burst was cut), and its correlation one sample apart over its power
   above the floor high enough: in the channel mode above 0.45 (a Wi-Fi edge in the channel is
   near 0); in a bank's bin above 0.75, where the bin's own filter correlates a band filling it
   to some 0.7 (simulated through the prototype 0.69 to 0.72, Wi-Fi measured 0.6 to 0.9) and
   Bluetooth LE at 2 samples a symbol (LE 2M at 4 MS/s, LE 1M at 2 MS/s) keeps 0.78 to 0.88; and
   at 4 MS/s the LE 1M pass only above 0.85 (LE 1M keeps 0.95; every one of 437 packets of a 40
   MS/s survey was above 0.9, and 58 % of the other bursts, which held most of the decoder's
   time, below 0.85), the LE 2M pass only below 0.92. Without these gates the decoder spent 98
   000 cycles a burst on noise and lost a tenth of them (the survey's 32 us scraps, marked as
   cut by the C6's drops, ran it 13 000 times in 15 s), and the 40 MS/s survey lost a third of
   its bursts;
2. IEEE 802.15.4 (`src/dec_wpan.c`, `lib/ieee802154/ieee154_phy.c` with receiver instances) on
   a channel of the grid (2405 + 5 n MHz), 300 to 4500 us: at 4 MS/s from a bin, the channels
   between two bins (odd MHz with an even centre) from the two neighbours combined,
   `z[m] = y_k[m] (-j)^m + y_(k+1)[m] j^m` with m the absolute frame index (the bank keeps
   absolute phase, `esdr_pfb.h`; checked with a tone between two bins and with frames through
   the bank: 6 of 6 combined, 0 of 6 from either bin alone); at 2 MS/s, the channel mode's,
   read at 4 MS/s with every other sample a cubic midpoint (`(9 (x[m] + x[m+1]) - x[m-1] -
   x[m+2]) / 16`): simulated from the C6's 40-input boxcar at any offset to the chips, the PHY
   decodes that down to 6 dB as from 4 MS/s. The carrier offset there comes from the phase over
   the frame less what its decoded chips turn (O-QPSK as MSK, chip j by (-1)^(j+1) c[j] c[j-1]
   times 90 degrees): within 1 kHz, where the usual estimate (the squared phase step a chip)
   misses by tens of kHz on boxcar samples;
3. wideband bursts and runs as burst records;
4. what no decoder claimed goes to the **generic analysis**, a thread of its own at the lowest
   priority (time-sliced with the shell) with a queue
   of an eighth of the pool: it takes the time
   left over and never holds the decoders up. A burst whose correlation one sample apart is below
   0.5 of its power above the floor, that power at least the floor's, fills the channel (the
   analysis found such bursts unknown or wideband, median bandwidth 1.8 MHz of 2): it goes out
   as a wideband record unanalysed. One the queue has no room for goes out with the detector's
   metadata only (`PCAP_BF_SKIPPED`).

The analysis (`src/dec_generic.c`, `src/refine.c`): centre and occupied bandwidth from an
averaged spectrum; **refinement**, the user's rule ("only channelize further if the bandwidth of
the signal is sufficiently small after the previous channelization, recursively"): while the
signal fills less than a quarter of the rate, mix to its centre and decimate by two through a
half-band stage, then measure again, down to 62.5 kS/s; then the modulation, symbol rate and
bits, from at most 8192 samples of the loud part (the record's duration and power cover all of
it). On the P4 it costs 0.4 M cycles for a survey burst and 1.4 M for a channel mode burst of
some 350 us; a 2048-sample 2FSK burst 1.9 M, of which the symbol grid's search (`try_fsk()`)
1.1 M, the phase steps 0.27 M, the spectrum 0.18 M. The decoders and the analysis inline took
2.6 M cycles a burst and lost nine bursts in ten in the channel mode.

**The pool**: `CONFIG_APP_PCAP_BURSTS` buffers of 128 KiB in PSRAM, 128 on the badge (16 MiB),
32 on the EV board. The survey's 40 channels hold one each while open and the channelizer can
keep the decoder waiting for some 12 ms, during which a busy band opens a few dozen bursts: 48
buffers lost a quarter of the survey's bursts, 128 lose about 1 %.

Bluetooth LE access addresses: the advertising one, the host's (`PCAP_REQ_ADD_AA`) and those of
CONNECT_IND PDUs seen (their CRCInit with them, the four newest), so connections set up in view
are followed while their hops land in view.

**Metadata**: in the link type's own header where it has room (RF channel, PHY, CRC flags for
Bluetooth LE; channel, frequency, LQI, FCS type for 802.15.4), the rest as a pcapng comment
(`src/records.c`): frequency, channel, rate, PHY, carrier offset, power and floor in dBFS, SNR,
gain, duration. Power is dBFS, not dBm: the radio is not calibrated, so the dBm fields stay
unset.

**Output** (`src/output.c`, `src/pcapng.c`): one pcapng section per capture (SHB, three IDBs in
the order of `PCAP_IF_*`: 0 Bluetooth LE, 256; 1 802.15.4 TAP, 283; 2 bursts, 147; `if_tsresol`
9), an EPB per packet, an ISB per interface every second (received and dropped); 4 KiB transfer
buffers in internal RAM go out when full or after 50 ms, each ending the host's read (a
zero-length packet after a whole number of packets). A block that does not fit is dropped and
counted, never waited for. A host that comes during a capture gets the section's header first.
Timestamps are nanoseconds: the stream's axis (the C6's 80 or 40 MS/s clock, or the channel's
sample index) anchored to the host's time (`PCAP_REQ_SET_TIME`) at a run's first block.

## USB

Vendor interface (class 0xFF) with one bulk IN endpoint, 512-byte packets at high speed
(`src/usb_pcap.c`); the CDC-ACM shell and DFU of `apps/common/src/app_usb.c`; VID 0x2fe3,
PID 0x0005, product "ESP-SDR pcap". The P4's OTG controller has 896 words of FIFO RAM
(GHWCFG3, read on the badge) and makes an IN endpoint bulk capable only with a FIFO of 128
words: `g-tx-fifo-size = <16 128 128 64 64 64 64>` gives CDC-ACM's notification endpoint 16 words
and the two bulk IN endpoints 128 each (816 words in all with the RX FIFO and EP0's); three FIFOs
of 128 did not fit, the driver refused them, USB never came up and MCUboot reverted the image.
The image's version must be MicroPython's (0.1.3) or later: MCUboot refuses downgrades.

Vendor requests: `esdr_proto.h`'s below 0x40 go to the C6 (frequency and gapless settings are
kept and applied at the start; `ESDR_REQ_SET_CHANNELIZER` sets the bank), the app's own from
0x40 (`src/pcap_proto.h`): `START`, `STOP`, `SET_TIME`, `SET_DECODERS`, `GET_STATS`, `ADD_AA`,
`SET_MODE`. The host leaving (interface disabled) stops the capture.

## Host side

`apps/pcap/tools/esdr_pcap.py` is a Wireshark extcap and a standalone tool (pyusb): one interface
per badge (`esdr-pcap-<serial>`), the options in its dialog, the requests, then bulk IN copied
in whole pcapng blocks (resynchronising after a torn read) until Wireshark stops it.
`apps/pcap/tools/esdr_burst.lua` dissects the burst record (`esdr_burst.*`), reading by its
header length so that later versions with more fields still parse.

## Testing

Unit (native_sim/native/64, twister): `pcapng` (blocks byte for byte, the records, the
comment), `pcap_ble` (the golden packets at 2 and 4 samples a symbol, LE 1M and 2M, offsets,
bad CRCs, CONNECT_IND, an SNR sweep), `pcap_wpan` (receiver instances, frames through the bank,
the combination's signs, bad FCS, the channel mode's 2 MS/s from the C6's boxcar at three chip
offsets), `pcap_generic` (the analysis on synthetic bursts, its sample cap),
`pcap_chain` (Bluetooth LE packets GFSK modulated at 80 MS/s at six bins through the bank and
decoded from their bin; nothing from the mirror bin), `gapless_sort` (the C6's width sorting on
synthetic dump words); `esdr_pfb` and `esdr_pfbq` cover M = 20 (numpy reference regenerated
with it). Host (pytest): `test_esdr_pcap.py` (the extcap's output and options, a
capture against a fake badge, resynchronisation, a stream from the C writer read by a parser
from the specification and by tshark with the Lua dissector). Smoke: the app builds for both P4
boards. Bench: `tools/esdr_pcap.py -w` on the badge, then tshark; 802.15.4 from `apps/wpan` on an
S3 (`wpan on`, `wpan set chan 25`, `wpan tx 400 20 20`: test frames whose bytes follow from their
counter, so that every wrong symbol shows).

## Implementation

Built 2026-10-26 and 27 on the badge (P4 v1.0 at 360 MHz, C6 osmosdr_serial), measured with the
host tool and tshark, 15 s each:

- channel mode, advertising channel 37 (2402 MHz), automatic gain: 2346 Bluetooth LE packets,
  every CRC good, 0 of 4568 bursts lost; 1015 bursts analysed, 1229 filling the channel (before
  the paced receive's fix: 313 and 334 packets from some 25 advertisers);
- channel mode, 802.15.4 channel 25 (2475 MHz), an S3 on the desk sending 400 test frames 20 ms
  apart: 393 decoded, and 158 frames of a Zigbee network in range (ACKs, data requests, ZigBee
  commands; tshark dissects them through the TAP header);
- survey at 2440 MHz: 18 552 bursts, 220 lost (1.2 %), 6785 wideband, 8852 analysed, 3635
  recorded unanalysed; 83 to 91 cycles a pair in the filterbank; no Bluetooth LE packet whole
  (above);
- survey at 40 MS/s (2440 MHz, the PHY's 20 MHz mode, 31 MHz low-pass), 20 s: 241 Bluetooth LE
  packets, every CRC good; 28 887 bursts, 4303 lost (15 %); 102 cycles a pair in the filterbank
  (M = 20) at twice the 80 MS/s survey's pairs. The PIE kernels give the C ones' outputs bit for
  bit at every M, 20 included (run on the badge).

On both cores (Zephyr SMP, 2026-10-27; an S3 sending 802.15.4 frames on channel 25, 30 s per run,
two runs of each image alternated, the same sources and runtime statistics in each):

| survey | image | bursts | lost | BLE, CRC good | analysed | radio blocks dropped |
|---|---|---|---|---|---|---|
| 40 MS/s | one core | 32 560, 34 947 | 6325, 8008 | 314, 295 | 1303, 890 | 0 |
| 40 MS/s | decoders on CPU 1 | 46 891, 46 846 | 1085, 68 | 215, 205 | 17 216, 16 766 | 1880, 1796 |
| 40 MS/s | stream on CPU 1 | 39 935, 45 094 | 16, 0 | 353, 325 | 7170, 7307 | 0 |
| 80 MS/s | one core | 16 400, 26 699 | 43, 215 | 0 | 9721, 12 863 | 0 |
| 80 MS/s | stream on CPU 1 | 32 789, 33 504 | 0, 0 | 0 | 12 716, 13 428 | 0 |

With the stream thread alone on CPU 1 the filterbank costs 78 cycles a pair (wall clock; on one
core the reader's preemption made it 98 to 101) and leaves CPU 1 46 to 51 % idle at 40 MS/s, 77 %
at 80 MS/s; CPU 0 carries the reader (29 %), the decoder (23 to 25 %) and the generic analysis in
the rest. With the decoders on CPU 1 instead, CPU 0 had the stream (66 %) and the reader (28 %):
the reader's preemption and the generic analysis' traffic through the shared L1 data cache (without
the analysis 128) took the filterbank to 141 to 145 cycles a pair, 5 % of the radio blocks were
dropped and the USB threads starved (output records dropped): fewer packets decoded than on one
core.
