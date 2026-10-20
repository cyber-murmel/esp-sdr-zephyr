.. _esp_sdr_osmosdr_bridge:

osmosdr bridge (WHY2025 badge)
##############################

Overview
********

The WHY2025 badge's ESP32-P4 as an :ref:`osmosdr <esp_sdr_osmosdr>` USB device on its USB 2.0
high-speed OTG port (USB-C port J8), the badge's ESP32-C6 running
:ref:`osmosdr_serial <esp_sdr_osmosdr_serial>` over SDIO as the radio. The host sees the same
vendor interface as the S3's osmosdr app (VID 0x2fe3, PID 0x0005; the Soapy module and
``../osmosdr/tools/esdr_usb`` work unchanged), with 512-byte bulk packets:

- vendor requests go to the C6 as request frames and its replies come back;
- the C6's receive blocks are already in the USB block format and go out on bulk IN as they
  are (a pool of 16 blocks, 16 ms at 2 MS/s);
- the host's transmit samples go to the C6 as transmit frames.

Receive reaches 2 MS/s (8-bit items above 250 kS/s), transmit 250 kS/s (the C6's limits). At
2 MS/s, ``esdr_usb rx`` measured 4.0 MB/s gapless over 5 s runs, and a tone 60 dB over the floor
with its image some 55 dB down.

The C6's triggered snapshots (``ESDR_MODE_TRIGGER``, ``../osmosdr_serial/README.rst``) pass
through as they are: ``esdr_usb trig`` writes 80 MS/s snapshots around each burst. With the
Communicator sending 5.69 bursts a second it took 5.6 a second, none broken.

Building and flashing
*********************

The C6 first (it reaches J8 only while the P4 hands it the port, as a P4 image built with
``CONFIG_WHY2025_BADGE_USB_TO_C6=y`` does), then the bridge, which takes J8 for the P4:

.. code-block:: console

   west build -b why2025_badge/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west flash --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00
   west build -b why2025_badge/esp32p4/hpcore esp-sdr-zephyr/apps/osmosdr_bridge
   west flash --esp-device /dev/ttyUSB0

The console is on the CH340 (``/dev/ttyUSB0``): the radio's description at boot, then block
counters every 10 s.
