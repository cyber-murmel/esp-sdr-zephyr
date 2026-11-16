.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_pcap:

pcap (WHY2025 badge)
####################

Overview
********

The WHY2025 badge's ESP32-P4 demodulates and decodes what its ESP32-C6 radio receives and sends
it to the computer over USB as a pcapng stream for Wireshark. The design and its measurements are
in ``doc/pcap.md``.

- **Known protocols arrive as themselves**: Bluetooth LE packets on
  ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`` (256: RF channel, access address and its bit errors,
  CRC checked and valid, LE 1M or 2M), IEEE 802.15.4 frames on ``LINKTYPE_IEEE802_15_4_TAP``
  (283: channel, frequency, LQI, the FCS for Wireshark to check). Each packet carries the
  signal's metadata as a pcapng comment (``frame.comment``): frequency, channel, rate, carrier
  offset, power and noise floor in dBFS, SNR, gain, duration.
- **Every other burst** arrives as a record of ``LINKTYPE_USER0`` (147, ``struct pcap_burst_hdr``
  of ``src/pcap_proto.h``): frequency, carrier offset, occupied bandwidth, duration, modulation
  (CW, OOK, 2FSK, 4FSK, MSK, BPSK, QPSK, wideband), symbol rate, deviation, power, floor and
  peak, and the bits sliced at the estimated rate, all best effort. ``tools/esdr_burst.lua``
  dissects it (fields ``esdr_burst.*``). Signals wider than several channels (Wi-Fi) come as a
  record each, without bits. The analysis runs in the time the decoders leave: a burst filling
  its channel comes as wideband unanalysed, one it has no time for with the detector's metadata
  only (flag "Not analysed").
- **Modes**, chosen per capture, alternating, or following 802.15.4:

  - the **survey** (default): the C6's gapless 80 MS/s receive, pruned to the 10 us windows that
    stand out, through the P4's 40-channel filterbank (2 MHz, 4 MS/s a channel) and a burst
    detector per channel. It covers +-40 MHz around the centre, but the C6 keeps only some
    50 us of a burst whole (what it can copy per 154 us unit): it finds and describes bursts
    across the band, and decodes the short packets that survive whole. At 40 MS/s
    (``--survey-rate 40``, ``pcap rate 40``) it covers +-20 MHz with 20 channels of the same width
    (the edge bin at +-20 MHz left out; not with 16 channels) and keeps bursts some three times
    longer: Bluetooth LE packets decode there, some 10 a second at the desk, where the 80 MS/s
    survey decodes none. What lies beyond 20 MHz out folds in, a few dB down near 20 MHz and 15
    to 25 dB down from 25 MHz on;
  - the **channel** mode: one channel streamed without a gap by the C6's paced receive at
    2 MS/s: complete Bluetooth LE 1M packets (an advertising channel: 2402, 2426 or 2480 MHz)
    or 802.15.4 frames (a channel 2405 + 5 (n - 11) MHz, read at 4 MS/s through an
    interpolator) of that channel;
  - **following 802.15.4** (``--mode follow``, ``pcap start follow``): the survey until it finds
    the start of an 802.15.4 frame, then the channel mode on that channel until it has been quiet
    for ``--channel-ms`` (at most 4 times that), then at least ``--survey-ms`` of the survey again;
    a channel other than the last one goes first. Whole frames of the networks in range, without
    knowing their channels (``doc/pcap.md``, "Following 802.15.4").

- **USB**: the pcapng stream on a vendor bulk IN endpoint (high speed), the shell on CDC-ACM,
  DFU into MCUboot's secondary slot (``west dfu``), VID 0x2fe3, PID 0x0005, product "ESP-SDR
  pcap".

On the bench (2026-10-27, an office): the channel mode on advertising channel 37 decoded 2346
Bluetooth LE packets in 15 s, every CRC good, 0 of 4568 bursts lost; the survey turned some 10 000
to 20 000 bursts in 15 s into records (most of them wideband), 83 to 91 cycles a pair in the
filterbank. On the WHY2025 badge the P4 runs both cores: the channelizer has CPU 1, the reader, USB
and the decoders CPU 0. A 40 MS/s survey there lost 16 and 0 of some 40 000 bursts in 30 s and
decoded 325 to 353 Bluetooth LE packets (on one core: a fifth of the bursts lost, 295 to 314
packets); the channelizer used half of CPU 1, and a second generic analysis worker there takes the
rest (twice the bursts analysed; doc/pcap.md).

The radio
*********

The C6 runs :ref:`osmosdr_serial <esp_sdr_osmosdr_serial>` over SDIO, its default build on the
badge (gapless receive in it). Its width sorting (``ESDR_GAPLESS_F_WIDE``, the ``--c6-wide``
option) lets the C6 send a short record in place of a wide signal's samples; it is off by
default (at the desk it found few wide windows and cost the C6 5 to 12 % of its CPU).

Building and flashing
*********************

Sysbuild with MCUboot, in the flash layout of :ref:`micropython <esp_sdr_micropython>` on the
P4: the two apps update each other over DFU and keep its FAT volume.

.. code-block:: console

   west build --sysbuild -b why2025_badge/esp32p4/hpcore esp-sdr-zephyr/apps/pcap
   west dfu -d build --serial <badge serial>

From MicroPython or this app, ``west dfu`` downloads into the secondary slot and confirms the
new image through the shell. The PIE kernels of the filterbank need Espressif's assembler
(``riscv32-esp-elf-as``, in the dev shell of ``default.nix``).

Wireshark
*********

