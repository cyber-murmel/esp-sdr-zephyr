.. _esp_sdr_osmosdr_serial:

osmosdr over a serial link
##########################

Overview
********

The :ref:`osmosdr <esp_sdr_osmosdr>` protocol (``../osmosdr/src/esdr_proto.h``) for the
ESP32-C6 and the ESP32-C5, whose only USB port is USB-Serial-JTAG: the vendor
requests travel as frames on the byte stream, receive blocks and transmit samples in between
(the serial transport in ``esdr_proto.h``). The app drives the port's FIFOs itself; the console
is on UART0 (D6 TX, D7 RX on the XIAO). One direction at a time, as on the S3.

- **Receive**: continuous I/Q at 250, 125, 62.5, 31.25 or 15.625 kS/s, 8 or 16 bit. The modem's
  IQ estimator sums the 80 MHz baseband samples in hardware; the CPU reads the running sums at
  the sample rate and sends their differences (``esp_sdr_rx_paced_*()``): each sample is the
  sum of 80 MHz / rate inputs, a boxcar decimator. No offset tuning: the LO leakage stays at
  the centre.
- **Transmit**: the host's samples go to the TX DC value register, which drives the DAC once
  the vendor tone path has turned the transmit chain on (``esp_sdr_tx_paced_*()``), through an
  8-tap-per-phase FIR; 250 kS/s goes to the DAC as it is. The DAC runs at up to 250 kS/s on
  the C6 (125 kS/s x 2) and 500 kS/s on the C5 (125 kS/s x 4). C6 2400 to 2500 MHz, C5 2400 to
  5900 MHz (whole MHz); power steps 0 to 15 (2 dB each).
- The CPU paces both with interrupts locked and serves USB between samples, never so late
  that a sample instant moves.

USB-Serial-JTAG carries about 500 kB/s each way: 250 kS/s receive fits as 8-bit items only,
16 bit up to 125 kS/s. Transmit at 250 kS/s keeps up on the C5; on the C6 about 10 % of the
input arrives late and plays as zeros, so 125 kS/s is its reliable rate.

Other links
===========

- **UART** (chosen ``esdr,link-uart``): the WHY2025 badge's C6 to its P4 at 5 Mbaud, RTS for
  flow control; the same rates as USB-Serial-JTAG. The badge's fallback link, built with
  ``-DEXTRA_CONF_FILE=uart.conf``: the image as before the gapless receiver, which needs SDIO
  (the Wi-Fi driver brings the radio up).
