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
  flow control; the same rates as USB-Serial-JTAG.
- **SDIO** (``CONFIG_APP_LINK_SDIO``, the WHY2025 badge's default): the C6's SDIO slave,
  function 1, 4 bit, as esp-hosted uses it (``src/link_sdio.c``, on the HAL's LL layer). The
  host's writes land in 16 buffers of 512 bytes counted by tokens; the receive blocks go out
  by DMA straight from their buffers (11.6 MB/s measured raw), replies zero padded to whole
  words, which the host skips between frames. A host that restarts writes bit 0 of its
  register 0x08C: the radio drops the send queue and starts its counters again.

  Receive then reaches **2 MS/s** (2, 1, 0.5 MS/s, then the rates above), gapless at the
  WHY badge's P4. Above 250 kS/s the items are 8 bit, filled a quarter block at a time by a
  tight loop (``esp_sdr_rx_paced_fill_iq8()``, 59 CPU cycles a sample of the 80 there are at
  2 MS/s; the digital gain is a power of 2 there). 2.5 MS/s leaves 3 % of the samples late,
  3.2 MS/s does not fit. Transmit stays at 250 kS/s.

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

Building and flashing
*********************

.. code-block:: console

   west build -b xiao_esp32c6/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west build -b waveshare_esp32c5_zero/esp32c5/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west flash --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00

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

Statistics
**********

``GET_STATS`` fills the osmosdr fields; the pacing reports in the S3 ring's: ``ring_runs``
counts streams, ``ring_errors`` samples taken a whole period late, ``ring_late_max`` the worst
lateness in CPU cycles.
