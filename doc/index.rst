.. SPDX-License-Identifier: GPL-3.0-or-later

##############
esp-sdr-zephyr
##############

A Zephyr module that turns the Wi-Fi radio of five ESP32 chips into a software defined radio (raw
I/Q receive, and transmit on all but the classic ESP32), an IEEE 802.15.4 software radio built on
it, a Zigbee coordinator, and the apps that use them.

About these docs
****************

Structure
=========

The docs open with an overview, `ESP-SDR on Zephyr <../README.rst>`__: safety, getting started and
the library at a glance. Five parts follow, each building on the one before:

#. `The radio <radio/index.md>`__ follows the signal through the analog radio: the receive and
   transmit paths, tuning, gain, filters, impairments and crystal calibration.
#. `Samples in and out <samples/index.md>`__ starts where the samples leave the ADC and enter the
   DAC: burst capture, gapless receive, transmit, paced streams and host transports.
#. `Modulation and coding <modem/index.md>`__ turns those samples into packets and back: the
   channelizer, IEEE 802.15.4, GFSK and Bluetooth LE, burst detection, QAM and OFDM, error
   correction.
#. `Applications <apps/index.md>`__ puts the first three parts to work: the apps, their internals
   and the MCH2022 badge.
#. `Reference <dev/index.md>`__ holds what the parts look up: chip notes, code placement, testing,
   the ZigBit reference and the register pages.

A guide page opens with what it covers and where it sits, then gives the concepts, the
implementation, the differences between chips, the measurements and the limits. The API reference
(below) documents the headers.

Goal
====

The docs explain how the Wi-Fi radio of these chips works as a software defined radio, and how the
library, the modems and the apps are built on it. Bench measurements back the claims, with their
date and setup.

Audience
========

- Developers who build on ``esp_sdr`` or extend the apps: Parts 1 to 3, then the reference.
- Users of the apps: the overview and Part 4.

The pages assume C, Zephyr (west, Kconfig, devicetree) and basic signal processing.

Scope
=====

The docs cover the ``esp_sdr`` library, the IEEE 802.15.4 software radio, the Zigbee coordinator
and the apps. Which chip has which radio is in `The radio <radio/index.md>`__ (Chip lineup), which
chip has which sample path in `Samples in and out <samples/index.md>`__ (Sample paths per chip).

They leave out:

- Productive or certified use: see Safety in `ESP-SDR on Zephyr <../README.rst>`__.
- The Zigbee stack's API, which its headers in ``include/zigbee/`` document.

API reference
*************

The API reference of the library is generated with Doxygen from ``include/``:
`API reference`__. The library's groups follow the order of the parts; the IEEE 802.15.4 groups
come after them.

__ doxygen/index.html

Contents
********

.. toctree::
   :maxdepth: 1
   :caption: Overview

   overview

.. toctree::
   :maxdepth: 1
   :caption: Part 1: The radio

   radio/index
   radio/transceiver
   radio/tuning
   radio/gain
   radio/filters
   radio/impairments
   radio/crystal-calibration

.. toctree::
   :maxdepth: 1
   :caption: Part 2: Samples in and out

   samples/index
   samples/bursts
   samples/rings
   samples/transmit
   samples/paced
   samples/transports

.. toctree::
   :maxdepth: 1
   :caption: Part 3: Modulation and coding

   modem/index
   modem/channelizer
   modem/ieee802154
   modem/gfsk
   modem/bursts
   modem/qam-ofdm
   modem/fec

.. toctree::
   :maxdepth: 1
   :caption: Part 4: Applications

   apps/index
   apps/capture
   apps/rfprobe
   apps/stream
   apps/osmosdr
   apps/osmosdr_serial
   apps/osmosdr_bridge
   apps/osmosdr_hs
   apps/chancap
   apps/link
   apps/wpan
   apps/zstack
   apps/ubertooth
   apps/ubertooth-internals
   apps/pcap
   apps/pcap-internals
   apps/multiapp
   apps/multiapp-internals
   apps/micropython
   apps/mch2022_coproc
   apps/mch2022

.. toctree::
   :maxdepth: 1
   :caption: Part 5: Reference

   dev/index
   dev/chips
   dev/code-placement
   dev/testing
   dev/zigbit
   dev/registers-esp32s3
   dev/registers-esp32c6

Indices
*******

* :ref:`genindex`
* :ref:`search`
