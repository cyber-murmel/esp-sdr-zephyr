.. _esp_sdr_chancap:

chancap
#######

Overview
********

Channelized burst capture on the XIAO ESP32S3: the receiver watches a grid of
equally spaced channels (BLE at 2 MHz, IEEE 802.15.4 at 5 MHz, any uniform grid
inside 16 MHz) and sends one I/Q capture per packet per channel to the host as
VITA 49.2 over a vendor bulk endpoint. The design is in ``doc/chancap.md``; the
library module is ``esp_sdr_chancap.h``.

- **Receive**: the ring runs at 16 MS/s without decimation on CPU 1 and hands
  the stream to CPU 0 through SRAM bank 2. There a wideband power gate decides
  when the channelizer runs, a polyphase filter bank (8 channels on the vector
  unit) or a decimator per channel (other grids) splits the band, and a power
  detector per channel cuts the packets out, with a lookback before the trigger
  and a hang time after the power drops. Captures live in a PSRAM slab.
- **Output**: one VITA 49.2 stream per channel (stream ID base + channel) in
  4 KiB blocks on bulk IN: a context packet before every capture (centre
  frequency, rate, the channel's noise floor as the reference level in dBFS,
  the burst's peak above it as gain stage 1), then 16-bit signal data packets
  whose trailer marks the capture's first packet (sample loss), its last (user
  bit 8), a cut at the length limit (bit 9) and a chunk of a continuous stream
  (bit 10). Timestamps are the ring's pair index as seconds and picoseconds.
- **Modes**: both gates can be switched at runtime (``chancap set wbgate``,
  ``chgate``): per-packet captures, every channel in full while the band is
  busy, or every channel as a continuous stream in chunks.
- USB: the capture function, CDC-ACM shell, DFU (MCUboot).

Building and flashing
*********************

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/chancap
   west flash

Later updates: ``west dfu -s <usb serial>`` (see ``apps/osmosdr/README.rst``).
The app uses the same USB IDs as the other apps (2fe3:0005, host udev rules as
in ``apps/stream/README.rst``); the host tool finds it by its product string.

Using it
********

The grid and the detector come from Kconfig (``CONFIG_APP_FIRST_FREQ_HZ``,
``CONFIG_APP_SPACING_HZ``, ``CONFIG_APP_CHANNELS``, ``CONFIG_APP_CHANNEL_RATE_HZ``
and the thresholds; BLE's 8 channels from 2402 MHz at 2 MS/s by default) and
start at boot. The shell on the CDC-ACM port changes them:

.. code-block:: console

   chancap set first 2405000000 spacing 5000000 channels 3 rate 4000000
   chancap stop
   chancap start
   chancap status

``chancap set`` without arguments lists the keys; ``wb``, ``ch``, ``wbgate``
and ``chgate`` also apply to the running capture. ``chancap bench`` times the
channelizer on synthetic data and checks the vector kernels against the C
reference. ``chancap status`` prints the plan (engine, LO, taps), the pairs
handed over and channelized, captures, drops, hand-over overruns and the
channelizer's cycles per input pair.

On the host, ``tools/chancap_bridge.py`` reads the stream and turns every
capture into a GNU Radio PDU: ``--print`` lists them, ``--save DIR`` writes
them as ``.npz`` (``iq`` as complex float, full scale 1.0, and the metadata
as JSON), ``--zmq tcp://*:5555`` publishes the serialized PDUs for a
gr-zeromq "ZMQ SUB Message Source". The PDU metadata keys are ``rx_time``,
``rx_freq``, ``rx_rate``, ``channel``, ``start_index``, ``power_db``,
``noise_db``, ``truncated`` and ``chunk``. It reads the bulk data as one
stream: a read need not end where the board's 4 KiB block did, and a word
that cannot start a packet is skipped and counted.

``tools/ble_pcap.py`` turns the captures of the BLE grid into pcap for
Wireshark (``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR``): it demodulates LE 1M,
finds the advertising access address (or ``--aa``, with ``--crc-init``),
dewhitens with the channel's index and checks the CRC, using
``apps/ubertooth/tools/le_ref.py`` for the link layer bits. The radio header
carries the RF channel, the access address's bit errors and the CRC result;
the power fields stay unset (the board knows dBFS, not dBm).

.. code-block:: console

   ble_pcap.py -w ble.pcap --seconds 60 --print   # a file, a line per packet
   ble_pcap.py -w - --seconds 0 | wireshark -k -i -   # live
   ble_pcap.py --npz DIR -w ble.pcap              # chancap_bridge.py --save's files

On the bench (the C5 advertising through the RF switch, 2026-10-18), Wireshark
shows its ``ADV_SCAN_IND`` with the CRC valid, along with the advertisers
nearby; about a third of the C5's advertisements on channel 37 come through,
the rest are lost to channelizer overruns (see Limits).

Limits
******

- The grid must fit the ring's 16 MHz; a spacing that divides 16 MHz runs the
  filter bank at the spacing or twice it, other spacings run a decimator per
  channel at 16 MHz divided by an integer, and their offsets from the LO must
  be multiples of 125 kHz.
- The channelizer shares CPU 0 with USB and the shell. When the band is busy
  for longer than it keeps up with, the hand-over ring overruns, the capture
  restarts from the newest data and ``status`` counts it.
- The LO is placed between two channels (filter bank) or 1.5 MHz off the
  centre of the grid (decimator bank), so its leakage falls on a channel edge.
- USB full speed carries about 1 MB/s: about 250 kS/s of 16-bit captures in
  all, so the per-channel gate must stay on for a busy band.
