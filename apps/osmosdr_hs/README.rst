.. _esp_sdr_osmosdr_hs:

osmosdr over USB high speed (ESP32-S31)
#######################################

Overview
********

An ESP32-S31-Function-CoreBoard-1 or a VIEWE PLCM-UES31S043H800V480C-U as an
:ref:`osmosdr <esp_sdr_osmosdr>` USB device on its USB 2.0 high-speed port, the radio on the same chip. The host sees the same vendor interface as the S3's
osmosdr app (VID 0x2fe3, PID 0x0005; the Soapy module and ``../osmosdr/tools/esdr_usb`` work
unchanged), with 512-byte bulk packets. Receive only, 8-bit items, at 20, 16, 10, 8, 5, 4, 3.2, 2.5
or 2 MS/s: 160 MHz over an even divider. A host that asks ``ESDR_REQ_GET_RATES`` (``esdr_proto.h``;
the SoapySDR module and ``esdr_usb`` do) gets them all; an older one knows 20, 10 and 5.

The samples come from the gapless ring through the modem's diagnostic bus
(``esp_sdr_diag16.h``). Its nodes are blocks of the wire format with the header left out: the app
writes the header into the hole in front of the samples and copies eight blocks at a time into the
memory USB reads. The ring never waits, so a host that falls behind gets whole blocks or none, with
``ESDR_BLK_OVERFLOW`` on the next one and the block index jumping over what went. There is no digital
filter: a lower rate takes every n-th sample of the bus, and the analog low-pass goes no lower
than some 13 MHz, so the rates below 20 MS/s alias.

Measured on one board against a laptop's xHCI port:

- 20 MS/s for 300 s through ``esdr_usb`` (40.157 MB/s on the wire, block headers included): no gap,
  no overflow, no bad block. 35 s through SoapySDR's rate test: no gap.
- Every rate of the list for 5 s through ``esdr_usb``: 100 % of the rate, no gap, no overflow; 16,
  3.2 MS/s and 7 MS/s (taken as 8) through SoapySDR's rate test for 8 s: no gap, nothing dropped.
- With USB running the stream leaves 81 % of the CPU free at 20 MS/s, 87 % at 10, 96 % at 5
  (``loadlog.conf``); without USB, 86 % at 20 and 72 % at 40 (``selftest.conf``).
- A host process stopped for 40 ms loses nothing: what the host has queued (32 transfers of 64 KiB,
  in ``esdr_usb`` and the Soapy module) and what the device holds cover it. Longer stalls lose their
  excess in whole batches, flagged, the host and the device agreeing on the count.

At a fixed gain each start runs the DC servo on the ring, which takes some 500 ms. So does every
change of frequency, gain, rate or bandwidth: the stream stops and comes back, and the block index
counts the samples the restart lost, so it stays the time axis. A request repeating the value in
force changes nothing and restarts nothing. The Soapy module fills a gap with zeros up to 1,000,000
samples (50 ms at 20 MS/s) and reports a longer one with ``SOAPY_SDR_END_BURST`` and in its log;
``esdr_usb`` fills up to 100,000,000 and counts the rest. Automatic gain has no servo, the gain
moves: its DC stays where the PHY's tables leave it, some 2 steps on a quiet band at an rms of 2.5.
A frequency outside the PLL's reach, 2200 to some 2900 MHz, gives no stream:
``GET_STATE`` says centre 0 and the stats keep the error.

Streaming needs a high-speed link: at full speed ``ESDR_MODE_RX`` is refused with a stall.

A suspend stops the stream and the pads: the port's VBUS is the board's own, so a suspend is all a
pulled cable leaves. A host that resumes with no bus reset gets its stream back at index 0 with
``ESDR_BLK_GAP``; a bus reset ends the stream. Read from the code, not yet run with a real suspend.

Connecting it
*************

**The HS port is the board's USB-A socket J4, and its VBUS is always driven** (a TPS2051C with its
enable pulled high, no GPIO). A plain A-to-A cable ties the board's 5 V to the PC's. Use one with
VBUS cut, or tape over pin 1 of the plug, from J4 to a USB-A port of the PC, and power the board
from one of its USB-C ports. An A-to-C cable into a PC's USB-C port cannot work: the cable makes the
PC the sink. An A-to-C cable with VBUS cut and a C-to-A adapter on its C end does work.

**The cable decides whether this is high speed at all.** Cut only VBUS and leave the data pair
twisted and whole. A first cable, with its data pair not kept that way, passed the chirp and worked
at 12 Mbit/s, but at 480 Mbit/s the host saw ``device descriptor read/64, error -71``: the chip's
receiver decoded not one start-of-frame packet at its boot squelch setting, and lost most packets at
any lower one. With a proper cable the same image enumerates first time. A USB stick plugged straight
into J4, with the controller in host mode, showed the chip's own path clean: 620 of 620 descriptor
reads over 13 of the 16 squelch settings, the boot value among them.

The USB-C ports stay usable: the UART bridge has esptool and the log, USB-Serial-JTAG stays up for
OpenOCD.

**GPIO 20 to 25, 36 to 40, 42 to 44, 47 and 48 are driven at up to 40 MHz: nothing may be connected
to them on J2.**

The VIEWE ESP32-S31 board
=========================

``ues31s043h800v480c_u/esp32s31/hpcore`` runs this too, and connects the same way: its USB-A socket is
the OTG port, its VBUS is always driven, power and console are on the USB-C socket. There the ring's 16
pads are the panel's data lines, GPIO 2 to 17, so this image leaves the display dark. Measured with the
same VBUS-cut cable: 20 MS/s for 30 s, 40.157 MB/s, no gap, overflow or bad block.

Building and flashing
*********************

.. code-block:: console

   west blobs fetch hal_espressif
   west blobs fetch esp-sdr-zephyr
   west build -b esp32s31_function_coreboard/esp32s31/hpcore esp-sdr-zephyr/apps/osmosdr_hs
   west flash --esp-device /dev/serial/by-id/usb-Silicon_Labs_CP2102N_*

The VIEWE board builds as ``ues31s043h800v480c_u/esp32s31/hpcore`` and flashes through its CH340,
``/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0``. Its ring takes the panel's data lines: the panel stays dark.

What the stream leaves of the CPU, without USB and in the log with it:

.. code-block:: console

   west build -b esp32s31_function_coreboard/esp32s31/hpcore esp-sdr-zephyr/apps/osmosdr_hs \
     -- -DEXTRA_CONF_FILE=selftest.conf
   west build -b esp32s31_function_coreboard/esp32s31/hpcore esp-sdr-zephyr/apps/osmosdr_hs \
     -- -DEXTRA_CONF_FILE=loadlog.conf
