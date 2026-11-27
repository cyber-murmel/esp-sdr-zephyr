Every app for every board it runs on, built with Nix. Flash them from Chrome or Edge with the
[web flasher](https://software.defined.rodeo/flasher/), or with esptool:

- `esp-sdr-<app>-@TAG@-<board>.bin`: one app's whole flash image (bootloader or MCUboot, the
  app, multiapp's FAT volume), written at 0x0:
  `esptool --chip <chip> write-flash 0x0 esp-sdr-<app>-@TAG@-<board>.bin`.
  The MCH2022 badge's RP2040 gets a `.uf2` instead.
- `esp-sdr-@TAG@-<board>.zip`: the board's images one by one, with their ELF files, the signed
  images for USB DFU (`west dfu`) and `flash_args`.
- `firmware.json`: the list of images, as the web flasher reads it.
- `esp-sdr-tools-@TAG@-x86_64-linux.tar.gz`: the host tools for a Linux PC without Nix (glibc 2.38
  or newer, as Ubuntu 24.04): the C programs `esdr_usb`, `vrt_udp`, `vrt_tx`, `udp_tx` and the
  simulators, with libusb linked in, and the Python tools as files in their `apps/<app>/tools`
  layout. Its `README.txt` lists what each needs.
- `esp-sdr-gateware-@TAG@-mch2022-top.bin` and `-link.bin`: the FPGA bitstreams of the MCH2022
  badge (raw iCE40 bitstreams, not flash images), the DSP chain (micropython, fpga_demo) and the
  link (osmosdr_serial). The images of these apps carry them already; the files are for loading
  them by other means.

The list of every image is at the end. The rest of these notes is for the Seeed XIAO ESP32S3
(8 MB flash) and an x86_64 Linux PC:

- `esp-sdr-multiapp-@TAG@-xiao_esp32s3.bin`: multiapp, with MCUboot and every app on its FAT
  volume.
- `@PLUGIN@`: the SoapySDR module for the board's osmosdr app (`driver=esdr`), built on
  Ubuntu 24.04 for SoapySDR ABI @ABI@ and libusb 1.0. Elsewhere, build it from
  `apps/osmosdr/soapy`.
- `esp32s3-esp-sdr-@TAG@.svd`, `esp32c6-esp-sdr-@TAG@.svd`: Espressif's SVDs with the radio
  registers this project documents
  ([ESP32-S3 registers](https://software.defined.rodeo/dev/registers-esp32s3.html),
  [ESP32-C6 registers](https://software.defined.rodeo/dev/registers-esp32c6.html)),
  for debuggers.

## Host setup

- On Ubuntu, `apt install gqrx-sdr soapysdr-tools` brings gqrx and SoapySDR.
- Without root, the PC needs udev rules for the board (USB vendor 2fe3):
  [USB IDs and host access](https://software.defined.rodeo/apps/index.html#usb-ids-and-host-access)
  has them.

## gqrx

1. Flash with the web flasher, or with esptool and the board in download mode (hold BOOT while
   plugging it in, or run `download` on the shell of a running esp-sdr app,
   `esdr.shell("download")` in its MicroPython REPL):
   ```
   esptool --chip esp32s3 write-flash 0x0 esp-sdr-multiapp-@TAG@-xiao_esp32s3.bin
   ```
2. Load the plugin: SoapySDR loads the modules in the directories `SOAPY_SDR_PLUGIN_PATH` names.
   ```
   mkdir -p ~/.local/lib/esdr && cp @PLUGIN@ ~/.local/lib/esdr/
   export SOAPY_SDR_PLUGIN_PATH=~/.local/lib/esdr
   ```
3. Start osmosdr on the board. multiapp boots into MicroPython's launcher (again after every
   power cycle), whose REPL runs the shell's `multiapp run osmosdr`: Ctrl-C stops the launcher,
   the board reboots into osmosdr, and `SoapySDRUtil --find=driver=esdr` then lists it:
   ```
   printf '\x03\rimport esdr; esdr.shell("multiapp run osmosdr")\r' > /dev/serial/by-id/usb-Zephyr_Project_ESP-SDR*-if00
   ```
4. Run `gqrx` from the same shell. In its I/O device configuration: device string
   `driver=esdr,soapy=1`, input rate 250000 (the most USB full speed carries).

## Other apps

The other apps: `multiapp list` and `multiapp run <name>` on the board's USB shell
([multiapp](https://software.defined.rodeo/apps/multiapp.html)).

## ESP32-S31 Function CoreBoard

Flash through the UART bridge's USB-C port (a CP2102N, which esptool and the web flasher reset
into download mode over RTS and DTR; hold BOOT while pressing RST when nothing answers):
`esptool --chip esp32s31 write-flash 0x0 esp-sdr-<app>-@TAG@-esp32s31_function_coreboard.bin`.
Its apps use the USB-A socket J4 (high speed) or the RJ45 for data.

- `osmosdr_hs` (the recommended image): the board as an osmosdr SDR over USB high speed on the
  USB-A socket J4, receive only, 8 bit I and Q up to 20 MS/s. The SoapySDR module above and
  `esdr_usb` from the tools tarball work with it unchanged: after flashing it starts as an osmosdr
  device (there is no launcher), `SoapySDRUtil --find=driver=esdr` lists it, and in gqrx the device
  string is the one above (`driver=esdr,soapy=1`) with the input rate `20000000` in place of
  250000. J4's VBUS is always driven: use a cable with VBUS cut (a plain A-to-A cable ties the
  board's 5 V to the PC's) and power the board from a USB-C port
  ([osmosdr_hs](https://software.defined.rodeo/apps/osmosdr_hs.html)).
- `stream_eth`: the gapless receive, 8 bit I and Q up to 40 MS/s, as VITA 49.2 signal data
  over UDP on the gigabit Ethernet (`stream_eth-onecore` and `stream_eth-product` are the build
  variants). The board takes an address by DHCP or IPv6 autoconfiguration and logs it on its
  UART; a host subscribes with a short handshake, there is no authentication, so use it on a LAN
  only ([stream_eth](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/main/apps/stream_eth/README.rst)). Receive it with
  `vrt_udp <address> -f <Hz> -r <S/s> -o file.cs8` from the tools tarball, or with the SoapySDR
  module above: `SoapySDRUtil --probe="driver=esdr,vrt=<address>"`, and in gqrx the device
  string `soapy=0,driver=esdr,vrt=fe80::1234%eth0` (the board's address, link-local with the
  PC's interface). From 20 MS/s on raise `net.core.rmem_max`, or the kernel drops datagrams.
- `txstream_eth`: the transmit side, 20 or 40 MS/s from the PC to the DAC with
  `vrt_tx --to <address> --rate 20 --fmt iq8 --source file:<file>` (tools tarball). The radio stays
  off unless the run asks for `--rf` and `tx arm` on the board's shell allows it
  ([txstream_eth](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/main/apps/txstream_eth/README.rst));
  `txstream_eth-injector` plays a tone made on the board.
- `rxbench` (UDP to port 4993, `udp_tx` from the tools tarball) and `ptp_chain` (the IEEE 1588
  clock offset of two boards on one cable) are benches.
- `multiapp` is **bench only**: its MCUboot is built with MCUboot's public development key, not
  the project's own signing key, which stays with the maintainer. Anybody can sign an image that
  this bootloader boots. The web flasher marks it and never preselects it; the apps it carries
  are also listed as images of their own. For a board that others can reach, build
  `apps/multiapp` with your own key (`ESDR_SIGNING_KEY`, see
  [multiapp](https://software.defined.rodeo/apps/multiapp.html)).
