.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_pcap:

####
pcap
####

On the WHY2025 badge, the ESP32-P4 demodulates and decodes what its ESP32-C6 radio receives and
sends it to the computer over USB as a pcapng stream for Wireshark.

Overview
********

- Known protocols arrive as themselves: Bluetooth LE packets and IEEE 802.15.4 frames, with the
  signal's metadata in a comment (`Output format`_).
- Every other burst arrives as a record of its own, with its modulation and parameters, best
  effort.
- The modes survey the band, stream one channel without a gap, or follow 802.15.4 (`Modes`_).
- USB: the pcapng stream on a vendor bulk IN endpoint (high speed), the shell on CDC-ACM, DFU
  into MCUboot's secondary slot (``west dfu``); VID 0x2fe3, PID 0x0005, product "ESP-SDR pcap".

The design and its measurements are in `pcap internals <../../doc/apps/pcap-internals.md>`__.

Hardware
********

The WHY2025 badge: its ESP32-P4 runs this app, its ESP32-C6 is the radio.

The radio
=========

- The C6 runs `osmosdr_serial <../osmosdr_serial/README.rst>`__ over SDIO, its default build on
  the badge (gapless receive in it).
- Its width sorting (``ESDR_GAPLESS_F_WIDE``, the ``--c6-wide`` option) lets the C6 send a short
  record in place of a wide signal's samples.
- Width sorting is off by default: at the desk it found few wide windows and cost the C6 5 to
  12 % of its CPU.

Building and flashing
*********************

Sysbuild with MCUboot, in the flash layout of `micropython <../micropython/README.rst>`__ on the
P4: the two apps update each other over DFU and keep its FAT volume.

.. code-block:: console

   west build --sysbuild -b why2025_badge/esp32p4/hpcore esp-sdr-zephyr/apps/pcap
   west dfu -d build --serial <badge serial>

- From MicroPython or this app, ``west dfu`` downloads into the secondary slot and confirms the
  new image through the shell (`Applications <../../doc/apps/index.md>`__, "DFU updates").
- The PIE kernels of the filterbank need Espressif's assembler (``riscv32-esp-elf-as``, in the dev
  shell of ``default.nix``).

Usage
*****

Modes
=====

A capture runs one mode, alternates between two, or follows 802.15.4:

.. list-table::
   :header-rows: 1

   * - Mode
     - Constant
     - Host tool
     - Shell
   * - Survey (default)
     - ``PCAP_MODE_SURVEY``
     - default
     - ``pcap start survey``
   * - Channel
     - ``PCAP_MODE_CHANNEL``
     - ``--mode channel``
     - ``pcap start channel``
   * - Alternating
     - ``struct pcap_mode_cfg``
     -
     - ``pcap start alt``
   * - Following 802.15.4
     - ``PCAP_MODE_F_FOLLOW``
     - ``--mode follow``
     - ``pcap start follow``

Survey
------

- The C6's gapless 80 MS/s receive, pruned to the 10 us windows that stand out, goes through the
  P4's fixed-point polyphase filterbank (``apps/common/esdr_pfbq.h``, PIE SIMD) at M = 40.
- That makes 2 MHz channels at 4 MS/s, the widest of the decoded protocols, with a burst detector
  per channel.
- The survey finds and describes bursts across the band, and decodes the short packets that
  survive whole. How much of a burst the C6 keeps whole is in
  `Gapless receive <../../doc/samples/rings.md>`__.

At 40 MS/s (``--survey-rate 40``, ``pcap rate 40``, ``PCAP_MODE_F_HALF``):

.. list-table::
   :header-rows: 1

   * -
     - 80 MS/s
     - 40 MS/s
   * - Span
     - +-40 MHz around the centre
     - +-20 MHz
   * - Channels
     - 40 (M = 40)
     - 20 of the same width (M = 20), the edge bin at +-20 MHz left out; not with 16 channels
   * - Bursts kept
     - what the C6 can copy per unit
     - some three times longer
   * - Bluetooth LE at the desk
     - none decoded
     - some 10 packets a second
   * - Beyond the span
     -
     - folds in: a few dB down near 20 MHz out, 15 to 25 dB down from 25 MHz on

Channel
-------

- One channel streamed without a gap by the C6's paced receive at 2 MS/s (``ESDR_MODE_RX``, 8-bit
  items, the LO on the channel, the DC taken out on the C6), and a burst detector over it.
- It gets complete Bluetooth LE 1M packets of an advertising channel (2402, 2426 or 2480 MHz).
- Or 802.15.4 frames of a channel 2405 + 5 (n - 11) MHz, read at 4 MS/s through an interpolator.

Alternating
-----------

The capture alternates between the survey and the channel mode with dwell times
(``struct pcap_mode_cfg``); a switch costs some 15 ms of the C6.

Following 802.15.4
------------------

#. The survey runs until it finds the start of an 802.15.4 frame.
#. Then the channel mode runs on that channel while its frames keep coming: until it has been
   quiet for ``--channel-ms`` (at most 4 times that).