- **SDIO** (``CONFIG_APP_LINK_SDIO``, the WHY2025 badge's default): the C6's SDIO slave,
  function 1, 4 bit, as esp-hosted uses it (``src/link_sdio.c``, on the HAL's LL layer). The
  host's writes land in 16 buffers of 512 bytes counted by tokens; the receive blocks go out
  by DMA straight from their buffers (11.6 MB/s measured raw), replies zero padded to whole
  words, which the host skips between frames. A host that restarts writes bit 0 of its
  register 0x08C: the radio drops the send queue and starts its counters again.

  Receive then reaches **2 MS/s** (2, 1, 0.5 MS/s, then the rates above), gapless at the
  WHY badge's P4. Above 250 kS/s the items are 8 bit, filled a block at a time by a tight loop
  (``esp_sdr_rx_paced_fill_iq8()``, 59 CPU cycles a sample of the 80 there are at 2 MS/s; the
  digital gain is a power of 2 there). The link's turn between blocks makes the next poll a few
  samples late: the fill gives each sample that poll missed an equal share of its sums, so that
  the samples stay on their grid. (Filled a quarter block at a time and catching up with short
  windows, the stream had a glitch every 255 us at 2 MS/s, a spike and 16 us of skewed samples:
  it cost most Bluetooth LE packets and one 802.15.4 symbol in seventy.) 2.5 MS/s leaves 3 % of
  the samples late, 3.2 MS/s does not fit. Transmit stays at 250 kS/s.

The MCH2022 badge
=================

The badge's classic ESP32 (``mch2022/esp32/procpu``) has no paced receiver: it streams 16 MS/s
bursts decimated by the library (``esp_sdr_rx_capture_decimated()``) at 2 MS/s to 125 kS/s,
each about 1 ms contiguous; the gap to the next burst moves the block index and flags the block
``ESDR_BLK_GAP``, as many bursts as the link carries (about 46 per second: 46 kS/s of samples
on average at 2 MS/s). Bursts at the native rates (``ESDR_REQ_CAPTURE_BURST``) are as on the
others; no transmit. The link is UART0 at 2 Mbaud, which the badge's RP2040 bridges to its first
USB serial port without flow control (3 and 4 Mbaud did not answer); the host sets the line
rate: ``baud=2000000`` in the SoapySDR device string, ``--baud 2000000`` for
``tools/esdr_serial.py``. No console.

The RP2040 resets the ESP32 into its download mode for esptool and keeps IO0 low until a reset
into the application, which the RTS/DTR sequences do not always give; its WebUSB request 0x23
does (``wValue`` 0: application), e.g. with pyusb
``usb.core.find(idVendor=0x16D0, idProduct=0x0F9A).ctrl_transfer(0x21, 0x23, 0, 4, None)``.

Triggered snapshots
===================

On the ESP32-C6 (``CONFIG_ESP_SDR_TRIG``, ``include/esp_sdr/esp_sdr_trig.h``),
``ESDR_MODE_TRIGGER`` keeps the whole 80 MS/s, but only around bursts. The dump engine writes a
circular ring of 16384 samples (205 us) into SRAM block 2 while the CPU differences the IQ
estimator's sums of I^2 + Q^2, I and Q over the same 80 MHz input every 10 us: each window's
power about its mean, so the receiver's DC does not count. Three windows in a row whose power
passes the threshold (``ESDR_REQ_SET_TRIGGER``, a multiple of the quiet baseline, 8 by
default) after a quiet one let the engine run on for the samples asked for after the first of
them, then stop it. Bursts shorter than 20 to 30 us do not trigger: that keeps out a 1.75 us
glitch the Wi-Fi driver puts into the receiver every 139 ms in some of its states. The newest ``ESDR_TRIG_SAMPLES`` (16320) samples go to the host as 8
int8 blocks flagged ``ESDR_BLK_TRIGGER``, the first also ``ESDR_BLK_GAP``, their index counted
from the trigger (negative before it).

The engine never writes the block's upper 64 KiB: the blocks are packed there and sent by DMA
from it, after which the ring runs again. That dead time is about 4 ms (packing and 33 KB
over SDIO at 11.5 MB/s), so a busy band yields some tens of snapshots a second; in the office's
2.4 GHz Wi-Fi the WHY badge's P4 received 52 a second with none broken.

The SDIO link tops out at about 13 MB/s within a transfer whatever the bus clock: 20 to 80 MHz
(80 works, with the P4 sampling on input phase 1 or 2) and SDIO blocks of 512 to 2048 bytes
(larger ones corrupt data) all give the same, which places the limit in the C6's slave. A
continuous 40 MS/s stream does not fit, hence the snapshots.

Gapless receive
===============

The WHY2025 badge's C6 (SDIO) is built for it by default: its board configuration holds what
``gapless.conf`` sets, which another C6 on SDIO takes with ``-DEXTRA_CONF_FILE=gapless.conf``.
The C6 then receives the whole 80 MHz without a gap (``ESDR_MODE_GAPLESS``,
``include/esp_sdr/esp_sdr_gapless.h``, after upstream's ring and h0m3us3r's eSpDR): the dump
engine runs as a ring that never stops while the CPU switches it between SRAM blocks 1 and 2
every 12288 samples (154 us), with marker words in the next block that prove each unit starts
where the last one ended. The ring takes two blocks, so the image must fit block 0: the radio
comes up without the Wi-Fi driver (``CONFIG_ESP_SDR_PHY_ONLY``), which also drops its periodic
receive glitch. The other modes (paced receive and transmit, triggered snapshots, bursts) stay
in the image. The run keeps the PHY in its 20 MHz mode with the widest low-pass (unless the host
set a bandwidth), which passes the whole band: within 7 dB out to +-35 MHz, where the PHY's
40 MHz mode is some 35 dB down and ignores the low-pass.

The IQ estimator's 10 us windows decide what is kept: windows past a threshold over the quiet
baseline, with one before and one after, are copied raw into staging while their block can still be
read (the ring in block 3, then the upper halves of blocks 1 and 2, which the ring leaves free: a
window goes to its own unit's block, which the engine has just left, and is packed while the engine
writes the other one; canary words guard those halves), then packed to int8 with a shift from the
window's power and DC (the DC left in and reported) into ``ESDR_BLK_SEG`` blocks of segments
(``esdr_seg.h``). Every segment keeps its index on the run's axis (80 MS/s, 40 with
``ESDR_GAPLESS_F_HALF``), across the runs a request or a ring error restarts. Two servos keep 5 to
10 % headroom (``ESDR_REQ_SET_GAPLESS``): the threshold rises and falls so that what is kept fills
the link's budget short of it, and with automatic gain the receive gain steps so that the loudest
windows' peaks stay short of full scale. Bursts are kept whole or not at all: a window is admitted
only with room for it in the staging it can reach (block 3 and the smaller half), a burst starts
only with an eighth of that free, and one cut for want of room drops its rest until a quiet window
(or, in a band that never rests, until as many samples as that staging holds have gone by and an
eighth is free again).

With ``ESDR_GAPLESS_F_HALF`` the dump engine runs at 40 MS/s (its rate bit, DUMP_CTRL bit 15): a
unit lasts 307 us, the CPU has twice the time to copy a sample, and every index, count and window of
the run is on the 40 MS/s axis. The engine drops every other sample without a filter, so the run
sets the low-pass to 31 MHz: +-15 MHz stays flat, and what lies beyond 20 MHz out folds in, a few dB
down near 20 MHz and 15 to 25 dB down from 25 MHz on. Width sorting is off at that rate. The CPU
cannot read a block while the engine writes it (stale data), so reading the ring behind the engine
is not possible on the C6.

With ``ESDR_GAPLESS_F_WIDE`` (width sorting, off by default) the C6 judges each kept chunk of a
window's length by how fast it decorrelates (its correlations 50 to 125 ns apart against its
power above the noise, ``lib/esp_sdr/src/esp_sdr_gapless_sort.h``): a signal wider than some
10 MHz (Wi-Fi) goes to the link as a 4-pair record (``ESDR_SEG_WIDE``, ``struct esdr_seg_wide``:
its span, the lag-one correlation whose angle is its mean frequency, the three correlations) in
place of its samples, and the threshold servo counts the bytes saved. A judged point costs
103 cycles, the sorting 5 to 12 % of the CPU on the badge; ``ESDR_REQ_GET_GAPLESS_STATS`` counts
the chunks judged, those sent as records and the cycles by activity.

