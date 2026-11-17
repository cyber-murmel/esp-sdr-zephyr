Every app for every board it runs on, built with Nix. Flash them from Chrome or Edge with the
[web flasher](https://software.defined.rodeo/flasher/), or with esptool:

- `esp-sdr-<app>-@TAG@-<board>.bin`: one app's whole flash image (bootloader or MCUboot, the
  app, multiapp's FAT volume), written at 0x0:
  `esptool --chip <chip> write-flash 0x0 esp-sdr-<app>-@TAG@-<board>.bin`.
  The MCH2022 badge's RP2040 gets a `.uf2` instead.
- `esp-sdr-@TAG@-<board>.zip`: the board's images one by one, with their ELF files, the signed
  images for USB DFU (`west dfu`) and `flash_args`.
- `firmware.json`: the list of images, as the web flasher reads it.

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
   plugging it in, or run `usj` on the shell of a running esp-sdr app, `esdr.shell("usj")` in
   its MicroPython REPL):
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
