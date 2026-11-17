# Applications

<!-- nav-start -->
Previous: [Error correction](../modem/fec.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Web flasher](flasher.md)
<!-- nav-end -->

Part 4 puts the earlier parts to work: the radio, its sample paths and the modems of
[Modulation and coding](../modem/index.md) become apps you build, flash and drive from a host.
Each app is a Zephyr application in `apps/<name>`; several also run inside
[multiapp](../../apps/multiapp/README.rst) on the ESP32-S3 and the ESP32-S31.

Every app page follows one layout: Overview, Hardware, Building and flashing, Usage, Output
format, Measurements, Limitations and See also, each where the app has content for it.

Three apps have a page with their design and measurement log, right after the app:
[multiapp internals](multiapp-internals.md), [ubertooth internals](ubertooth-internals.md) and
[pcap internals](pcap-internals.md). [MCH2022 badge](mch2022.md) follows mch2022_coproc and covers
the badge's RP2040 firmware and its FPGA accelerator.

This page lists the apps and holds what they share: building, DFU updates, USB access, the shell
conventions and debugging.

## Apps by group

One line per app, in the reading order of this part.

### Bench

| App | What it does |
| --- | --- |
| [capture](../../apps/capture/README.rst) | Capture survey on the console |
| [rfprobe](../../apps/rfprobe/README.rst) | Bench probe: tuning, tones, registers |
| [coexbench](../../apps/coexbench/README.rst) | The ESP32-S3's Wi-Fi driver, ESP-NOW and the radio library on one radio: what burst captures cost a Wi-Fi link; results in [Coexistence](../radio/coexistence.md) |
| [rxbench](../../apps/rxbench/README.rst) | How fast UDP reaches the ESP32-S31 CoreBoard's gigabit Ethernet through the MAC driver's receive hook, past the network stack |
| [ptp_chain](../../apps/ptp_chain/README.rst) | The offset between the IEEE 1588 hardware clocks of two ESP32-S31 CoreBoards on a direct gigabit link, from MAC timestamps |

### Streaming

| App | What it does |
| --- | --- |
| [stream](../../apps/stream/README.rst) | VITA 49.2 RX and TX over USB (CDC-NCM) |
| [osmosdr](../../apps/osmosdr/README.rst) | HackRF-style USB SDR for GNU Radio and gqrx |
| [osmosdr_serial](../../apps/osmosdr_serial/README.rst) | The same over USB-Serial-JTAG, UART or SDIO; on the WHY2025 badge's C6 gapless 80 MS/s receive |
| [osmosdr_bridge](../../apps/osmosdr_bridge/README.rst) | The WHY2025 badge's P4 as a USB high-speed osmosdr device, its C6 running `osmosdr_serial` behind it |
| [osmosdr_hs](../../apps/osmosdr_hs/README.rst) | The ESP32-S31 as a USB high-speed osmosdr device, its own radio up to 20 MS/s |
| [stream_eth](../../apps/stream_eth/README.rst) | The ESP32-S31 CoreBoard's gapless receive as VITA 49.2 signal data over UDP on gigabit Ethernet, 20 to 40 MS/s, unicast to whoever subscribes |
| [txstream_eth](../../apps/txstream_eth/README.rst) | The ESP32-S31 DAC streamed over gigabit Ethernet: samples arrive over UDP (`vrt_tx`) and are played by whole-bank flips on CPU 1; protocol in [Transmit stream](../dev/txs.md) |
| [chancap](../../apps/chancap/README.rst) | One capture per packet per channel of a grid, as VITA 49.2 over USB |

### Protocols

| App | What it does |
| --- | --- |
| [link](../../apps/link/README.rst) | QAM or OFDM packet link |
| [wpan](../../apps/wpan/README.rst) | 802.15.4 test shell |
| [zstack](../../apps/zstack/README.rst) | Zigbee coordinator with the Z-Stack ZNP serial protocol |
| [ubertooth](../../apps/ubertooth/README.rst) | Ubertooth One device firmware for the upstream host tools; design in [ubertooth internals](ubertooth-internals.md) |
| [pcap](../../apps/pcap/README.rst) | The WHY2025 badge's P4 decodes its C6's bursts into pcapng for Wireshark; design in [pcap internals](pcap-internals.md) |

### Platform

| App | What it does |
| --- | --- |
| [multiapp](../../apps/multiapp/README.rst) | One firmware that runs the others; design in [multiapp internals](multiapp-internals.md) |
| [micropython](../../apps/micropython/README.rst) | MicroPython REPL on USB, toward GNU Radio flowgraphs |
| [mch2022_coproc](../../apps/mch2022_coproc/README.rst) | Firmware for the MCH2022 badge's RP2040: all the stock one does, with its UARTs on DMA and a bulk link to the ESP32; design in [MCH2022 badge](mch2022.md) |
| `apps/fpga_demo` | Shell commands for the MCH2022 badge's FPGA (the link, graphics, the DSP chain, the badge's radio); no README, see [MCH2022 badge](mch2022.md) |
| `apps/c6_bootstrap` | The ESP32-P4-Function-EV-Board's P4 installs an ESP32-C6 image through the C6's esp-hosted-mcu firmware (its OTA requests over SDIO); no README |

