# Host transports

<!-- nav-start -->
Previous: [Paced streams](paced.md) · Up: [Samples in and out](index.md) · Next: [Modulation and coding](../modem/index.md)
<!-- nav-end -->

The link to the host sets the rates of every path in this part: a ring that runs gapless on the
chip still loses samples if the link cannot carry them. This page lists the links, what each
carries and which chip uses which. It builds on the paths of [Samples in and out](index.md).

## Transports at a glance

| Link | Chips and boards | Apps | Capacity | Best measured stream |
|---|---|---|---|---|
| USB full speed, CDC-NCM (VITA 49.2) | ESP32-S3 | [stream](../../apps/stream/README.rst) | about 1 MB/s receive | 8 bit / 400 kS/s transmit, 100 % ([stream](../../apps/stream/README.rst)) |
| USB full speed, vendor bulk | ESP32-S3 | [osmosdr](../../apps/osmosdr/README.rst) | 1.16 MB/s in, 1.18 MB/s out | 500 kS/s 8 or 4-bit receive, gapless ([osmosdr](../../apps/osmosdr/README.rst)) |
| USB-Serial-JTAG | ESP32-C6, ESP32-C5 | [osmosdr_serial](../../apps/osmosdr_serial/README.rst) | about 500 kB/s each way | 250 kS/s 8-bit receive |
| UART | WHY2025 badge, C6 to P4 | [osmosdr_serial](../../apps/osmosdr_serial/README.rst) | 5 Mbaud | the same rates as USB-Serial-JTAG |
| SDIO | WHY2025 badge, C6 to P4 | [osmosdr_serial](../../apps/osmosdr_serial/README.rst) | about 13 MB/s within a transfer | 11.6 MB/s raw; 2 MS/s paced receive |
| USB high speed, vendor bulk | ESP32-S31 | [osmosdr_hs](../../apps/osmosdr_hs/README.rst) | 40.157 MB/s with a 256-word TX FIFO | 20 MS/s ([osmosdr_hs](../../apps/osmosdr_hs/README.rst)) |
| USB high speed, bridge | WHY2025 badge, P4 for the C6 | [osmosdr_bridge](../../apps/osmosdr_bridge/README.rst), [pcap](../../apps/pcap/README.rst) | SDIO to the C6 (above) | 2 MS/s receive, gapless in 5 s runs ([osmosdr_bridge](../../apps/osmosdr_bridge/README.rst)) |
| RP2040 links | MCH2022 badge, classic ESP32 | [osmosdr_serial](../../apps/osmosdr_serial/README.rst), [mch2022_coproc](../../apps/mch2022_coproc/README.rst) | UART up to 5 Mbaud; FPGA fast line 15 Mbaud; USB alone 928 kB/s | 450 kS/s at 2 MS/s through the FPGA, some 900 kB/s ([MCH2022 badge](../apps/mch2022.md)) |

## USB full speed

The ESP32-S3 apps use the S3's OTG port at full speed.

### VITA 49.2 over CDC-NCM

- The USB transport of [stream](../../apps/stream/README.rst) is CDC-NCM, an Ethernet-like network
  device over USB.
- Both the receive VITA stream and the transmit VITA command and data channel ride over UDP/IPv6
  inside that virtual link, on different UDP ports.
- USB full speed limits the receive stream to about 1 MB/s. The rates this gives per item width,
  and the packet format, are in [stream](../../apps/stream/README.rst).

### osmosdr vendor bulk

- USB full speed, vendor bulk: 1.16 MB/s in, 1.18 MB/s out, measured with two XIAO ESP32S3 boards.
- The vendor requests and the block format are in [osmosdr](../../apps/osmosdr/README.rst).

## Serial links

The ESP32-C6 and ESP32-C5 carry the osmosdr protocol over byte streams.

### USB-Serial-JTAG

- The only USB port of the C6 and the C5 is USB-Serial-JTAG.
- The osmosdr vendor requests travel as frames on the byte stream, with receive blocks and transmit
  samples in between (the serial transport in `esdr_proto.h`).
- The app drives the port's FIFOs itself. One direction runs at a time, as on the S3.
- USB-Serial-JTAG carries about 500 kB/s each way: 250 kS/s receive fits as 8-bit items only,
  16 bit up to 125 kS/s.

#### FIFO service

Found with the C5's diagnostic-bus ring ([Gapless receive](rings.md#esp32-c5-diagnostic-bus-ring)):

- The decimated samples reach the sink in chunks every 51 us.
- The USB-Serial-JTAG FIFO takes one 64-byte packet per visit.
- Serving it once per 8 KiB ring block (205 us) capped the stream near 310 kB/s and lost a quarter
  of the samples at 250 kS/s.

#### Host-fed transmit

The sources disagree about transmit at 250 kS/s from a host over USB-Serial-JTAG. Neither is dated:

| Source | ESP32-C6 at 250 kS/s | ESP32-C5 at 250 kS/s | Reliable rate |
|---|---|---|---|
| [osmosdr_serial](../../apps/osmosdr_serial/README.rst) | about 10 % of the input arrives late and plays as zeros | keeps up | 125 kS/s on the C6 |
| C5 bench: one C5-Zero, a XIAO ESP32-C6 at the other end of a 30 dB coax attenuator | not measured here; "starves on the C5 too" refers to the C6 | starves too: USB OUT delivers about 239 kS/s | 125 kS/s on both chips |

### UART