#. Then at least ``--survey-ms`` of the survey again; a channel other than the last one goes
   first.

This gets whole frames of the networks in range without knowing their channels. The sighting
rule is in `Burst detection <../../doc/modem/bursts.md>`__, the rest and the measurements in
`pcap internals <../../doc/apps/pcap-internals.md>`__.

Wireshark
=========

Installing
----------

``tools/esdr_pcap.py`` is a Wireshark extcap and a standalone tool (pyusb). Link it into
Wireshark's personal extcap folder and the dissector into its personal Lua plugins (Help > About >
Folders shows both; on Linux typically ``~/.local/lib/wireshark/extcap`` and
``~/.local/lib/wireshark/plugins``):

.. code-block:: console

   ln -s $PWD/esp-sdr-zephyr/apps/pcap/tools/esdr_pcap.py ~/.local/lib/wireshark/extcap/
   ln -s $PWD/esp-sdr-zephyr/apps/pcap/tools/esdr_burst.lua ~/.local/lib/wireshark/plugins/

Capturing
---------

- Every badge is then an interface ``esdr-pcap-<serial>``.
- Its dialog sets the mode, the frequencies, the gain (automatic: the C6's gain servo in the
  survey, gain 60 on the channel), the survey's rate, the channelizer, the C6's gapless threshold
  and windows, the width sorting and the decoders.
- The extcap sends the options as requests, then copies bulk IN in whole pcapng blocks
  (resynchronising after a torn read) until Wireshark stops it.
- ``tools/esdr_burst.lua`` dissects the burst record (``esdr_burst.*``). It reads by the record's
  header length, so later versions with more fields still parse.

Without Wireshark
-----------------

.. code-block:: console

   esdr_pcap.py --list
   esdr_pcap.py -w capture.pcapng --mode channel --channel-freq 2402
   esdr_pcap.py -w - | wireshark -k -i -

Every channel of a wideband stream
==================================

The host tool
-------------

- ``tools/esdr_gr_pcap.py`` writes the same records on the host, for every channel of a 20 MS/s
  stream at once: from `osmosdr_hs <../osmosdr_hs/README.rst>`__ (an ESP32-S31, through the esdr
  SoapySDR module) or a recording of one.
- GNU Radio cuts each Bluetooth LE and 802.15.4 channel within 8 MHz of the centre out of the
  stream: a frequency translating filter each (4 MS/s, flat to 1 MHz, 50 dB down from 1.6 MHz).
- Each channel goes through this app's detector and decoders: the Pcap egg's capture core
  (``apps/micropython/cmod/esdr_pcap_core.c``), built with the host compiler on the first run and
  cached in ``$XDG_CACHE_HOME/esdr-pcap``.

Merging the channels
--------------------

What the channels see of each other is sorted out before the write:

- an access address a CONNECT_IND taught one channel is followed on all of them;
- a burst record of a packet another channel decoded goes: the two grids' channels are 1 MHz
  apart where they meet, and an 802.15.4 frame reaches into channels 3 MHz away;
- of two channels' records of one signal (at one frequency, or between them from the same moment)
  the one nearest its channel's centre stays, the stronger where neither was analysed;
- a wide signal (Wi-Fi) becomes one wideband record over its span and time. A weak one lifts the
  channels it covers over their floors unevenly and in pieces of some 30 us: records that three
  channels, or two far apart, start at once open it, and the pieces within its span join it until
  it has been quiet for 100 us.

``--no-merge`` writes what each channel saw.

Commands
--------

.. code-block:: console

   python3 esp-sdr-zephyr/apps/pcap/tools/esdr_gr_pcap.py -f 2409M -w capture.pcapng
   python3 esp-sdr-zephyr/apps/pcap/tools/esdr_gr_pcap.py -f 2409M -w - | wireshark -k -i -
   esdr_usb rx -f 2409000000 -r 20000000 -t 10 -o rec.cs8
   python3 esp-sdr-zephyr/apps/pcap/tools/esdr_gr_pcap.py --file rec.cs8 -f 2409M -w rec.pcapng

- The default centre, 2409 MHz, holds advertising channel 37, data channels 0 to 6 and 802.15.4
  channels 11 to 13, none of them at DC.
- ``python3`` of the dev shell (``default.nix``) has GNU Radio and the SoapySDR module.
- The generic analysis runs up to ``--analyses`` times a second (1000); the other bursts go out as
  their detector's numbers.

Measured, 2026-11-10
--------------------

On the VIEWE board and a laptop (Intel i7-12700H), at the desk:

- Ten channels kept up with the stream, no gap and nothing dropped in 30 s.
- At 2427 MHz, 30 s gave 3868 Bluetooth LE packets on advertising channel 38, every CRC good, and
  92 frames of the 802.15.4 network on channel 15 (Thread MLE and ZigBee among them).