`apps/common` is not an app: it holds what the apps share, USB with DFU, watchdogs, crash records
and the `usj` shell command.

## Apps per chip

`yes` means the app builds and runs on that chip; `yes, m` means it also runs as an extension
inside multiapp there. A board name says where the chip sits. What each chip can do with samples
is in [Samples in and out](../samples/index.md).

| App | ESP32-S3 | ESP32-C6 | ESP32-C5 | ESP32 | ESP32-S31 | ESP32-P4 | RP2040 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| capture | yes, m | yes | yes | MCH2022 | yes, m | | |
| rfprobe | yes, m | yes | yes | | yes, m | | |
| stream | yes, m | | | | | | |
| osmosdr | yes, m | | | | | | |
| osmosdr_serial | | yes, also WHY2025 | yes | MCH2022 | | | |
| osmosdr_bridge | | | | | | WHY2025 | |
| osmosdr_hs | | | | | yes, m | | |
| stream_eth | | | | | yes, m | | |
| chancap | yes | | | | | | |
| link | yes, m | | | | | | |
| wpan | yes, m | yes | | | | | |
| zstack | | yes | | | | | |
| ubertooth | yes | | | | | | |
| pcap | | | | | | WHY2025 | |
| multiapp | yes | | | | yes | | |
| micropython | yes, m | | yes | MCH2022 | VIEWE | WHY2025 | |
| mch2022_coproc | | | | | | | MCH2022 |
| fpga_demo | | | | MCH2022 | | | |
| c6_bootstrap | | | | | | EV board | |

- The WHY2025 badge's C6 builds `osmosdr_serial` with its gapless 80 MS/s receive by default; on
  that badge `pcap` and `osmosdr_bridge` use it as their radio.
- The classic ESP32 of the MCH2022 badge takes bursts only.
- On the ESP32-S31, `micropython` runs on the VIEWE board's display.

## Building and flashing

Released images need no build: the [Web flasher](flasher.md) writes them from the browser.

Each app's page has its build commands. What they have in common:

### Sysbuild and MCUboot

- Every ESP32-S3 app except `apps/capture` needs `west build --sysbuild` (MCUboot, USB DFU).
- A plain build of an existing sysbuild directory fails with a cached-source mismatch.
- Kconfig values passed with `-D` stay in the CMake cache: an old override (for example
  `-Dlink_CONFIG_APP_SPS=8`) persists until set again.

### SoC and board configuration

Zephyr applies two layers of an app's configuration:

| Files | Holds | Example |
| --- | --- | --- |
| `socs/<soc>_<cpu cluster>.conf` and `.overlay` | what the app needs on every board with that chip | `socs/esp32s3_procpu.conf` for the XIAO ESP32S3 and the Communicator |
| `boards/<board>.conf` and `.overlay` | what one board adds | |

The ESP32-S3 apps put their shell on a USB CDC-ACM port with
`apps/common/boards/s3_cdc_shell.dtsi`.

### Badges

#### Hackaday Communicator

- The badge has a reset button but no boot button.
- When no app answers `usj` any more, ROM download mode needs test pad TP1 (IO0) shorted to
  ground while pressing reset.
- A sysbuild `west flash` over its USB-Serial-JTAG does work for the first install, before an app
  owns OTG.

#### WHY2025 badge's ESP32-C6

- The ESP32-C6 (`why2025_badge/esp32c6/hpcore`) reaches USB-C port J8 only while the ESP32-P4
  drives its GPIO2 high.
- With `apps/micropython` on the P4, `west dfu --c6` hands the port over for a while
  (`esdr.usb_to_c6()`) and flashes the C6.
- Otherwise run a P4 image built with `CONFIG_WHY2025_BADGE_USB_TO_C6=y` and without esp-hosted
  (which resets the C6), then flash the C6 over J8 with `west flash`.
- The badge's other quirks (the P4's flash, the backlights, the LoRa module) are in
  [Chip notes](../dev/chips.md).

## DFU updates

On the ESP32-S3 the first flash goes over USB-Serial-JTAG. From then on the app owns the USB port,
so RTS and DTR can no longer reset the chip into the ROM bootloader: later updates go over USB DFU
into MCUboot.

### west dfu

```console
west dfu -s <usb serial>
west dfu -d <build dir> [-s <usb serial>]
```

- `west dfu` is `scripts/west_commands/dfu.py`.
- Its build directory is `build` unless `-d` names another.
- `-s` is needed with several boards.
- It detaches the app into DFU, downloads the image, waits for MCUboot to test-boot it, checks
  that its shell keeps answering and only then confirms it (`mcuboot confirm`).
- The signed image grows in steps of 64 KiB (flash MMU page alignment), so its size is no evidence
  of a new build.
- Gate an update on the build's exit status (a pipe into `grep` hides it), else `west dfu` flashes
  the previous image.

### Test boot and revert

- An image that is never confirmed is reset by a watchdog after `APP_TRIAL_TIMEOUT_S` (60 s by default, 120 s
  on the S31 multiapp; the same real time on every chip) and MCUboot reverts to
  the previous one, so a broken update never needs a replug.