``tools/esdr_pcap.py`` is a Wireshark extcap and a standalone tool (pyusb). Link it into
Wireshark's personal extcap folder and the dissector into its personal Lua plugins (Help >
About > Folders shows both; on Linux typically ``~/.local/lib/wireshark/extcap`` and
``~/.local/lib/wireshark/plugins``):

.. code-block:: console

   ln -s $PWD/esp-sdr-zephyr/apps/pcap/tools/esdr_pcap.py ~/.local/lib/wireshark/extcap/
   ln -s $PWD/esp-sdr-zephyr/apps/pcap/tools/esdr_burst.lua ~/.local/lib/wireshark/plugins/

Every badge is then an interface ``esdr-pcap-<serial>``; its dialog sets the mode, the
frequencies, the gain (automatic: the C6's gain servo in the survey, gain 60 on the channel),
the survey's rate, the channelizer, the C6's gapless threshold and windows, the width sorting and
the decoders.
Without Wireshark:

.. code-block:: console

   esdr_pcap.py --list
   esdr_pcap.py -w capture.pcapng --mode channel --channel-freq 2402
   esdr_pcap.py -w - | wireshark -k -i -

The badge's own clock times the packets, anchored to the host's time (``PCAP_REQ_SET_TIME``) at
the first block of a run: a few milliseconds off the host's, consistent within the run.

Every channel of a wideband stream
**********************************

``tools/esdr_gr_pcap.py`` writes the same records on the host, for every channel of a 20 MS/s
stream at once: from :ref:`osmosdr_hs <esp_sdr_osmosdr_hs>` (an ESP32-S31, through the esdr
SoapySDR module) or a recording of one. GNU Radio cuts each Bluetooth LE and 802.15.4 channel
within 8 MHz of the centre out of the stream, a frequency translating filter each (4 MS/s, flat to
1 MHz, 50 dB down from 1.6 MHz), and each channel goes through this app's detector and decoders:
the Pcap egg's capture core (``apps/micropython/cmod/esdr_pcap_core.c``), built with the host
compiler on the first run and cached in ``$XDG_CACHE_HOME/esdr-pcap``.

What the channels see of each other is sorted out before the write:

- an access address a CONNECT_IND taught one channel is followed on all of them;
- a burst record of a packet another channel decoded goes: the two grids' channels are 1 MHz
  apart where they meet, and an 802.15.4 frame reaches into channels 3 MHz away;
- of two channels' records of one signal (at one frequency, or between them from the same
  moment) the one nearest its channel's centre stays, the stronger where neither was analysed;
- a wide signal (Wi-Fi) becomes one wideband record over its span and time. A weak one lifts the
  channels it covers over their floors unevenly and in pieces of some 30 us: records that three
  channels, or two far apart, start at once open it, and the pieces within its span join it until
  it has been quiet for 100 us.

``--no-merge`` writes what each channel saw.

.. code-block:: console

   python3 esp-sdr-zephyr/apps/pcap/tools/esdr_gr_pcap.py -f 2409M -w capture.pcapng
   python3 esp-sdr-zephyr/apps/pcap/tools/esdr_gr_pcap.py -f 2409M -w - | wireshark -k -i -
   esdr_usb rx -f 2409000000 -r 20000000 -t 10 -o rec.cs8
   python3 esp-sdr-zephyr/apps/pcap/tools/esdr_gr_pcap.py --file rec.cs8 -f 2409M -w rec.pcapng

The default centre, 2409 MHz, holds advertising channel 37, data channels 0 to 6 and 802.15.4
channels 11 to 13, none of them at DC. ``python3`` of the dev shell (``default.nix``) has GNU Radio
and the SoapySDR module. The generic analysis runs up to ``--analyses`` times a second (1000), the
other bursts go out as their detector's numbers.

On the VIEWE board and a laptop (Intel i7-12700H), at the desk, 2026-11-10: ten channels kept up
with the stream, no gap and nothing dropped in 30 s. At 2427 MHz 30 s gave 3868 Bluetooth LE
packets on advertising channel 38, every CRC good, and 92 frames of the 802.15.4 network on
channel 15 (Thread MLE and ZigBee among them). The channels closed 127 000 bursts, which went out
as 16 962 records: 12 222 wideband (Wi-Fi), 3763 unknown, 629 CW, 318 2-FSK, 29 OOK, 1 BPSK. On a
recording the same ten channels decode 1.8 times as fast as the stream comes; GNU Radio's filters
take most of the time, each on a core of its own.

A stream of 20 MS/s never holds two channels 24 MHz or more apart, advertising channels 37 and 38
among them. In its outer 2 MHz either side may show what the analog low-pass lets through from
beyond 10 MHz, folded (``--edge`` narrows the band).

Shell
*****

On the CDC-ACM port, for the bench without the host tool:

.. code-block:: console

   pcap start [survey|channel|alt|follow [survey_ms channel_ms]]
   pcap stop
   pcap freq <MHz>          the survey's centre
   pcap chfreq <MHz>        the channel mode's frequency
   pcap gain <index|auto>
   pcap chan <16|32|40|64|80> [open dB]
   pcap ratio <ratio>       the C6's gapless threshold floor
   pcap wide <0|1>          the C6's width sorting (80 MS/s only)
   pcap rate <80|40>        the survey's rate
   pcap dec <hex>           decoders: ble 1, wpan 2, generic 4, wideband 8, bad CRC 10,
                            ble data 20, comments 40
   pcap stats

Logs go to the console UART (the CH340).
