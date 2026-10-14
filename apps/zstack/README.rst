.. _esp_sdr_zstack:

zstack
######

Zigbee coordinator with the Z-Stack ZNP serial protocol

Overview
********

A Zigbee USB stick for the XIAO ESP32-C6: it speaks the MT (monitor and test) serial protocol
of TI's Z-Stack 3.x network processor, so hosts written for a CC2652 stick (zigbee2mqtt with
``adapter: zstack``, zigpy-znp) can drive it. The firmware is our own: no TI code, the Zigbee
layers are written for this repository (``lib/zigbee``, ``include/zigbee``).

Progress (``Z-Stack`` below always means the protocol, not TI's firmware):

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - Phase
     - State
   * - 1
     - Done: MT framing with resync, SREQ dispatch, ``SYS``, ``UTIL`` and the NV item store in the
       ``storage`` flash partition.
   * - 2
     - Done on the bench, one check open: the native radio backend, energy and active scans,
       beacon responses, association with short address allocation and indirect delivery,
       formation, restore and the NIB in Z-Stack 3.30's aligned layout, the ``APP_CNF``, ``ZDO``,
       ``AF`` and ``DEBUG`` commands. zigpy-znp's complete application startup and
       zigbee-herdsman (Zigbee2MQTT) start, restore after a reboot, scan real networks and
       register their endpoints. Open: a real device associating.
   * - 3
     - NWK security, routing, APS and the trust center (the transport key, install codes), data
       through ``AF``: not started. Until then a device that associates stays a MAC child: it
       gets its short address and no network key, and application data requests are confirmed
       with "no route".
   * - 4, 5
     - Sleepy end devices (frame pending in the ACK); the ESP32-S3 software radio: not started.

The host link is USB-Serial-JTAG (``/dev/ttyACM*``). The console and logs are on UART0 (D6 TX,
D7 RX). A soft reset (``SYS_RESET_REQ``) restarts the ZNP state and sends ``SYS_RESET_IND``
without rebooting the chip, because a reboot would re-enumerate USB under the host's open port.

The IEEE address is the factory MAC with ``FFFE`` in the middle. NV items persist: the changes
are written to flash about a second after the last one, and at once on ``SYS_RESET_REQ``.

Hosts that toggle DTR and RTS on connect reset the C6 (USB-Serial-JTAG). zigpy-znp: set
``skip_bootloader: false``. zigbee-herdsman has no such option: it pulses the lines when its first
pings go unanswered and Linux drops them when the port closes, so the stick may restart once
when Zigbee2MQTT starts or stops. The network comes back from flash by itself.

Debugging: ``DEBUG_SET_THRESHOLD`` (MT subsystem 8, command 0) with threshold 1 streams stack events
and with 2 every frame the MAC sends or receives to the host as ``DEBUG_MSG`` AREQs (ASCII).

The antenna: the chip antenna by default; add ``-DCONFIG_XIAO_ESP32C6_EXT_ANTENNA=y`` to build for
the U.FL port.

Building and running
********************

ESP32-C6 over USB-Serial-JTAG:

.. code-block:: console

   west build -b xiao_esp32c6/esp32c6/hpcore esp-sdr-zephyr/apps/zstack
   west flash --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00

Unit tests of the protocol layer run on the host: ``tests/unit/zigbee_znp`` (see
``doc/testing.md``).