- An S3 image put on by DFU must start USB OTG (`app_usb_init()` in `apps/common`, which also arms
  the trial watchdog) or clear `RTC_CNTL_SW_HW_USB_PHY_SEL` and `RTC_CNTL_SW_USB_PHY_SEL` early in
  boot ([ESP32-S3 registers](../dev/registers-esp32s3.md#register-rtc_cntl_usb_conf_reg-0x60008120)).
- Otherwise it stays off USB until a power cycle: the OTG driver sets both bits and a software
  reset keeps them.

### Recovery with esptool

Without DFU, esptool writes the images over the USB-Serial-JTAG.

#### Handing the port over

- esptool reaches an S3 whose app owns OTG once the app's shell command `usj` (`apps/common`) has
  handed the port to the USB-Serial-JTAG.
- esptool's reset at the end boots the app, which takes the port back.
- MicroPython on an S3 hands the port over with `esdr.shell("usj")`, as the shell apps' `usj`
  does.
- `CONFIG_APP_USB_START_DELAY_MS` keeps the USB-Serial-JTAG alive for a few seconds after reset,
  for recovery builds.

#### west dfu --esptool

- `west dfu --esptool` writes MCUboot, the image and multiapp's volume in one esptool run.
- One run matters: a sysbuild `west flash` runs esptool once per image, and the first one's reset
  leaves the second without a port.
- It needs a running esp-sdr app to hand the port over.

#### By hand

- Write MCUboot at 0x0 and `zephyr.signed.confirmed.bin` at 0x20000 with `esptool`.
- That image is padded to its slot and ends in a confirmed trailer.
- `zephyr.signed.bin` written plain leaves an earlier image's trailer at the slot's end, which
  MCUboot reads as bad magic; `mcuboot confirm` then fails with -5.
- `mcuboot` reports -5 for the secondary slot's header too while that slot holds no image, which
  is harmless.
- A generic ESP32-S3 devkit's first install goes this way, followed by `mcuboot confirm` on the
  shell within 30 s ([rfprobe](../../apps/rfprobe/README.rst)).

## USB IDs and host access

- The apps share the USB IDs 2fe3:0005.
- `west dfu` finds only 2fe3:0005 (app) and 2fe3:ffff (DFU).
- [ubertooth](../../apps/ubertooth/README.rst) says `dfu.py` also knows its Ubertooth ID 1d50:6002.
- Host udev rules match USB product IDs: a build with a new PID cannot be opened (or
  DFU-detached) until a rule covers it.
- [ubertooth](../../apps/ubertooth/README.rst) presents the Ubertooth's IDs by default; its page
  lists them and their udev rule.
- A shell port serves one process: a host tool holding it (such as a running `link_perf.py run`)
  makes another tool's open of the same board fail.

Without root, the PC needs udev rules for the board (USB vendor 2fe3), for example in
`/etc/udev/rules.d/70-esp-sdr.rules`:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="2fe3", MODE="660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="2fe3", MODE="660", GROUP="dialout", TAG+="uaccess"
ATTRS{idVendor}=="2fe3", ENV{ID_MM_DEVICE_IGNORE}="1"
```

On NixOS the same three lines go into `services.udev.extraRules = '' ... '';`.
[stream](../../apps/stream/README.rst) has the network settings for its CDC-NCM link.

## Shell conventions

The apps' shells follow one pattern:

- `status`
- `set <key> <value> ...`
- `clear`
- verbs for actions.

The S3 apps' shell is on a USB CDC-ACM port
([SoC and board configuration](#soc-and-board-configuration)), and `usj` from `apps/common` hands
the USB port to the USB-Serial-JTAG ([Recovery with esptool](#recovery-with-esptool)).
[wpan](../../apps/wpan/README.rst) shows the pattern in full.

## Debugging

### JTAG on an image that owns OTG

- On a running app, `usj` takes the USB device off the bus and selects the USB-Serial-JTAG.
- The PC sees the app go and the USB-Serial-JTAG come, and OpenOCD and esptool work until the
  next boot of the app.
- On a hung one, a shutdown handler (or a `PRE_KERNEL_1` hook in a diagnostic build) that clears
  the two PHY select bits ([Test boot and revert](#test-boot-and-revert)) hands the PHY over at
  the next restart.
- The PC sees no disconnect then; a `USBDEVFS_RESET` ioctl on the port's `/dev/bus/usb` node
  makes it enumerate.

### Resets without a crash record

A reset with no crash record (`multiapp crash`, `link status`) is the TG0 watchdog's first stage: a
CPU wedged with interrupts masked.

### Stack sizes

- With `CONFIG_INIT_STACKS`, run `kernel thread stacks` after exercising every path.
- A measured peak up to 85 % of the size is enough margin.
- An overflowing shell stack shows as 100 % on the logging stack below it.
- Xtensa frame sizes are in the disassembly as `entry a1, N`.

<!-- nav-start -->
Previous: [Error correction](../modem/fec.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Web flasher](flasher.md)
<!-- nav-end -->
