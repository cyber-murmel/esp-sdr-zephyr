.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_wpan:

####
wpan
####

.. nav-start

Previous: `link <../link/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `zstack <../zstack/README.rst>`__

.. nav-end

An IEEE 802.15.4 test shell for the ESP32-C6 and the ESP32-S3: it sends and receives test frames
through any Zephyr 802.15.4 radio driver.

Overview
********

The ``wpan`` shell compares radios against each other: the C6's native radio and the S3's
``esp-sdr`` software radio, whose PHY and driver are in
`IEEE 802.15.4 <../../doc/modem/ieee802154.md>`__. Both send and count the same test frames
(`Test frames`_).

Hardware
********

.. list-table::
   :header-rows: 1

   * - Chip
     - Board
     - Radio
   * - ESP32-C6
     - XIAO ESP32C6
     - the native radio (``ieee802154_esp32``) in raw mode
   * - ESP32-S3
     - XIAO ESP32S3
     - the ``esp-sdr`` software radio (``CONFIG_ESP_SDR_IEEE802154``, device ``esp_sdr_154``),
       without the L2; frames come through ``ieee154_esp_sdr_set_rx_cb()``

Building and flashing
*********************

ESP32-C6, native radio, over USB-Serial-JTAG:

.. code-block:: console

   west build -b xiao_esp32c6/esp32c6/hpcore esp-sdr-zephyr/apps/wpan
   west flash

ESP32-S3, software radio, with the shell on the OTG port and DFU updates as the other apps
(`Applications <../../doc/apps/index.md>`__, "DFU updates"):

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/wpan
   west flash                    # first install; later: west dfu -s <usb serial>

The S3 build uses the gapless ring receive (three dump banks), so its thread stacks and tables
live in ``ESP_SDR_HIGH_RAM`` (`Code placement <../../doc/dev/code-placement.md>`__).

Usage
*****

Shell
=====

The commands follow the shared pattern (`Applications <../../doc/apps/index.md>`__, "Shell
conventions").

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``wpan status``
     - radio, settings, receive and transmit counters (and the software radio's counters on the
       S3, the ACK counters among them)
   * - ``wpan set [<key> <value> ...]``
     - settings, several at once; without arguments the keys (`Settings`_)
   * - ``wpan clear``
     - zero the receive and driver counters
   * - ``wpan on``, ``wpan off``
     - start or stop receiving (the S3 software radio starts off)
   * - ``wpan tx <count|0> [ms] [payload] [dst] [ack] [csma]``
     - send test frames (0: until ``wpan stop``)
   * - ``wpan stop``
     - stop a running ``tx``
   * - ``wpan measure``
     - one 1 ms capture: carrier offset, power, chip phase steps (S3)
   * - ``wpan dump``
     - one 1 ms capture as base64 I/Q, for ``tools/iq_dump.py`` (S3)
   * - ``wpan crash``
     - reset cause, driver event trail, stalled CPU 0, last crash (S3)
   * - ``wpan mem``
     - system heap use (with ``CONFIG_SYS_HEAP_RUNTIME_STATS``)
   * - ``wpan bench acktx [seq]``
     - one ACK on the air now (S3, receiving)
   * - ``wpan bench [units] [subsample]|phy|ack``
     - ring decimation by 4 and PHY cost on synthetic data, or the ACK waveform build and front end
       switch times (S3, radio off after one ``wpan on``)
   * - ``wpan bench dma [ack]``
     - the ACK tail (289 slots from slot 416) gathered from the templates into bank 2 by the GDMA
       (S3, ``CONFIG_ESP_SDR_DMA``, radio off but one ``wpan on`` since boot), compared word for word
       with the CPU build, plain and with 16 byte bursts. The SRAM, register and backup DMA benches
       are ``probe dma`` in `rfprobe <../rfprobe/README.rst>`__. Design and numbers:
       ``doc/dev/dma-chains.md``.
   * - ``sniffer start|stop|channel|speed|stats|stats_reset``
     - capture every frame as a text line for Wireshark (`Sniffer`_)

Settings
--------

.. list-table::
   :header-rows: 1

   * - Key
     - Values
     - Radio
   * - ``chan``
     - 11..26 (2405 + 5 (n - 11) MHz)
     - both
   * - ``txpower``
     - <dBm>
     - both
   * - ``promisc``
     - 0, 1
     - both
   * - ``pan``
     - <PAN ID>
     - both
   * - ``addr``
     - <short> (auto-ACK on both radios)
     - both
   * - ``print``
     - 0, 1, 2: nothing, a line, a line and a hex dump per frame
     - both
   * - ``rxgain``
     - auto, <index>
     - S3
   * - ``rxinvert``
     - 0, 1
     - S3
   * - ``txinvert``
     - 0, 1
     - S3
   * - ``lo``
     - below, above
     - S3
   * - ``unit``
     - <pairs>: the ring unit length, default 1024
     - S3
   * - ``ack``
     - <delay us>[,<limit us>]: the ACK timing, 192,512
     - S3

Example session
===============

The C6 sends, the S3 receives on channel 15:

.. code-block:: none

   s3:~$ wpan set chan 15 print 0
   s3:~$ wpan on
   c6:~$ wpan set chan 15
   c6:~$ wpan tx 1000 5 16
   s3:~$ wpan status

Sniffer
=======

The ``sniffer`` commands make any of the boards a one-channel 802.15.4 sniffer for Wireshark.
The firmware commands, their replies and the capture lines match Zephyr's
``samples/net/ieee802154/sniffer`` (Zephyr ``cf45cd71868``), so that sample's extcap also works with
these boards.

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
     - Reply
   * - ``sniffer start``
     - promiscuous mode, radio on, capture lines on
     - ``started``, ``already started``
   * - ``sniffer stop``
     - capture lines off, radio off (``wpan on`` turns it back on)
     - ``stopped``, ``already stopped``
   * - ``sniffer channel [<n>]``
     - the channel, or a new one (11..26) while stopped
     - ``<n>``, ``channel <n>``
   * - ``sniffer speed [<baud>]``
     - nothing: both ports are USB, where the rate has no effect; kept for upstream's extcap
     - ``speed <baud>``
   * - ``sniffer stats``
     - frames queued, dropped (queue full), printed, bytes printed, queue high-water mark and use
     - ``enq=.. drop=.. tx_rec=.. tx_bytes=.. q_hwm=.. q_used=../..``
   * - ``sniffer stats_reset``
     - zero those counters
     - ``stats reset``

Capture is off at boot. Each frame then comes out on the shell's port as one line:

.. code-block:: none

   psdu: <hex> power: <rssi dBm> lqi: <lqi> time: <us since boot>

- ``psdu`` is the frame without its FCS (the radios check it and drop bad frames); the host tool
  appends a recomputed one.
- ``power`` is ``-32768`` when the radio has no RSSI: always on the S3's software radio.
- ``time`` is the C6 driver's receive timestamp. The S3's software radio has none, so its frames
  carry the time they reached the app, a few hundred microseconds after the frame ended.
- The lines go through the shell, so a prompt or terminal codes can precede ``psdu:``; the host
  tool searches for the pattern and ignores everything else.
- While capturing, ``wpan set print`` lines are left out; the ``wpan status`` counters keep
  counting.
- Frames the port cannot keep up with are dropped at the queue and counted as ``drop`` in
  ``sniffer stats``; compare ``enq`` with ``wpan status``'s received frames.

Measured on an ESP32-S3-DevKitC (N16R8) and a XIAO ESP32C6, each sending ``wpan tx`` test frames
to the other on channel 15 (2026-11-23):

- S3 sniffing: 200 of 200 frames of 31 bytes at 100 frames/s in the pcap. With 111-byte frames
  every 5 ms, a nearly full channel, the radio took all 400 but the CDC-ACM port printed 211: some
  25 KB/s of lines, about 100 longest frames a second, with the PHY busy on the same CPU.
- C6 sniffing: every frame the radio took reached the pcap (queue high-water mark 0 to 1), RSSI
  included.
- The shell's TX ring is 1024 bytes (``prj.conf``): with Zephyr's default 8 the S3 printed only
  161 of 199 frames at 100 frames/s.

Wireshark
---------

``tools/wpan_sniffer.py`` is the extcap: it sends the ``sniffer`` commands, reads the lines and
writes pcap with the 802.15.4 TAP link type (283), carrying channel, RSSI and LQI. It is Zephyr's
``scripts/ieee802154_sniffer.py`` with these changes:

- On Linux it opens the port raw, without touching DTR and RTS: pySerial's open resets a C6 on
  USB-Serial-JTAG.
- It lists only these boards: ``wpan sniffer, ESP32-S3 software radio`` for the S3's CDC-ACM port,
  ``wpan sniffer, USB-Serial-JTAG`` for a C6, whose port has the same USB IDs as any Espressif
  chip's USB-Serial-JTAG. The WHY2025 badge's C6 appears there once the P4 has handed J8 to it
  (``esdr.usb_to_c6()``).
- The capture baud rate defaults to 0 (unchanged) and the channel to 11.

Install it into Wireshark's personal extcap folder (Help > About Wireshark > Folders):

.. code-block:: console

   ln -s $PWD/esp-sdr-zephyr/apps/wpan/tools/wpan_sniffer.py ~/.local/lib/wireshark/extcap/

It needs Python 3.10 and pySerial. Standalone, for example on channel 15 into a file:

.. code-block:: console

   esp-sdr-zephyr/apps/wpan/tools/wpan_sniffer.py --capture \
       --extcap-interface /dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_802.15.4_<serial>-if00 \
       --channel 15 --log-file sniffer.log --fifo capture.pcap

``src/sniffer.c``, ``src/sniffer_line.c`` and ``tools/wpan_sniffer.py`` come from that Zephyr
sample and keep its Apache-2.0 licence.

Output format
*************

Test frames
===========

- Test frames are data frames (PAN ID compression, short addresses).
- Their payload starts with the magic ``X154`` and a 32-bit counter, followed by a pattern derived
  from the counter.
- The receiver checks the pattern and counts frames, test frames, corrupt ones, duplicates and
  counter gaps (lost frames).

See also
********

- `IEEE 802.15.4 <../../doc/modem/ieee802154.md>`__: the software radio's PHY, driver and ACKs.
- `Testing <../../doc/dev/testing.md>`__: the regression tests that use these frames.
- `zstack <../zstack/README.rst>`__: a Zigbee coordinator on the C6's radio.
- `Applications <../../doc/apps/index.md>`__: shell conventions, DFU updates and USB access.

.. nav-start

Previous: `link <../link/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `zstack <../zstack/README.rst>`__

.. nav-end
