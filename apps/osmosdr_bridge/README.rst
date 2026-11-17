.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_osmosdr_bridge:

##############
osmosdr_bridge
##############

The WHY2025 badge's ESP32-P4 as an osmosdr USB device on USB high speed, with the badge's
ESP32-C6 as the radio.

Overview
********

The WHY2025 badge's ESP32-P4 acts as an `osmosdr <../osmosdr/README.rst>`__ USB device on its
USB 2.0 high-speed OTG port (USB-C port J8). The badge's ESP32-C6, running
`osmosdr_serial <../osmosdr_serial/README.rst>`__ over SDIO, is the radio.

- The host sees the same vendor interface as the S3's osmosdr app (VID 0x2fe3, PID 0x0005),
  with 512-byte bulk packets.
- The Soapy module and ``../osmosdr/tools/esdr_usb`` work unchanged.
- Vendor requests and transmit samples go to the C6 as frames; the C6's receive blocks go out
  on bulk IN as they are. `Host transports <../../doc/samples/transports.md>`__ describes the
  bridging.
- Receive reaches 2 MS/s (8-bit items above 250 kS/s), transmit 250 kS/s: the C6's limits.

Hardware
********

.. list-table::
   :header-rows: 1

   * - Board target
     - Chip
     - Role
     - Ports
   * - ``why2025_badge/esp32p4/hpcore``
     - ESP32-P4
     - this app, the USB device
     - USB high speed on USB-C port J8; console and flashing on the CH340 (``/dev/ttyUSB0``)
   * - ``why2025_badge/esp32c6/hpcore``
     - ESP32-C6
     - the radio, ``osmosdr_serial`` over SDIO
     - its USB-Serial-JTAG on J8 (see `Building and flashing`_)

Building and flashing
*********************

Flash the C6 first. It reaches J8 only while the P4 hands it the port, as a P4 image built with
``CONFIG_WHY2025_BADGE_USB_TO_C6=y`` does. Then flash the bridge, which takes J8 for the P4:

.. code-block:: console

   west build -b why2025_badge/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west flash --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00
   west build -b why2025_badge/esp32p4/hpcore esp-sdr-zephyr/apps/osmosdr_bridge
   west flash --esp-device /dev/ttyUSB0

The console is on the CH340 (``/dev/ttyUSB0``): the radio's description at boot, then block
counters every 10 s.

The channelizer's PIE kernels (``../common/src/esdr_pfbq_pie.S``) need Espressif's assembler,
``riscv32-esp-elf-as``, which the dev shell of ``default.nix`` has. A build without it (CMake
warns) runs the C reference kernels instead, some 40 times slower on the P4 and far too slow for
the stream.

`Applications <../../doc/apps/index.md>`__ has the steps all apps share.

Usage
*****

Triggered snapshots
===================

The C6's triggered snapshots (``ESDR_MODE_TRIGGER``, see
`osmosdr_serial <../osmosdr_serial/README.rst>`__) pass through as they are:
``esdr_usb trig`` writes 80 MS/s snapshots around each burst.
`Burst capture <../../doc/samples/bursts.md>`__ explains the trigger.

Channelizer
===========

With the C6 in gapless mode (see `osmosdr_serial <../osmosdr_serial/README.rst>`__), the bridge
runs a channelizer over the segment blocks: a polyphase filterbank, 2x oversampled, and a burst
detector per channel (``../common/include/esdr_pfbq.h``).

``ESDR_REQ_SET_CHANNELIZER`` sets it:

- 16, 32, 40, 64 or 80 channels over the 80 MHz;
- an opening level;
- a channel mask.

The open channels' samples go out as ``ESDR_BLK_CHAN`` blocks, int16 at 32 times the raw units,
each run with its index on the channel's absolute grid. ``esdr_usb chan -C 40 -O 6`` records
them and counts bursts per channel.

`Channelizer <../../doc/modem/channelizer.md>`__ describes the fixed-point filterbank, its
accuracy and its cost on the P4.

Measurements
************

.. list-table::
   :header-rows: 1

   * - Test
     - Result
   * - ``esdr_usb rx`` at 2 MS/s, 5 s runs
     - 4.0 MB/s gapless; a tone 60 dB over the floor, its image some 55 dB down
   * - ``esdr_usb trig``, the Communicator sending 5.69 bursts a second
     - 5.6 snapshots a second, none broken

See also
********

- `osmosdr_serial <../osmosdr_serial/README.rst>`__: the C6's app and its receive modes.
- `osmosdr <../osmosdr/README.rst>`__: the protocol, the host tool and the Soapy module.
- `Channelizer <../../doc/modem/channelizer.md>`__: the filterbank behind ``ESDR_BLK_CHAN``.
- `Host transports <../../doc/samples/transports.md>`__: the bridge among the other links.
