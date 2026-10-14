.. _esp_sdr_ubertooth:

ubertooth
#########

Overview
********

The device firmware of an `Ubertooth One <https://github.com/greatscottgadgets/ubertooth>`__
on the ESP32-S3 software radio. The unmodified host tools (``ubertooth-util``,
``-specan``, ``-btle``, ``-rx``, ``-scan``, the Wireshark extcap) talk to it
over the Ubertooth USB protocol; the CC2400 radio of the original is replaced
by software demodulation of the ring receive and a GFSK modulator on the DAC.

The USB function is the protocol of ``libubertooth``: vendor interface 0 with
bulk IN 0x82 (and OUT 0x05), the vendor requests 0 to 73 on the control
endpoint, the API version (``0x0107``) as ``bcdDevice``. The CDC-ACM shell and
DFU sit behind it.

Status
******

Written and tested on the host; the lines below say what has also run on a board.

.. list-table::
   :header-rows: 1

   * - Part
     - State
   * - USB function, vendor requests, clock, queue, register file
     - unit tests on ``native_sim`` (``tests/unit/ubertooth_core``); ``ubertooth-util`` runs on a board
   * - Spectrum analyser (``ubertooth-specan``)
     - runs on a board: 79 channels per sweep in ascending order, about 84 sweeps/s; levels not
       calibrated yet
   * - BLE sniffing (``ubertooth-btle``)
     - planned
   * - Transmit (BLE advertising, TX test, generic packet)
     - BLE advertising (``ubertooth-btle -s``) runs on a board and is seen by a PC Bluetooth adapter
       (address, flags and CRC intact); TX test starts and stops; generic packet not tried
   * - BR/EDR symbol stream (``ubertooth-rx``, ``-scan``)
     - planned, fixed channel and the 25 Hz sweep only

What cannot be done
*******************

Retuning the radio takes 3 to 4 ms and needs the ring stopped, so Bluetooth
BR hopping (1600 hops/s) is out of reach: ``START_HOPPING``, ``TX_SYMBOLS``
and the follow modes stall. Jamming (``JAM_ONCE``, ``JAM_CONTINUOUS``) needs a
reaction inside the packet and stalls too; ``SET_ISP``, range test and repeater
have no counterpart here. BLE connection following works for connection
intervals the retune time allows.

USB identity
************

The tools open only the IDs of the Ubertooth (``1d50:6000``, ``1d50:6002``,
``ffff:0004``). ``CONFIG_APP_UBERTOOTH_USB_ID_UBERTOOTH`` (default) presents
``1d50:6002``, the ID of Great Scott Gadgets' Ubertooth One: use it to run the
unmodified tools on a bench, not in a product. The product string says
"ESP-SDR Ubertooth One" and the manufacturer stays "Zephyr Project". DFU mode
keeps the project ID ``2fe3:ffff`` (``CONFIG_APP_USB_DFU_VID``). Choose
``CONFIG_APP_UBERTOOTH_USB_ID_PROJECT`` for ``2fe3:0005``; the tools then need a
patch to open the device.

The vendor interface must be interface 0 and own the endpoints 0x82 and 0x05.
The tree's Zephyr gives a class fixed endpoint addresses with
``USBD_EP_ADDR_FIXED`` (a local patch, ``usbd.h``); without it the interface
carries an unused interrupt endpoint, which gives IN 0x82 and OUT 0x01 (enough
for the C tools, not for ``host/python/ubtbr``).

Building and flashing
*********************

.. code-block:: console

   west build -d build-clanker/xiao_esp32s3/ubertooth --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/ubertooth
   west flash

Later updates go over DFU like the other apps: ``west dfu -s <usb serial>``
(``dfu.py`` knows both IDs). On Linux the tools need access to the USB device:
the udev rule of the ``ubertooth`` package (``40-ubertooth.rules``) covers
``1d50:6002``. The tools are in nixpkgs:

.. code-block:: console

   nix-shell -p ubertooth
   ubertooth-util -v -s -p

Shell
*****

``ubt status``: mode, channel, USB state, queue fill, clock. For bench work (``doc/ubertooth.md``):
``ubt tune <a> <b> [n]`` times retunes, ``ubt rx start <mhz> [gain]`` / ``stop`` / ``stats`` runs the
4 MS/s ring stream, ``ubt rx spec`` and ``ubt rx floor`` show its spectrum and noise floor,
``ubt specanfs <dbm>`` sets the spectrum analyser's level calibration.

Source of the Ubertooth parts
*****************************

``src/upstream/`` holds files of the Ubertooth project (GPL-2.0-or-later, headers
kept): ``ubertooth_interface.h`` (the protocol), ``types.h`` and ``bluetooth_le.[ch]``
(CRC-24, whitening, channel helpers). The rest is new, under GPL-3.0-or-later.
The USB product, the protocol and the behaviour of the requests follow the
upstream firmware (``firmware/bluetooth_rxtx/bluetooth_rxtx.c``); deviations are
marked in the sources.
