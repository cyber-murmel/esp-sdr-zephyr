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
