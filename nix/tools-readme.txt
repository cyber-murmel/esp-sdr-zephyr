ESP-SDR host tools @VERSION@

The host tools of esp-sdr-zephyr (https://github.com/cyber-murmel/esp-sdr-zephyr, documentation
at https://software.defined.rodeo) for a Linux PC without Nix: x86_64 and glibc 2.38 or newer
(Ubuntu 24.04, Debian 13, Fedora 39). GPL-3.0-or-later, see LICENSE.

bin/: C programs, ready to run. libusb is linked in, nothing else is needed.

  esdr_usb    the osmosdr app's USB link: info, throughput tests, receive to a file, transmit
  vrt_udp     receive the ESP32-S31's VITA 49.2 stream (apps/stream_eth) over UDP as raw cs8
  vrt_fake    a stand-in for that board on the loopback, to try vrt_udp without one
  vrt_tx      send a sample stream to the ESP32-S31's transmit stream (apps/txstream_eth)
  tx_fake     a stand-in for that board on the loopback, to try vrt_tx without one
  udp_tx      paced UDP packets for apps/rxbench
  qam_sim     host check of the link PHY (apps/link)
  ofdm_sim    host check of the OFDM link (apps/link)

esdr_usb, vrt_udp, vrt_tx and udp_tx print their usage with -h. The options of all of them are
in the header of their source (apps/<app>/tools/*.c in the repository). qam_sim and ofdm_sim run
their simulation without arguments.

apps/<app>/tools/: the Python tools, as in the repository, where some import their siblings
and some another app's tools. Run them with python3, for example
python3 apps/osmosdr/tools/esdr_scope.py. Their Python packages (pip, or your distribution's):

  numpy and pyserial   most of them: serial links, captures and checks
  pyusb                chancap_bridge.py, esdr_serial.py, esdr_pcap.py (and libusb 1.0)
  pyzmq                chancap_bridge.py
  matplotlib           gapless.py
  Pillow               esp_sdr_lab.py
  PyYAML               mkegg.py
  GNU Radio 3.10 with gr-osmosdr and PyQt5
                       esdr_scope.py, esdr_gr_pcap.py, esp_sdr_lab.py (apt install gnuradio
                       gr-osmosdr; they need the SoapySDR module, see the release notes)

A few development tools (prof.py, le_ref.py, gen_tone_table.py, esdr_gr_pcap.py) read files of the
repository around them and work only in a checkout.

Left out, in the source tree (apps/<app>/tools/ of the repository): the Python scripts under
apps/micropython/tools/bench/, the launcher osmosdr/tools/esdr_scope.sh, pcap/tools/esdr_burst.lua
(a Wireshark dissector), the loopback_test.sh scripts of stream_eth and txstream_eth, the C
sources and the txstream_eth Makefile (which also builds the unit test test_tx).
