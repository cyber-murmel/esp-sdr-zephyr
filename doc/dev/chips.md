# Chip notes

<!-- nav-start -->
Previous: [Reference](index.md) · Up: [Reference](index.md) · Next: [Code placement](code-placement.md)
<!-- nav-end -->

This page collects chip and board behaviour that Parts 1 and 2 do not cover: CPU and toolchain
costs, reset and boot quirks, board files and displays. Each entry says what happens, how it
shows up and how the code deals with it.

The bench boards are in [The radio](../radio/index.md#bench-setups). Register-level evidence is
on [ESP32-S3 registers](registers-esp32s3.md) and [ESP32-C6 registers](registers-esp32c6.md).
Where code and data live is in [Code placement](code-placement.md).

The chips come in the order S3, C6, C5, P4, S31: the C5's engine builds on the C6's, and the
S31's PIE is described against the P4's.

## ESP32-S3

### CPU and toolchain

Costs of the Xtensa core and its toolchain that matter in hot loops:

| Item | What happens | What the code does |
|---|---|---|
| No hardware float divide | A float `/` calls `__divsf3`. | In hot loops, compare ratios by cross multiplication (`sc_scan()` in `ofdm.c`). |
| Integer `%` by a constant | A `%` by a constant that is not a power of two is a real division too. | Use a power-of-two size and a mask. |
| `lrintf()` | A library call, out of line in flash. Two per sample in the OFDM transmit path cost about 7.5 ms per frame. | Round inline (`emit()` in `ofdm.c`). |
| Many accumulators spill | Loops with four or more running sums get them spilled to the stack: about 100 cycles per step for the sliding Schmidl-Cox sums. | Do less work per step: the OFDM search scans coarsely first. |
| Contraction is off | Zephyr's ISO C mode disables fused multiply-adds. | `apps/link` turns them on for its DSP files (`-ffp-contract=fast`). |
| ROM `memcpy()` and `memmove()` | They are the ROM's on the S3 and copy bytewise. GCC turns copy loops into `memcpy()` calls unless built with `-fno-tree-loop-distribute-patterns`. | The hot paths use the vector unit or inline word copies. |

Why hot code belongs in IRAM is in [Code placement](code-placement.md#why-placement-matters).

### esp-dsp FFT kernel

esp-dsp's S3 FFT kernel assembles empty without the target macro.

- `apps/link` builds only `dsps_fft2r_fc32_aes3_.S` from the `esp-dsp` west project, with
  `-DCONFIG_IDF_TARGET_ESP32S3=1` (hal_espressif's `sdkconfig.h` lacks it).
- It uses its own twiddle table in esp-dsp's layout: cos, sin, bit reversed. One table built for
  N serves every smaller size.
- From flash the kernel is still 1.76 times faster than the same algorithm in C from IRAM: 85.7
  against 150.6 us for 256 points.
- The S3 mask ROM has the twiddle table, but no FFT code.

### USB OTG endpoints

The S3's OTG controller has fewer IN endpoints for classes than the apps' USB classes need
together. How multiapp registers a subset per boot is in
[multiapp internals](../apps/multiapp-internals.md#usb-profiles).

## ESP32-C6

### Modem state survives a chip reset

The C6 keeps the previous firmware's modem state across a chip reset.

#### What happens

- A chip reset of the ESP32-C6 (through USB-Serial-JTAG, as esptool and every port open do)
  leaves its modem subsystem as the previous firmware left it.
- ESP-IDF resets it on a software restart (`esp_system_reset_modules_on_exit()`); a chip reset
  does not.
- After `apps/wpan` had run the C6's native 802.15.4 radio, `esp_sdr` on the next firmware
  received with a noise floor about 25 dB up: rms 40 to 50 instead of 2 at fixed gain 40, the
  same at every gain index and frequency.
- The vendor's channel tuning put the PLL MHz off: 2404 instead of 2412 MHz in its fractional
  divider.

#### How it shows up

- Tones from another board land at the wrong offset, or not at all, at Wi-Fi channel
  frequencies.
- Captures look like a much noisier receiver.
- `tests/regression/lo_tuning` failed its link check whenever it ran after
  `tests/regression/ieee802154_link` ([Testing](testing.md#regression-tests)).

#### How it is handled

On the C6 the library resets the modem subsystem at `PRE_KERNEL_1`, before the Wi-Fi driver
brings the radio up, as ESP-IDF's restart path does (`esp_sdr_modem_reset()` in
`lib/esp_sdr/src/esp_sdr.c`).

The registers it pulses are
[MODEM_SYSCON_MODEM_RST_CONF_REG](registers-esp32c6.md#register-modem_syscon_modem_rst_conf_reg-0x600a9810)
and [MODEM_LPCON_RST_CONF_REG](registers-esp32c6.md#register-modem_lpcon_rst_conf_reg-0x600af024).

#### Clean and stale registers

Bench 2026-10-09, after the native 802.15.4 radio ran (`apps/wpan`) and the chip was only reset,
not power cycled:

| Item | Clean | Stale |
|---|---|---|
| PLL for 2412 MHz through the vendor channel path | 2412 MHz, `0x63` regs 3 to 6 `08 33 33 01` | about 2404 MHz, `08 11 11 00` |
| `0x62` regs 1 and 5 (code) at 2412 MHz | 0xbb (187) | 0xbf (191) |
| `0x61:4` (`I2C_ULP_OCODE`) | 0x53 | 0x4f |
| `0x61:9` | 0x59 | 0x5a |

A power cycle or a deep sleep clears the stale state, and so does the modem reset at
`PRE_KERNEL_1`.

## ESP32-C5

### Board files

The Waveshare ESP32-C5-Zero is board `waveshare_esp32c5_zero/esp32c5/hpcore`, with its files in
`boards/waveshare/esp32c5_zero/`:

- console on USB-Serial-JTAG;
- Espressif's partitions at the C5's 0x2000 boot offset in its 4 MB flash;
- PSRAM disabled;
- simple boot.

The chip itself is in [The radio](../radio/index.md#chip-lineup). The board's antenna switch,
which `board.c` drives, is in [Transceiver](../radio/transceiver.md#board-antenna-selection). How
the image is split around the capture bank is in
[Code placement](code-placement.md#esp32-c5-image-split).

### Dump gate stalls the bus

- Any access to `0x600a20b4` (the C6's dump source gate, in the Bluetooth baseband) takes
  1.64 ms and reads 0.
- Captures work without it, so the C5 build skips it.
- With it, every capture took 1.7 ms longer.

## ESP32-P4

### PIE SIMD

The ESP32-P4's PIE SIMD has 128-bit vectors q0 .. q7, the 40-bit lane accumulators QACC and the
scalar XACC.

- It needs Espressif's assembler: `-march=..._xespv2p1`, the PIE of the P4 before v3; older
  binutils called it `xesppie`.
- The Zephyr SDK's assembler knows no PIE.
- The rules below were found on the WHY2025 badge's P4 (v1.0) and are all applied in
  `apps/common/src/esdr_pfbq_pie.S`.

#### Enabling PIE and the hardware loops

- PIE and the hardware loops are off until their state CSRs are set: 0x7f2 and 0x7f1 to 1.
  Otherwise an instruction is illegal.
- Unaligned vector access needs bit 1 of `esp.movx.w.cfg`, and costs 3 to 4 cycles.

#### Registers and operands

PIE instructions take x8 .. x15 and x24 .. x31 only, `esp.movx.w.sar` included: with `t0` the
assembler reports "illegal operands".

#### Loop setup relocation

- `esp.lp.setup` with a label needs a relocation only Espressif's linker knows. Zephyr's links it
  as another type, even with `-mno-relax`.
- Write it as a word: `0x402b | id << 7 | rs1 << 15 | ((end - .) >> 1) << 20`.
- Binutils 16.2 with `xesploop` resolves the offset itself and emits no relocation.

#### Arithmetic modes

- `esp.srcmb.s16.qacc` shifts with floor, and saturates only with select 1 (0 wraps).
- `esp.cmul.s16` modes 0 and 1 write the low and the high two complex lanes, so it works in
  place. The sums are exact, floored by SAR.
- `vadd`, `vsub` and `vmul` saturate.
- Loading QACC (`esp.ldqa`, `esp.mov.s16.qacc`) gives a rounding addend for free.

#### Issue timing

- Nearly every PIE instruction issues in one cycle, a multiply-add with a fused load too.
- The core issues one instruction a cycle, so fusing halves the cost.
- A vector used by the next instruction stalls one cycle.

#### Vector state and threads

- The vector state is in no thread context: one thread per core may use it.
- The enables and the vector, accumulator and loop registers are per core.
- Under SMP that thread is therefore pinned before it starts (`apps/pcap`). Moved, it would trap
  where PIE is off, or carry on with the other core's state.

### ROM memcpy

- The ESP32-P4's memcpy, memmove and memset are ROM routines (the `esp32p4.rom.libc*.ld` scripts)
  at some 5.5 cycles a byte.
- Moving the channelizer's 1.5 kB history cost 13 cycles a sample until a PIE copy took over.
- Copy hot data with words or vectors.

### Float latency

- The in-order P4 core waits on float results: an independent `fmul.s` issues every cycle, a
  dependent `fadd.s` waits 4.
- Interleave independent chains. The detector's decisions do: four channels as written-out
  scalars, a per-channel threshold instead of byte flags.
- Keep loop bounds in locals: with `-fno-strict-aliasing` every float store reloads struct
  fields.

### Float registers and FPU sharing

The ESP32-P4's float registers are not switched by default.

- Zephyr's SoC defconfig turns `CONFIG_FPU` on without `CONFIG_FPU_SHARING`.
- A thread that preempts another in float code then leaves its own register values behind.
- In `apps/micropython` the channelizer thread (above MicroPython) made `esdr.fb_wf_row()` see a
  valid range as `vmax <= vmin` after seconds to minutes.
- Both P4 apps set `CONFIG_FPU_SHARING=y`.
- The lazy switch works on the v1.0 silicon: Zephyr decodes the trapping instruction from
  `mtval`, which the P4 fills, so ESP-IDF's FPU reason CSR erratum does not matter.

### Branch prediction

- The ESP32-P4's branch prediction is MHCR, CSR 0x7c1: return stack, predictive jumps, BTB.
- ESP-IDF's start code sets it on both cores.
- In Zephyr only the flash cache restore set it, for the core doing the flash operation, so the
  second core could run without it.
- The SMP port sets it on both cores at start.

### Core-1 boot address and stall

The ESP32-P4's core-1 boot address and stall survive warm resets: both sit in the always-on
domain.

- After an image that started core 1, the ROM can release core 1 into whatever code now occupies
  that address.
- ESP-IDF's restart leaves core 1 stalled.
- MCUboot clears the address, resets core 1 and releases the stall first
  (`soc/espressif/esp32p4/hw_init.c`).
- On the WHY2025 badge, with an MCUboot without the first two steps, the first reboot of an SMP
  image wedged the chip: USB stayed enumerated, nothing answered.
- Without the release, a single-core image (MicroPython, pcap) hung early after an SMP image's
  reboot, before its USB came up, while a cold boot was fine.

### WHY2025 badge

#### Flash in DIO mode

- The WHY2025 badge's ESP32-P4 (v1.0) runs its XTX XT25F128 flash in DIO mode, where IO2 and IO3
  are WP# and HOLD#.
- The Espressif HAL cleared the SPI1 control register before erases and status reads, which drove
  both low.
- The flash then ignored the erase and reported idle: erases "succeeded" in under a millisecond
  and changed nothing, while page programs worked.
- Fixed in the HAL (esp_hal_mspi: esp32p4: keep the flash line levels when clearing ctrl).
  Without the fix no DFU image ever reaches the update slot intact.

#### Backlights

The badge's ESP32-C6 drives both backlights: its images hold them on, the P4 cannot.

#### LoRa module

- The badge's LoRa module is an Ai-Thinker Ra-01SH (SX1262), not the Ra-01H (SX1276) its
  schematic names.
- An SX1276 driver reads a constant 0xA2 (the SX126x status byte) and hangs in its image
  calibration loop.
- On the Ra-01H footprint the Ra-01SH's BUSY, DIO1 and RXEN pads are unconnected, and pad 5
  (C6 GPIO3, the Ra-01H's DIO0) is TXEN.
- The board's `semtech,sx1262` node has neither `busy-gpios` nor `dio1-gpios`: the Zephyr driver
  waits out each command's busy time and polls the IRQ status.
- Zephyr's `lora/send` and `lora/receive` samples run between it and the Communicator in both
  directions: -78 and -87 dBm at a metre, 8 dB SNR.

## ESP32-S31

### Sources

The board, the ESP32-S31-Function-CoreBoard-1, is in [The radio](../radio/index.md#chip-lineup).
Receive, a carrier, the CPU-paced transmit and the DAC engine's replay were measured on one such
board.

- The method is ESPARGOS esp-sdr's S31 firmware at 9b61e7465. Where a page notes otherwise, the
  code differs from it.
- Its PHY offsets belong to esp-phy-lib b88e4b76e090, the blob this repository fetches.
- The `lib/esp_sdr` code is `esp_sdr_s31.c`, `esp_sdr_diag16.c` and `esp_sdr_replay.c`.

### PIE on CPU1 only

The ESP32-S31's PIE is CPU1's only, and version 2.2: `-march=..._xesploop_xespv2p2`, binutils
16.2. ESP-IDF says the same in its `vectors.S`.

- On CPU0 the state CSR 0x7f2 reads 0 whatever is written, and a PIE instruction is illegal.
- On CPU1 it reads 0x22000000 plus the state.
- The P4's encodings (2.1) and 3.0's are other instructions there: misaligned stores and
  exception 31.
- So PIE needs SMP and a thread pinned to CPU1.
- The hardware loops (0x7f1, their state in `mstatus.XS`) work on both cores.

esp-dsp's P4 kernels, assembled for 2.2, against its C versions on the VIEWE board's S31
(rev v0.0), 1024 points (`tmp/s31/viewe/apps/piecheck`):

| Kernel | Result |
|---|---|
| 16-bit dot product | exact, 24 times faster |
| float FFTs (FPU and hardware loops only) | exact on either core, 1.5 times faster (radix 4: 88000 cycles) |
| 16-bit FFT | off by up to 146 counts against a float reference, where the C version is off by 2 |
| decimating FIR | wrong in every output; it also returns a register it never sets |

Some instruction differs from the P4's; it is not found yet.

### VIEWE board

The VIEWE PLCM-UES31S043H800V480C-U puts the same chip on a 4.3 inch 800x480 module
(`ues31s043h800v480c_u/esp32s31/hpcore`).

#### Board and console

- A CH340C sits on the USB-C socket; there is no USB-Serial-JTAG connector.
- OpenOCD needs a cable wired to GPIO 33 and 34 on header H1 (not tried).
- The panel was first read back on the pads, not looked at: at a pixel clock of 500 kHz the CPU
  can follow PCLK and compare all 384000 pixels of a pattern.

#### Pixel clock edge and VSYNC_END

- The vendor's `pclk_active_neg` means data changes on PCLK's rising edge and the panel takes it
  on the falling one.
- `lcd_ck_out_edge` is set: PCLK is high in a data cycle's first half. In Zephyr's terms that is
  `pixelclk-active = <1>`.
- VSYNC_END comes as the VSYNC pulse starts, at the frame's first clock and not at its last
  pixel.

#### Underruns

An underrun moves the picture for good.

- The DMA and the controller only meet at the start.
- Pixels the panel missed stay in the DMA's stream; every later frame shows that much later.
- IDF's restart only exists on the S3.
- The panel driver restarts both in the next VSYNC event.

Less than all of it does not do:

- A DMA and FIFO reset under a running controller, as on the S3, left the picture 1328 pixels
  off.
- A stop and start without `lcd_reset` left it 44 to 48 off.

The underrun interrupt:

- fires per pixel: 190000 a second with PSRAM at 40 MHz under load;
- took 19 % of a copy's speed;
- stays off until the restart.

#### Start order

Start the controller first. In IDF's order (DMA, 1 us, controller) the picture was:

- right with idle PSRAM at 200 MHz;
- one pixel late with PSRAM at 40 MHz (10 us did not help);
- one pixel late with the CPU in PSRAM at 200 MHz.

With the controller first, the FIFO fills in the first blanking and all of these are exact.

#### PSRAM and flash speed

| PSRAM clock | CPU read through the cache | Write back | Copy within PSRAM | Panel |
|---|---|---|---|---|
| 250 MHz, the board's clock | 90 MB/s | 69.5 MB/s | 53 MB/s | takes its 50 MB/s, no underrun |
| 200 MHz | 82 MB/s | 60 MB/s | 45 MB/s | |
| 40 MHz | 18.6 MB/s | | 7.8 MB/s | an underrun in every frame |

- The flash reads 28 MB/s in QIO at 80 MHz, 16 MB/s in DIO.
- The PSRAM timing tuning runs with the cache off, so its DQS scheme must be in RAM
  (`mspi_timing_by_dqs` in the linker script). Out of it the boot hangs at "Enter psram timing
  tuning".

#### Radio on the VIEWE board

- The ring's pads, and why the ring and the panel cannot run together:
  [Gapless receive](../samples/rings.md#viewe-board).
- A display's stream, the S31's filter settings and aliasing:
  [Filters](../radio/filters.md#esp32-s31-display-bursts).
- The panel's and the PSRAM's spurs in the receive band:
  [Impairments](../radio/impairments.md#display-panel-and-psram-clock).

<!-- nav-start -->
Previous: [Reference](index.md) · Up: [Reference](index.md) · Next: [Code placement](code-placement.md)
<!-- nav-end -->
