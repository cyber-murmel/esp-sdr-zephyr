# Modulation and coding

<!-- nav-start -->
Previous: [Host transports](../samples/transports.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Channelizer](channelizer.md)
<!-- nav-end -->

This is the third part of the docs. [The radio](../radio/index.md) ends with complex baseband
I/Q samples, and [Samples in and out](../samples/index.md) brings them into memory as bursts,
gapless rings and paced streams. This part turns those samples into packets and packets back into
samples.

It covers how the code finds signals in a band, demodulates them, finds their frames and corrects
their errors. Each page takes one technique as the tree implements it: the idea first, then the
implementation, then its measurements and limits. All figures live on the pages, not here.

## Fundamentals

The pages rely on these ideas; each is explained where it is used.

- **Complex baseband.** Every receiver here starts from I/Q samples around the LO
  ([The radio](../radio/index.md)).
- **Channels.** A channelizer splits one wideband stream into a grid of equally spaced channels,
  so that one receiver watches many ([Channelizer](channelizer.md)).
- **Phase-difference discriminator.** The imaginary part of a sample times the conjugate of an
  earlier one measures the phase step between them. Its sign is a hard decision per chip or bit,
  and a carrier offset only biases it ([IEEE 802.15.4](ieee802154.md),
  [GFSK and Bluetooth LE](gfsk.md)).