- The WHY2025 badge's C6 can reach its P4 over UART (chosen `esdr,link-uart`) at 5 Mbaud, with RTS
  for flow control.
- It carries the same rates as USB-Serial-JTAG.
- It is the badge's fallback link: its image is the one from before the gapless receiver, which
  needs SDIO; the Wi-Fi driver brings the radio up. How to build it is in
  [osmosdr_serial](../../apps/osmosdr_serial/README.rst).

### SDIO

- `CONFIG_APP_LINK_SDIO`, the WHY2025 badge's default, uses the C6's SDIO slave: function 1,
  4 bit, as esp-hosted uses it (`src/link_sdio.c`, on the HAL's LL layer).
- The host's writes land in 16 buffers of 512 bytes counted by tokens.
- The receive blocks go out by DMA straight from their buffers: 11.6 MB/s measured raw.
- Replies are zero padded to whole words, which the host skips between frames.
- A host that restarts writes bit 0 of its register 0x08C: the radio drops the send queue and
  starts its counters again.
- Paced receive reaches 2 MS/s over it ([Paced streams](paced.md)).

#### SDIO ceiling

- The SDIO link tops out at about 13 MB/s within a transfer, whatever the bus clock: 20 to 80 MHz
  all give the same (80 works, with the P4 sampling on input phase 1 or 2).
- SDIO blocks of 512 to 2048 bytes give the same too; larger ones corrupt data.
- That places the limit in the C6's slave.
- So the C6 sends triggered snapshots rather than a continuous stream
  ([Burst capture](bursts.md#triggered-snapshots)).

## USB high speed

The ESP32-S31 and the ESP32-P4 have a USB 2.0 high-speed OTG port.

### TX FIFO sizing

A stream's bulk IN endpoint needs more than one packet of TX FIFO.

| | ESP32-S31 | ESP32-P4 |
|---|---|---|
| FIFO RAM (`GHWCFG3`) | 896 words free in all; the FIFOs' power-on depth is 512 words | 896 words, read on the badge |
| Bulk IN FIFO | 128 words in `DIEPTXF1` (below) | an IN endpoint is bulk capable only with a FIFO of 128 words |
| Working configuration | 256 words or more | `g-tx-fifo-size = <16 128 128 64 64 64 64>`: 16 words for CDC-ACM's notification endpoint, 128 each for the two bulk IN endpoints (816 words in all with the RX FIFO and EP0's) |
| What failed | 128 words (above) | three FIFOs of 128 did not fit: the driver refused them, USB never came up and MCUboot reverted the image |

- On the S31, with 128 words every IN token during a refill gets a NAK, and the link stops at
  exactly 16.384 MB/s, 4 packets a microframe.
- From 256 words it carries 40.157 MB/s, 20 MS/s plus block headers, without a gap.
- On the S31, DMA burst length and `USBTRDTIM` (0, 5 and 9 tried, the driver's value left alone)
  hardly matter.
- A freshly reset S31 core reads 0 in `DIEPTXFn`: it is in host mode until forced.
- The P4's vendor interface and requests are in [pcap internals](../apps/pcap-internals.md).

### ESP32-S31 port

- The only OTG connector is the USB-A socket J4, with VBUS always driven: read
  [osmosdr_hs](../../apps/osmosdr_hs/README.rst) before connecting it to a PC.
- Zephyr's DWC2 driver works, buffer DMA from TCM included.

#### A marginal cable

A marginal cable looks like a dead PHY.

- The host reports high speed, then `-71` at every transaction, because the chirp is slow enough
  to survive what the data is not.
- The tell is `GINTSTS.SOF` (sticky, nothing clears it) staying 0, and full speed working
  (`DCFG.DEVSPD` 1 and a soft reconnect, possible over JTAG).
- `adj_vref_sq` boots as 8 where the HAL's header says 2. Lowering it lets a bad cable decode some
  packets, but a good cable needs no change.
- Host mode over JTAG with a USB stick in J4 tests the chip's path with no cable at all.

### ESP32-P4 bridge

[osmosdr_bridge](../../apps/osmosdr_bridge/README.rst) makes the WHY2025 badge's P4 a high-speed
USB device for the C6's radio:

- Vendor requests go to the C6 as request frames, and its replies come back.
- The C6's receive blocks are already in the USB block format and go out on bulk IN as they are,
  from a pool of 16 blocks (16 ms at 2 MS/s).
- The host's transmit samples go to the C6 as transmit frames.
- Receive reaches 2 MS/s (8-bit items above 250 kS/s), transmit 250 kS/s: the C6's limits.

## RP2040 links

The MCH2022 badge's classic ESP32 reaches USB through the badge's RP2040, which bridges its UART0
without flow control wires. The plain transport works through any RP2040 firmware. With
[mch2022_coproc](../../apps/mch2022_coproc/README.rst) on the RP2040 there are two more ways:

- **The framed link** (`esp_sdr/esdr_link.h`) on the RP2040's USB interface 6: blocks go by their
  length, with a checksum and a credit that keeps the buffers behind them from overrunning.
- **The fast line**: blocks go from the ESP32 by SPI DMA into the FPGA and on at 15 Mbaud to the
  RP2040, which merges them onto interface 6.

Both, their throughput and the stream rates they give are in [MCH2022 badge](../apps/mch2022.md).

<!-- nav-start -->
Previous: [Paced streams](paced.md) · Up: [Samples in and out](index.md) · Next: [Modulation and coding](../modem/index.md)
<!-- nav-end -->
