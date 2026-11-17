.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_stream:

######
stream
######

.. nav-start

Previous: `rfprobe <../rfprobe/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `osmosdr <../osmosdr/README.rst>`__

.. nav-end

A XIAO ESP32S3 as a small SDR on a USB network link: VITA 49.2 over UDP and IPv6, receive and
transmit.

Overview
********

The app turns a XIAO ESP32S3 into a small SDR on a USB network link. Raw I/Q from the Wi-Fi
receiver goes to the host as VITA 49.2 over UDP and IPv6, and VITA 49.2 from the host is
transmitted through the Wi-Fi DAC.

- **Receive**: CPU 0 captures bursts with the ``esp-sdr`` library, CPU 1 packs them with
  ``libvrt`` and sends them (SMP).

  - Raw bursts of 1404 samples at 16 MS/s.
  - Or a 3rd order CIC decimated stream at 16 MS/s / m (m 16 to 160), for a continuous low rate
    view of the band.

- **Transmit**: signal data packets are placed by their timestamps, interpolated linearly to
  40 MS/s and played gapless by the DAC in loop mode (see `Transmitting`_).
- **USB** on the OTG port: CDC-NCM network, CDC-ACM console and shell, DFU.

`Samples in and out <../../doc/samples/index.md>`__ follows this app's receive and transmit
paths from the antenna to the host sample.

Hardware
********

.. list-table::
   :header-rows: 1

   * - Board target
     - Chip
     - Console
     - Notes
   * - ``xiao_esp32s3/esp32s3/procpu``
     - ESP32-S3
     - CDC-ACM shell
     - two for a link, one transmits, one receives

Host requirements
=================

- Access to the USB device (VID 0x2fe3) for DFU: `Applications <../../doc/apps/index.md>`__ has
  the udev rules.
- An IPv6 link-local address on the board's network interface.
- UDP port 4991 open on it.

On NixOS:

.. code-block:: nix

   systemd.network.links."10-esp-sdr" = {
     matchConfig.MACAddress = "00:00:5e:00:53:01";
     linkConfig.Name = "esdr0";
   };
   networking.firewall.interfaces.esdr0.allowedUDPPorts = [ 4991 ];

Every board uses the same MAC address: with several boards, match the links by USB path instead
and keep them link-local only. The host tools find a board's interface from its USB serial
number.

Building and flashing
*********************

The app needs MCUboot (sysbuild) and the manual gain blob:

.. code-block:: console

   west blobs fetch esp-sdr-zephyr
   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/stream
   west flash

Later updates: ``west dfu -s <usb serial>`` (see `Applications <../../doc/apps/index.md>`__).

Transmit needs ``CONFIG_APP_TX``, which needs PSRAM and ``librftest.a``
(``CONFIG_ESP_SDR_TX``); without it the port serves the receiver's commands only.

Usage
*****

Receiving
=========

.. code-block:: console

   python3 esp-sdr-zephyr/apps/stream/tools/vrt_rx.py --iface esdr0 --seconds 10 --out iq.cs16

The script prints rates, decoded context and counter gaps, writes the samples as little endian
complex int16, and exits 0 when the stream is valid.

Receive commands
----------------

Command packets to the receive stream ID (UDP port 4992) set the receiver; query-state requests
return the settings:

.. list-table::
   :header-rows: 1

   * - Setting
     - Values
   * - RF reference frequency
     - whole MHz
   * - Gain
     - stage 1 is the PHY gain table index, not dB; stage 2 must be 0
   * - Sample rate
     - 16 MS/s, or 16 MS/s / m for decimation
   * - Payload format
     - 16, 12 or 8 bit items

USB full speed limits the raw receive stream to 0.24 MS/s at 16 bit, 0.32 MS/s at 12 bit and
0.48 MS/s at 8 bit (`Host transports <../../doc/samples/transports.md>`__ has the link's
capacity). Decimated streams are limited by the CIC on the capture CPU (about 30 % of the time
covered at 200 kS/s).

Transmitting
============

The board accepts VITA 49.2 packets on UDP port 4992. Command packets to the transmit stream ID
set the transmitter:

.. list-table::
   :header-rows: 1

   * - Setting
     - Values
   * - TX RF frequency
     - whole MHz; it is the receiver's LO too
   * - Sample rate
     - 40 MS/s divided by an integer of at least 2
   * - Gain
     - stage 1 is the power step, 0 weakest (see ``sdr set txgain``)
   * - Payload format
     - as for receive

- Errors are answered with execution acknowledges; query-state requests return the settings.
- Signal data packets are placed by their timestamps; missing packets become zero filled holes.
- TX starts with the first data packet and stops 500 ms after the last.

Measured on air (``sdr status``): 100 % at 16 bit / 200 kS/s, 12 bit / 250 kS/s and 8 bit /
400 kS/s, which is also where USB full speed tops out. `Transmit <../../doc/samples/transmit.md>`__
describes the DAC stream behind it.

Host tools
==========

``tools/esp_sdr_lab.py`` drives two boards (by USB serial) and records the receiver:

.. code-block:: shell

   # tone or chirp from A to B, decimated capture at B, cs16 + json on a real time axis
   python3 esp_sdr_lab.py link --tx <serial> --rx <serial> --signal chirp --offset 0 \
       --span 80000 --tx-rate 250000 --tx-bits 12 --rx-bits 12 --out chirp.cs16
   # spectrum painter: a picture on the receiver's waterfall, also saved as picture_rx.png
   python3 esp_sdr_lab.py link --tx <serial> --rx <serial> --signal image --image picture.png \
       --offset 0 --tx-rate 250000 --tx-bits 12 --rx-bits 12 --out picture_rx.cs16
   # the same picture to an IQ file, no boards
   python3 esp_sdr_lab.py paint picture.png --out picture.cs16

``tools/vrt_tx.py`` checks the transmit control and data handling on its own.

Shell
=====

The shell is on the CDC-ACM port (``/dev/ttyACM*``):

- ``sdr status``: settings, transmit path state and counters.
- ``sdr set <key> <value> ...``: ``freq <MHz>``, ``rxgain auto|<index>``, ``decim cic|fold``,
  ``txgain <step>``; ``sdr set`` lists them.
- ``sdr tonetx``, ``sdr sweep``, ``sdr mem``.
- ``net iface``, ``mcuboot``, ``kernel reboot cold``.

Output format
*************

Packets
=======

- **Signal data, receive**: stream ID 0x5D5D0001, timestamps (seconds since boot plus
  picoseconds), a trailer with sample loss at each burst start and over-range.
- **Context** (VITA 49.2, CIF1 compliance field): RF frequency, sample rate, bandwidth and
  payload format, at start, on change and once per second.
- **Signal data, transmit**: stream ID 0x5D5D0101 (see `Transmitting`_).
- **Command**: to the receive or the transmit stream ID on UDP port 4992 (see `Receiving`_ and
  `Transmitting`_).

Sample formats
==============

Link efficient complex signed fixed point with 16, 12 or 8 bit items, chosen by the host per
direction (payload format field).

Sample output
=============

.. code-block:: console

   vrt_rx: stream 0x5d5d0001 from fe80::ff:fea5:ae0
   vrt_rx: context VITA 49.2, rf 2412.000 MHz, rate 16.000 MS/s, bandwidth 16.000 MHz
   vrt_rx: 597 data packets/s, 829.7 kB/s, 0.204 MS/s, -25.2 dBFS, gaps 0, bad 0
   ...
   vrt_rx: 3570 data, 6 context packets, 1218560 samples, 0 gaps, 0 bad: PASS

See also
********

- `Samples in and out <../../doc/samples/index.md>`__: the receive and transmit chain of this app.
- `Host transports <../../doc/samples/transports.md>`__: VITA 49.2 over CDC-NCM among the other
  links.
- `Transmit <../../doc/samples/transmit.md>`__: the DAC stream.
- `osmosdr <../osmosdr/README.rst>`__: the same radio as a USB SDR for GNU Radio hosts.

.. nav-start

Previous: `rfprobe <../rfprobe/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `osmosdr <../osmosdr/README.rst>`__

.. nav-end