- On a recording the same ten channels decode 1.8 times as fast as the stream comes; GNU Radio's
  filters take most of the time, each on a core of its own.

The channels closed 127 000 bursts, which went out as 16 962 records:

.. list-table::
   :header-rows: 1

   * - Class
     - Records
   * - wideband (Wi-Fi)
     - 12 222
   * - unknown
     - 3763
   * - CW
     - 629
   * - 2-FSK
     - 318
   * - OOK
     - 29
   * - BPSK
     - 1

Band edges
----------

- A stream of 20 MS/s never holds two channels 24 MHz or more apart, advertising channels 37 and
  38 among them.
- In its outer 2 MHz either side may show what the analog low-pass lets through from beyond
  10 MHz, folded (``--edge`` narrows the band).

Shell
=====

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

Output format
*************

Sections and interfaces
=======================

One pcapng section per capture (``src/output.c``, ``src/pcapng.c``): an SHB and three IDBs in the
order of ``PCAP_IF_*``, with ``if_tsresol`` 9.

.. list-table::
   :header-rows: 1

   * - IDB
     - Link type
     - In the link type's header
   * - 0
     - ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`` (256)
     - RF channel, access address and its bit errors, CRC flags (checked and valid), PHY (LE 1M
       or 2M)
   * - 1
     - ``LINKTYPE_IEEE802_15_4_TAP`` (283)
     - channel, frequency, LQI, FCS type; the FCS for Wireshark to check
   * - 2
     - ``LINKTYPE_USER0`` (147)
     - the burst record (`Burst records`_)

- An EPB per packet, and an ISB per interface every second (received and dropped).
- A host that comes during a capture gets the section's header first.

Metadata comments
=================

- What does not fit the link type's header goes into a pcapng comment (``frame.comment``,
  ``src/records.c``): frequency, channel, rate, PHY, carrier offset, power and noise floor in
  dBFS, SNR, gain, duration.
- Power is dBFS, not dBm: the radio is not calibrated, so the dBm fields stay unset.

Burst records
=============

- A record is ``struct pcap_burst_hdr`` of ``src/pcap_proto.h``: frequency, carrier offset,
  occupied bandwidth, duration, modulation, symbol rate, deviation, power, floor and peak, and
  the bits sliced at the estimated rate, all best effort.
- The modulation is one of CW, OOK, 2FSK, 4FSK, MSK, BPSK, QPSK and wideband; the classification
  is in `Burst detection <../../doc/modem/bursts.md>`__.
- Wireshark shows it through the dissector (`Capturing`_).
- Signals wider than several channels (Wi-Fi) come as a record each, without bits.
- The analysis runs in the time the decoders leave: a burst filling its channel comes as wideband
  unanalysed, one it has no time for with the detector's metadata only (flag "Not analysed").

Transfers and timestamps
========================

- 4 KiB transfer buffers in internal RAM go out when full or after 50 ms, each ending the host's
  read (a zero-length packet after a whole number of packets).
- A block that does not fit is dropped and counted, never waited for.
- Timestamps are nanoseconds: the stream's axis (the C6's 80 or 40 MS/s clock, or the channel's
  sample index), anchored to the host's time (``PCAP_REQ_SET_TIME``) at a run's first block.
- So the badge's own clock times the packets: a few milliseconds off the host's, consistent
  within the run.

Measurements
************

On the bench, 2026-10-27, an office:

.. list-table::
   :header-rows: 1

   * - Run
     - Result
   * - Channel mode, advertising channel 37, 15 s
     - 2346 Bluetooth LE packets, every CRC good; 0 of 4568 bursts lost
   * - Survey, 15 s
     - some 10 000 to 20 000 bursts turned into records (most of them wideband); 83 to 91 cycles
       a pair in the filterbank
   * - 40 MS/s survey, both cores, 30 s
     - 16 and 0 of some 40 000 bursts lost; 325 to 353 Bluetooth LE packets decoded
   * - 40 MS/s survey, one core, 30 s
     - a fifth of the bursts lost; 295 to 314 Bluetooth LE packets

- On the WHY2025 badge the P4 runs both cores: the channelizer has CPU 1, the reader, USB and the
  decoders CPU 0.
- The channelizer used half of CPU 1, and a second generic analysis worker there takes the rest
  (twice the bursts analysed).

The comparisons and the later runs are in `pcap internals <../../doc/apps/pcap-internals.md>`__.

See also
********

- `pcap internals <../../doc/apps/pcap-internals.md>`__: the P4 pipeline, the follow mode, USB and
  the badge measurements.
- `Burst detection <../../doc/modem/bursts.md>`__: detectors, classification and decoders.
- `Gapless receive <../../doc/samples/rings.md>`__: the C6's gapless 80 MS/s receive.
- `osmosdr_serial <../osmosdr_serial/README.rst>`__, `osmosdr_hs <../osmosdr_hs/README.rst>`__,
  `micropython <../micropython/README.rst>`__.