- **O-QPSK as MSK.** Half-sine O-QPSK turns the carrier phase by plus or minus 90 degrees each chip
  period, so the discriminator recovers its chips without carrier recovery
  ([IEEE 802.15.4](ieee802154.md#o-qpsk-as-msk)).
- **Single carrier and OFDM.** Single-carrier QAM sends shaped symbols on one carrier; OFDM sends
  them on subcarriers, each symbol with a cyclic prefix, with pilot symbols for the channel
  ([QAM and OFDM](qam-ofdm.md)).
- **MER.** The modulation error ratio a link reaches caps the constellation it can carry
  ([MER ceilings](qam-ofdm.md#mer-ceilings)).
- **Codes.** A frame's payload travels under an error correcting code or a CRC alone
  ([Error correction](fec.md)).

## Receiver tasks

Every receiver in this part solves the same tasks. The pages name them as follows.

### Detection

Finding when, and on which channel, a signal is on the air:

- the wideband and per-channel power gates of the [Channelizer](channelizer.md);
- the survey assembler and the channel detector of [Burst detection](bursts.md);
- the preamble hunt of [IEEE 802.15.4](ieee802154.md#receive-chain), and its preamble sightings in
  [Burst detection](bursts.md#802154-preamble-sightings);
- the squelch and burst edge of the Ubertooth demodulator, and the access code search of the
  MicroPython receiver ([GFSK and Bluetooth LE](gfsk.md));
- the Schmidl-Cox preamble search of [QAM and OFDM](qam-ofdm.md).

### Carrier offset

The two ends' crystals differ, so every receiver meets a carrier offset:

- the 802.15.4 discriminator is only biased by it
  ([IEEE 802.15.4](ieee802154.md#o-qpsk-as-msk)); the pcap decoder estimates it from the decoded
  chips ([Burst detection](bursts.md#at-2-mss));
- the MicroPython receiver takes it from the access code ([GFSK and Bluetooth LE](gfsk.md));
- QAM estimates it from the preamble and derives the sample clock offset from it; OFDM takes it
  from the preamble's phase and tracks the residual from its pilots
  ([QAM and OFDM](qam-ofdm.md#carrier-offset-and-sample-clock-drift)).

### Timing

Sampling each symbol at the right instant:

- two phase trackers in [IEEE 802.15.4](ieee802154.md#receive-chain);
- the slicer's sampling phase and early/late sums in [GFSK and Bluetooth LE](gfsk.md);
- the fine timing and the sample clock drift correction of OFDM
  ([QAM and OFDM](qam-ofdm.md#receiver)).

### Framing

Finding where a frame's bits start and what they mean:

- the SFD, PHR and length byte of [IEEE 802.15.4](ieee802154.md#receive-chain);
- the access address, dewhitening and CRC of Bluetooth LE
  ([GFSK and Bluetooth LE](gfsk.md#bluetooth-le-from-channelizer-captures));
- the link's headers with their CRC-16 ([QAM and OFDM](qam-ofdm.md)).

### Unknown signals

Describing what no decoder knows: centre, bandwidth, modulation and symbol rate, in the generic
analysis of [Burst detection](bursts.md#generic-analysis).

### Error correction

Recovering from bit errors, or at least detecting them: the codes of
[Error correction](fec.md), and the CRCs every framing above ends with.

## Implementations

| Page | Technique | Code | Chips |
| --- | --- | --- | --- |
| [Channelizer](channelizer.md) | polyphase filter bank, per-channel decimators, power gates | `esp_sdr_chancap.h` ([chancap](../../apps/chancap/README.rst)) | ESP32-S3 |
| | fixed-point polyphase filterbank | `esdr_pfbq.h` ([osmosdr_bridge](../../apps/osmosdr_bridge/README.rst), [pcap](../../apps/pcap/README.rst)) | ESP32-P4 |
| [IEEE 802.15.4](ieee802154.md) | O-QPSK received as MSK, half-sine transmit | `lib/ieee802154` ([wpan](../../apps/wpan/README.rst)) | ESP32-S3 |
| [GFSK and Bluetooth LE](gfsk.md) | GFSK demodulator | `gfsk_demod` ([ubertooth](../../apps/ubertooth/README.rst)) | ESP32-S3 |
| | LE 1M from channel captures | `tools/ble_pcap.py` ([chancap](../../apps/chancap/README.rst)) | host |
| | GFSK burst receiver | `cmod/esdr_fsk.c` ([micropython](../../apps/micropython/README.rst)) | MicroPython boards |
| [Burst detection](bursts.md) | burst detectors, Bluetooth LE and 802.15.4 decoders, generic analysis | [pcap](../../apps/pcap/README.rst) | ESP32-P4 with the ESP32-C6 as radio |
| | the same detector and decoders | `cmod/esdr_pcap_core.c` ([micropython](../../apps/micropython/README.rst)) | MicroPython boards |
| | the same, every channel of a wideband stream | `tools/esdr_gr_pcap.py` ([pcap](../../apps/pcap/README.rst)) | host |
| [QAM and OFDM](qam-ofdm.md) | single-carrier QAM, OFDM, CSMA/CA with ARQ | `src/qam.c`, `src/ofdm.c`, `src/mac.c` ([link](../../apps/link/README.rst)) | ESP32-S3 |
| [Error correction](fec.md) | Reed-Solomon, extended Hamming, BCH, CRC-32 | `src/bch.c` and the frame code ([link](../../apps/link/README.rst)) | ESP32-S3 |

## Pages in this part

1. [Channelizer](channelizer.md): many channels from one wideband stream, with power gates that
   cut out one capture per packet.
2. [IEEE 802.15.4](ieee802154.md): a software O-QPSK radio, its driver and its ACKs.
3. [GFSK and Bluetooth LE](gfsk.md): the Ubertooth demodulator, LE 1M from channel captures, and
   the MicroPython GFSK receiver.
4. [Burst detection](bursts.md): detectors, decoders and the generic analysis that turn a stream
   into classified bursts.
5. [QAM and OFDM](qam-ofdm.md): a packet link with two PHYs and a MAC, and where its time goes.
6. [Error correction](fec.md): the link's codes, their decoders, and how to choose one.

Part 4, [Applications](../apps/index.md), describes the apps that use these pages.

<!-- nav-start -->
Previous: [Host transports](../samples/transports.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Channelizer](channelizer.md)
<!-- nav-end -->
