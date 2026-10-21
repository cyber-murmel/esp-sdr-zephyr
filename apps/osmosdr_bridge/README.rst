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

Channelizer
***********

With the C6 in gapless mode (``../osmosdr_serial/README.rst``) and a channelizer set
(``ESDR_REQ_SET_CHANNELIZER``: 16, 32, 40, 64 or 80 channels over the 80 MHz, an opening level,
a channel mask), the segment blocks go through a polyphase filterbank, 2x oversampled, and a
burst detector per channel (``../common/include/esdr_pfbq.h``); the open channels' samples go out
as ``ESDR_BLK_CHAN`` blocks, int16 at 32 times the raw units, each run with its index on the
channel's absolute grid. ``esdr_usb chan -C 40 -O 6`` records them and counts bursts per channel.

The filterbank runs in fixed point four frames at a time on the P4's PIE SIMD
(``../common/src/esdr_pfbq_pie.S``), assembled by Espressif's assembler: the dev shell of
``default.nix`` has it (``riscv32-esp-elf-as``). A build without it (CMake warns) runs the same
arithmetic in the C reference kernels, some 40 times slower on the P4 and far too slow for the
stream. Against the float bank (``esdr_pfb.h``) the error stays within
0.4 sqrt(M) output units rms (``tests/unit/esdr_pfbq``, which also checks PIE against C bit for
bit on the badge). At 360 MHz the bridge takes 77 to 83 cycles a pair with 40 channels on busy
2.4 GHz air (filterbank 39, detector 15, channel blocks 10 to 15): the C6's whole link, 3.9 M
pairs/s kept at a fixed low threshold, went through with 12 to 27 % of the CPU to spare and
nothing dropped. The float bank had managed some 1 M pairs/s.

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