On the badge the ring ran 120 s (9.6 G samples) without an error. The CPU packs some 1000 samples a
unit while kept windows come in, about the link's 11 MB/s; what it can copy per unit, some 4000
samples at 80 MS/s, keeps a burst whole up to some 50 us (some three times as long at 40 MS/s, where
a unit's cap is twice the samples, each twice as long). ``../osmosdr/tools/esdr_usb gapless``
records the blocks through ``apps/osmosdr_bridge``, ``tools/gapless.py`` checks them (with a carrier
on air, its phase across the gaps proves every segment's index).

Building and flashing
*********************

.. code-block:: console

   west build -b xiao_esp32c6/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west build -b waveshare_esp32c5_zero/esp32c5/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west build -b why2025_badge/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west flash --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00

The WHY2025 badge's C6 reaches its USB port only while the P4 hands it over: ``west dfu --c6``
with ``apps/micropython`` on the P4, or as ``apps/osmosdr_bridge`` describes.

The board configs select the U.FL connector. The C5 has the same IQ estimator (0x24 lower) and
its TX DC value register at 0x600a0c08, where bit 26 enables the injection. Opening the port does not reset the board as long as the host leaves
DTR and RTS alone (the host tool and the SoapySDR module open it raw); tools that toggle them
(esptool, terminal programs) reset it.

Host tool
*********

``tools/esdr_serial.py`` speaks the transport from Python:

.. code-block:: console

   tools/esdr_serial.py <port> info
   tools/esdr_serial.py <port> rx --rate 250000 --seconds 4 --out rx.npy
   tools/esdr_serial.py <port> tx --rate 125000 --seconds 4 --tone 30000

SoapySDR, GNU Radio, gqrx
*************************

The osmosdr app's SoapySDR module (``../osmosdr/soapy``) has a serial backend: name the port in
the device string. It does not probe serial ports by itself (that would write into other
boards' consoles). Over the serial link the native format is CS8, and CF32 travels as 8-bit
items.

.. code-block:: console

   SoapySDRUtil --probe="driver=esdr,port=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00"
   gqrx        # device string: soapy=0,driver=esdr,port=/dev/serial/by-id/...

Behind a USB bridge add the line rate, as for the MCH2022 badge:
``driver=esdr,port=/dev/serial/by-id/usb-Badge.team_MCH2022_badge_<serial>-if00,baud=2000000``.

Statistics
**********

``GET_STATS`` fills the osmosdr fields; the pacing reports in the S3 ring's: ``ring_runs``
counts streams, ``ring_errors`` polls a whole period late, ``ring_late_max`` the worst
lateness in CPU cycles.
