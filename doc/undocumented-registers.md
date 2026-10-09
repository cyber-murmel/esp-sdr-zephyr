# Undocumented registers of the ESP32-S3 and ESP32-C6 radio

What this project knows about the hardware registers that `lib/esp_sdr` relies
on and that Espressif does not document, or documents without the behaviour
that matters: where each one is, its fields, what it does, the values seen, and
how sure we are. How the library copes with the radio's quirks is in
[hardware-quirks.md](hardware-quirks.md).

## How to read this page

- Every entry names its chip. "Both" means the ESP32-S3 and the ESP32-C6.
- Analog I2C registers are written `block:reg[bits]`: `0x62:12[3:2]` is bits
  3:2 of register 12 in analog block 0x62 (upstream esp-sdr uses the same form).
- Evidence paths are relative to this repository (`lib/`, `doc/`, ...) or, for
  files outside it, to the west workspace: `modules/lib/esp-sdr/` is upstream
  esp-sdr (commit 1fe5535), `modules/hal/espressif/` the HAL, `wifi-lib/` the
  PHY library decompile, `.claude/memory/` the project notes. "Bench
  2026-10-09" is a measurement on one XIAO ESP32S3 and one XIAO ESP32C6 with
  scratch tools that are not in the repository.
- `wifi-lib/` is the decompile of the **ESP32-S3** blobs only. Its 22 libphy
  objects (`phy_*.o` except `phy_test.o`) are byte-identical to the members of
  `modules/hal/espressif/zephyr/blobs/lib/esp32s3/libphy.a`; `bb_common.o`,
  `crypto_common.o`, `esp_phy_api.o`, `mac_common.o`, `phy_test.o`,
  `rf_test.o` and `wifi.o` to `zephyr/blobs/lib/esp32s3/librftest.a`
  (esp-phy-lib e37ecdc9). `wifi-lib/c/` is mechanical Ghidra output that drops
  arguments of indirect calls, so argument values below come from the
  disassembly in `wifi-lib/s/`. `wifi-lib/c/pbus_reconstructed.c` is a hand
  sketch that contradicts the disassembly of `adctrig()` and `dactrig()`
  (bit 15 polarity, which register is polled and cleared, a bank select that is
  not there); it is not used as evidence.
- ESP32-C6 entries marked decompile come from `objdump` of the C6 blobs
  (`modules/hal/espressif/zephyr/blobs/lib/esp32c6/libphy.a`,
  `zephyr/blobs/lib/esp32c6/librftest.a`) and of the C6 ROM. ROM addresses
  refer to `esp32s3_rev0_rom.elf` and `esp32c6_rev0_rom.elf` of esp-rom-elfs
  20230320.

| Confidence | Meaning |
|---|---|
| measured | Seen on the bench boards or on air between them. |
| working | The library depends on it and works; the meaning is not proven beyond that. |
| upstream | Stated by upstream esp-sdr (code, comments, docs); not re-checked here. |
| decompile | Read from vendor code (decompile or disassembly); not confirmed on hardware. |
| speculative | A guess. Treat the field as unknown. |

## Analog I2C

### Access functions and host ids

The library reaches the analog blocks through two ROM functions,
`rom_chip_i2c_readReg(block, host, reg)` and
`rom_chip_i2c_writeReg(block, host, reg, value)`, on 8-bit registers. The
Zephyr link maps them to the ROM (ESP32-S3 0x40035548 and 0x40035818 through
the stubs at 0x40005cd0 and 0x40005cdc; ESP32-C6 0x40003dc0 and 0x400040b4).

| Fact | Chip | Evidence |
|---|---|---|
| The `host` argument is ignored. The I2C master is picked from the block id: 0x62, 0x63, 0x64, 0x67 and 0x6b go to master 1, every other block to master 0 (bit mask 0x227 over `block - 0x62`). | both | *decompile*: S3 ROM `rom_chip_i2c_writeReg` overwrites the host register with the block, `rom_chip_i2c_readReg` never reads it; C6 ROM `rom_chip_i2c_readReg` and `rom_get_i2c_hostid` (0x40003d4a, `andi 551`); `wifi-lib/c/phy_i2c.c:218-279`. *measured* (C6): host 0 and 1 read identically in every register of blocks 0x60 to 0x6f (bench 2026-10-09). |
| The ESP32-S3 ROM's own `get_i2c_hostid()` (0x400354bc) sends only 0x62 to 0x64 to master 1. libphy replaces it at PHY init (`g_phyFuns + 0x15c`, `ram_get_i2c_hostid()`), which adds 0x67 and 0x6b. | S3 | *decompile*: `wifi-lib/c/phy_init.c:140` |
| Host values in code are therefore cosmetic and inconsistent: the library passes 0 for BBTOP on the S3 and 1 on the C6, 1 for the RF PLL and the LO selector; upstream's docs say host 1 for BBTOP on every chip while its S3 code passes 0; the HAL headers give hosts that do not match the routing (S3 `I2C_BBPLL_HOSTID 1` for a master 0 block). | both | `lib/esp_sdr/src/esp_sdr_priv.h:61,106`; `modules/lib/esp-sdr/docs/rx-controls.md:43`; `modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:76`; `modules/hal/espressif/components/soc/esp32s3/include/soc/regi2c_bbpll.h:18-19` |
| The ROM functions take no lock (S3 `rom_enter_critical_phy` and `rom_exit_critical_phy`, 0x40055bb8 and 0x40055bc0, are empty). libphy's `ram_chip_i2c_*` call `phy_i2c_enter_critical()`, which the HAL maps to `regi2c_enter_critical()`. The Wi-Fi PHY may run a sequence on the other core at any time, so the library takes `regi2c_enter_critical()` around every `rom_chip_i2c_*` sequence. | both | *decompile*: `wifi-lib/c/phy_i2c.c:239-279`; `modules/hal/espressif/components/esp_phy/src/phy_override.c:50-58`; *working*: `lib/esp_sdr/src/esp_sdr_priv.h:175-180`, `doc/hardware-quirks.md:165-168` |
| An access costs tens of microseconds, so the library writes filter codes at front-end setup, not around every capture. | S3 | *measured*: `lib/esp_sdr/src/esp_sdr_rx.c:110-114` |

### Block map

Register counts are the loop bounds of the vendor's debug dump
`phy_i2c_check()`. ESP32-S3 names are its strings, ESP32-C6 names the HAL's
comment. The HAL defines fields only for 0x61, 0x66, 0x69 and 0x6d (S3) and
0x61, 0x66, 0x69, 0x6a and 0x6d (C6).

| Block | ESP32-S3 | ESP32-C6 | Used by esp_sdr |
|---|---|---|---|
| 0x61 | `i2c_ulp`, 9 registers | ULP, 11 | no (C6 stale-state symptom) |
| 0x62 | `i2c_rfpll`, 13 (0 to 12) | PLL, 19 (0 to 18) | VCO status; C6 LO divider |
| 0x63 | `i2c_rfpll_sdm`, 6 | SDM, 7 | no (read on the bench) |
| 0x64 | `i2c_rfrx`, 11 | absent | no |
| 0x65 | `i2c_ckgen`, 9 | absent | S3 LO divider |
| 0x66 | `i2c_bbpll`, 11 | BBPLL, 11 | no |
| 0x67 | `i2c_bbtop`, 57 | BB, 57 | RX and TX filter codes |
| 0x69 | `i2c_sar`, 8 | SAR, 9 | no |
| 0x6a | `i2c_bias`, 8 | BIAS, 4 | no |
| 0x6b | `i2c_txrf`, 12 | TXRF, 1 to 15 | no |
| 0x6d | `i2c_dig_reg`, 15 | PMU, 15 | no |

Evidence: *decompile* `wifi-lib/c/phy_debug.c:135-210`, C6 `libphy.a`
`phy_debug.o` (`phy_i2c_check`);
`modules/hal/espressif/components/esp_hal_regi2c/esp32c6/regi2c_impl.c:9-19`.
*measured* (C6): blocks 0x60, 0x64, 0x65, 0x68, 0x6c, 0x6e and 0x6f read 0xff
in every register (bench 2026-10-09).

### Block 0x61 (ULP)

| Reg | Bits | Function | Values | Evidence |
|---|---|---|---|---|
| 4 | 7:0 | C6: `I2C_ULP_OCODE` (HAL name, function undocumented). | 0x53; 0x4f after the native 802.15.4 radio ran in the previous firmware (see "ESP32-C6 stale modem state"). | *measured* bench 2026-10-09; `modules/hal/espressif/components/soc/esp32c6/include/soc/regi2c_lp_bias.h:45` |
| 9 | 7:0 | C6: unknown. | 0x59; 0x5a in the same stale state. | *measured* bench 2026-10-09 |
| 8 | 2 | S3: set for the RC time-constant measurement in `get_rc_dout()`, cleared after. | | *decompile* `wifi-lib/s/phy_analog_cal.s:57, 107`, `wifi-lib/c/phy_analog_cal.c:22, 28` |

C6 registers 0 to 10 at 2412 MHz: `1f 02 08 49 53 40 51 2f 33 59 20` (bench
2026-10-09).

### Block 0x62 (RF PLL)

Both chips, the RF PLL's VCO and its calibration. The library reads
`0x62:12[3:2]` after every tune and, on the C6, drives the LO divider in
register 16. The capacitor code is `reg 5 | reg 7[2] << 8` (9 bits; the vendor
clamps to 0x1ff); a higher code means a lower VCO frequency.

| Reg | Bits | Name | Function | Values | Evidence |
|---|---|---|---|---|---|
| 0 | 6, 5 | calibration start | `restart_cal()` starts the VCO calibration with a rising edge on bit 5 while bit 6 is toggled: the S3 sets bit 6, pulses bit 5, clears bit 6; the C6 clears bit 6, pulses bit 5, sets bit 6. Bit 6 and the other bits: unknown. | S3 init 0xa8; C6 read 0x68 | *decompile* `wifi-lib/s/phy_rfpll.s:11-51`, C6 ROM `restart_cal` (0x4000587e), `wifi-lib/c/phy_i2c.c:353`; *measured* C6 dump |
| 1 | 7:0 | manual code, bits 7:0 | `write_pll_cap()` writes the code here (`rf_test` prints it as `ir_cap_ext`). After a tune it holds the low byte of the code in use. | S3: 127 with code 383, 208 at 2412 MHz | *decompile* `wifi-lib/s/phy_rfpll.s:274-295`, `wifi-lib/c/rf_test.c:705`; *measured* bench 2026-10-09 |
| 2 | 4 | manual code, bit 8 | Written with reg 1. | | *decompile* `wifi-lib/s/phy_rfpll.s:286-294` |
| 2 | 7, 3:0 | 4-bit code override | S3 `get_rf_freq_init()` clears bit 7, calibrates at 2437 MHz, copies reg 6 bits 3:0 into bits 3:0 and sets bit 7. What the 4-bit code controls is unknown. | S3 init 0x88; C6 read 0x8d with reg 6 = 0x0d | *decompile* `wifi-lib/c/phy_hw_freq.c:487-494`, `wifi-lib/c/phy_i2c.c:355`; *measured* C6 dump |
| 4 | 7:0 | unknown | | C6: 0x28 (40) at every PLL frequency read, 2280 to 2438 MHz | *measured* bench 2026-10-09 |
| 5 | 7:0 | code readback, bits 7:0 | | see "Measured PLL behaviour" | *decompile* `wifi-lib/s/phy_rfpll.s:303-325`; *measured* |
| 6 | 3:0 | calibrated 4-bit code | Copied into reg 2 by the vendor. | | *decompile* as reg 2 |
| 7 | 2 | code readback, bit 8 | | | *decompile* `wifi-lib/s/phy_rfpll.s:313-323`; *measured* |
| 7 | 1 | calibration done | `wait_rfpll_cal_end()` polls it every 20 us, 100 times, then prints `error: pll_cal exceeds 2ms!!!`. | 1 after every tune | *decompile* `wifi-lib/s/phy_rfpll.s:109-143`; *measured* |
| 7 | 0 | unknown | | S3: 1 in every read (reg 7 = 0x03 or 0x07) | *measured* bench 2026-10-09 |
| 11 | 6 | code source | Cleared before each software calibration (`set_rfpll_freq()`), set by `rfpll_cap_init_cal()` before it tries manual codes. Inferred: 0 = code from the calibration engine, 1 = code from reg 1 and reg 2 bit 4. | S3 init 0x48; C6 read 0x44 | *decompile* `wifi-lib/s/phy_rfpll.s:575-592, 685-697`, C6 ROM `set_rfpll_freq` and `rfpll_cap_init_cal` |
| 12 | 3:2 | VCO window comparator | After calibration: 0 = VCO inside its tuning window; 1 = target below the window (code pinned at its maximum); 2 = target above it (code 0). Live for the current code: the vendor's `rfpll_cap_correct()` moves the code in steps of 2, up while it reads 1 and down while it reads 2. `esp_sdr_set_freq()` and `esp_sdr_set_freq_offset()` return -ERANGE when it is not 0. | windows (PLL): S3 2193 to 2781 MHz, C6 2140 to 2884 MHz | *measured* bench 2026-10-09; *working* `lib/esp_sdr/src/esp_sdr.c:78-97, 194-196, 213-215`; *decompile* `wifi-lib/c/phy_rfpll.c:164-259` |
| 12 | 1 | unknown | | set from about 2700 MHz (PLL) up, both chips | *measured* bench 2026-10-09 |
| 12 | 0 | unknown | | C6: set below about 2412 MHz; S3: always 0 | *measured* bench 2026-10-09 |
| 14 | 7, 4:0 | C6: charge pump calibration done, result | See reg 15. | C6 read 0x14 | *decompile* C6 `libphy.a` `phy_rfpll.o` (`rfpll_chgp_cal`) |
| 15 | 6, 5, 4:0 | C6: charge pump calibration | `rfpll_chgp_cal()` clears bit 6, pulses bit 5, polls reg 14 bit 7 (20 us, 100 tries), reads r = reg 14 bits 4:0, sets bit 6 and writes bits 4:0 = min(31, 7r / 6 + 9). The C6 read reg 14 = 0x14 and reg 15 = 0x74 at 2412 MHz, which this formula does not produce, so the reading is incomplete or reg 14 changes later. | C6 read 0x74 | *decompile* as reg 14; *measured* C6 dump |
| 16 | 3 | C6: LO 5/6 divider | See "LO 5/6 divider". | | *measured* |
| 17, 18 | 7:0 | C6: unknown | | reg 17: 0xff at 2412, 0x7e at 2413 MHz; reg 18: 0x60 | *measured* C6 dump |

- After `set_rf_freq_offset()` (S3) the code is a manual one:
  `rfpll_cap_init_cal()` tries about 10 codes on either side of the calibrated
  one and keeps the average of those that read in-window (*decompile*
  `wifi-lib/c/phy_rfpll.c:265-313`).
- The Wi-Fi PHY's temperature tracking (`rfpll_cap_track()`, after a change of
  10 temperature units by default) reruns `rfpll_cap_correct()`, so the code can
  move after a tune (*decompile* `wifi-lib/c/phy_track.c:184-220`).
- Upstream esp-sdr does a manual 512-code search on the ESP32-S2 through the
  same fields (reg 1, reg 2 bit 4, reg 11 bit 6, reg 0 bit 7, and reg 12 bits
  3:2 as the window indication); it does not apply it to the S3 or the C6
  (*upstream* `modules/lib/esp-sdr/main/targets/esp32s2/pll.h:8-36`).
- C6 registers 0 to 18 at 2412 MHz:
  `68 bb 8d 61 28 bb 0d 03 80 f0 a0 44 00 00 14 74 07 ff 60` (bench 2026-10-09).

### Block 0x63 (RF PLL fractional-N divider)

**ESP32-S3**, registers 0 and 3 to 5:

`f_PLL = 0.75 * f_xtal * (32 + r3 + r4 / 2^8 + r5 / 2^16)`

With the 40 MHz crystal that is 30 MHz per integer step and about 458 Hz per
LSB. `write_rfpll_sdm()` writes reg 0 = 0x07, regs 3 to 5, then reg 0 = 0x17
(bit 4 presumably loads the word).

| PLL MHz | reg 3 | reg 4 | reg 5 | Decoded |
|---|---|---|---|---|
| 2412 | 0x30 | 0x66 | 0x66 | 30 MHz x 80.4 |
| 2500 | 0x33 | 0x55 | 0x55 | 30 MHz x 83.33 |
| 2700 | 0x3a | 0x00 | 0x00 | 30 MHz x 90 |

Evidence: *decompile* `wifi-lib/c/phy_rfpll.c:62-82` (`rfpll_set_freq()`),
`wifi-lib/s/phy_rfpll.s:59-98` (`write_rfpll_sdm()`); *measured* bench
2026-10-09: the raw values match the formula exactly.

**ESP32-C6**, registers 0 and 3 to 6:

`f_PLL = 1.5 * f_xtal * (32 + r3 + (r4 * 2^11 + r5 * 2^3 + r6[2:0]) / 2^19)`

That is 60 MHz per integer step with the 40 MHz crystal. The vendor computes a
word `W = r3 << 19 | r4 << 11 | r5 << 3 | r6` with
`f_PLL = 3 * f_xtal * (16 + W / 2^20)`; W bit 27 is computed but not written.
`write_rfpll_sdm()` clears `0x63:0[3]`, writes regs 3 to 6 and sets the bit
again.

| PLL MHz | reg 3 | reg 4 | reg 5 | reg 6 | Note |
|---|---|---|---|---|---|
| 2412 | 0x08 | 0x33 | 0x33 | 0x01 | |
| 2413 | 0x08 | 0x37 | 0x77 | 0x03 | |
| 2417 | 0x08 | 0x48 | 0x88 | 0x04 | |
| 2437 | 0x08 | 0x9d | 0xdd | 0x06 | |
| 2438 | 0x08 | 0xa2 | 0x22 | 0x01 | |
| 2280 | 0x06 | 0x00 | 0x00 | 0x00 | LO 1900 MHz through the divider |
| 2404 | 0x08 | 0x11 | 0x11 | 0x00 | asked for 2412 MHz in the stale modem state |

Evidence: *measured* bench 2026-10-09; *decompile* C6 ROM `write_rfpll_sdm`
(0x400058ee), C6 `libphy.a` `phy_rfpll.o` (`ram_rfpll_set_freq`). The bench
reading `60 MHz * (N + F / 2^20)` with N = 32 + reg 3 and
`F = r4 << 12 | r5 << 4 | r6` is the same formula except that the vendor code
weights reg 6 twice as much (it holds W bits 2:0), a difference below 0.2 kHz.
The bench reading also took reg 3 as the low 4 bits of N; the vendor code puts
8 bits there (N - 32), which agrees for every value seen. C6 reg 0 read 0x5b
and reg 1 0xab at 2412 MHz; their other bits are unknown.

On both chips the crystal selector of the vendor calls picks the divisor: 1 =
26 MHz, 2 = 32 MHz, anything else = 40 MHz (*decompile*
`wifi-lib/c/phy_rfpll.c:69-72`, C6 `ram_rfpll_set_freq`).

### Block 0x65 (ESP32-S3 clock generator) and the LO 5/6 divider

| Chip | Location | Function | Evidence |
|---|---|---|---|
| ESP32-S3 | `0x65:0[4]` (mask 0x10) | 0: the LO is the PLL. 1: LO = PLL / 1.2 (5/6 conversion). One selector feeds both the receive and the transmit mixer. | *upstream* `modules/lib/esp-sdr/main/common/rx_lo.h:47-62`, external-tone tests `modules/lib/esp-sdr/docs/rx-controls.md:76-100`; *measured* on air for receive and transmit (bench 2026-10-09) |
| ESP32-C6 | `0x62:16[3]` (mask 0x08) | Same. The C6 has no block 0x65. | same |

- Change only the bit (read-modify-write). Clear it before the vendor tuning
  call, which calibrates in normal mode; set it after the receive or transmit
  front-end setup (*working* `lib/esp_sdr/src/esp_sdr.c:106-108`,
  `lib/esp_sdr/src/esp_sdr_rx.c:92-95`, `lib/esp_sdr/src/esp_sdr_tx.c:25-26, 77-83`).
- Without the select on the transmit side, transmissions in the divider band
  went out at 6/5 of the target (*measured* bench 2026-10-09).
- The library uses the divider for every frequency below 2210 MHz (PLL at 1.2
  times the LO), upstream only from 1842 MHz (`lib/esp_sdr/src/esp_sdr.c:65-76`,
  `modules/lib/esp-sdr/main/common/rx_lo.h:15-19`).
- The S3 libphy and librftest objects only read block 0x65 in their debug dump
  (`wifi-lib/s/phy_debug.s:533`); a scan of the C6 blob objects found no
  access to `0x62:16` (*decompile*, objdump).
- Upstream qualifies the selector on the ESP32, S2, S3, C2, C3 and C6 and warns
  that the C5, C61, H2 and S31 lay out their analog blocks differently
  (`modules/lib/esp-sdr/docs/rx-controls.md:102-107`).

### Block 0x66 (BBPLL)

Documented by the HAL. Upstream's never-released `SAMPLE_RATE_PROBE` builds
write `0x66:4[3:2]` (`I2C_BBPLL_DIV_ADC`) around a capture to look for other
ADC rates and record no result; for the C6 upstream reports that no reliable
lower-rate path was found (*upstream*
`modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:140-155`,
`modules/lib/esp-sdr/docs/rx-controls.md:59-61`). esp_sdr does not touch the
block. C6 registers 0 to 10 at 2412 MHz:
`18 25 50 08 6b 80 73 00 c2 96 03` (bench 2026-10-09).

### Block 0x67 (BBTOP, analog baseband)

| Reg | Bits | Function | Values | Evidence |
|---|---|---|---|---|
| 4, 5 | 5:0 | Receive low-pass capacitor code, I (4) and Q (5). Larger is narrower; 0 is the widest setting, not a bypass. Bits 7:6 are kept. | 0 to 63. S3 about 69 MHz (0) to 13 MHz (60) two-sided, PHY default about 22 MHz; C6 54 to 12 MHz | *working* `lib/esp_sdr/src/esp_sdr_rx.c:126-148`; *upstream* curves `modules/lib/esp-sdr/main/common/rx_bandwidth.h:79-84`; *measured* (S3 default) `.claude/memory/esp-sdr-zephyr.md:348` |
| 4, 5 | 5:0 | Vendor scaling: the corner goes as 1 / (code + 8). `phy_rx_band_set(1, bw)` writes `clamp((base + 8) * 19 / min(bw, 19) - 8, 2, 60)` to both; `base` comes from the RC calibration (block 0x6a). | | *decompile* `wifi-lib/c/phy_api.c:141-159` |
| 6, 7 | 5:0 | Second capacitor pair, unknown, probably a second receive stage. Written at init and, with 0x0e and 0x0f, for 10 and 5 MHz channels. The library leaves it alone. | | *speculative*; *decompile* `wifi-lib/c/phy_i2c.c:338-339`, `wifi-lib/c/phy_feature.c:172-174` |
| 0x0c, 0x0d | 5:0 | Transmit low-pass pair A. | S3: 63 narrows TX to about +-6 MHz; codes below the calibrated one (about 35) do not widen it | *measured* `.claude/memory/esp-sdr-zephyr.md:416-418`; *decompile* `wifi-lib/c/rf_test.c:73-108` |
| 0x0e, 0x0f | 5:0 | Transmit low-pass pair B. | | *decompile* `wifi-lib/c/rf_test.c:73-108`; *working* `lib/esp_sdr/src/esp_sdr_rx.c:578-637` |
| 2 | 3:2 | unknown | 1 at init; `phy_close_rf()` writes reg 2 = 6 | *decompile* `wifi-lib/c/phy_i2c.c:352`, `wifi-lib/c/phy_init.c:120` |
| 3 | 2 | unknown; saved, changed and restored by the receive calibration; the S3 frequency hop engine writes reg 3 as 0xf0 and 0xf4 | | *speculative* `wifi-lib/c/phy_rx_cal.c:749-750, 842`, `wifi-lib/c/phy_hw_freq.c:619-627` |
| 0x14 to 0x17, 0x1c to 0x1f | 5:0 | unknown pairs written from PHY parameters at init; 0x1c to 0x1f are stepped with 0x0c and 0x0d in a TX DC calibration test | | *speculative* `wifi-lib/c/phy_i2c.c:342-350`, `wifi-lib/c/phy_test.c:679-742` |
| 0x24 to 0x38 | 7:0 | init constants | 0x24, 0x28: 0x48; 0x25, 0x29: 0x08; 0x2c, 0x2d, 0x30, 0x31: 0x88; 0x34, 0x35: 0x11; 0x38: 0xff | *decompile* `wifi-lib/c/phy_i2c.c:320-351` |

- Reg 4 and reg 6 both reading 0x10 is the vendor's sign that BBTOP lost its
  state; it then reruns its init (*decompile*
  `wifi-lib/c/phy_i2c.c:285-297`).
- On the S3, writing the other pairs (0x06/0x07 up to 0x1e/0x1f) disturbed
  reception until a reboot (*measured* `.claude/memory/esp-sdr-zephyr.md:418`).
  `esp_sdr_bbtop_read()` and `esp_sdr_bbtop_write()` give unchecked access
  (`lib/esp_sdr/src/esp_sdr_rx.c:648-667`).
- The C6 libphy groups the codes the same way (`filter_dcap_set()`: regs 4, 5,
  12, 13 from one PHY parameter, 6, 7, 14, 15 from the next), so the transmit
  pairs are probably at the same place on the C6 (*decompile* C6 `libphy.a`
  `phy_i2c.o`). `esp_sdr_tx_set_lpf()` is S3 only.
- The PHY's channel bandwidth mode changes these filters too: in mode 0 the S3
  transmit filter passes about +-10 MHz, in mode 2 the path passes +-18 MHz end
  to end (*measured* `.claude/memory/esp-sdr-zephyr.md:406`,
  `doc/hardware-quirks.md:56-65`).

### Block 0x6a (bias): RC calibration

ESP32-S3, *decompile*, not used by esp_sdr. `get_rc_dout(mode)` measures an
on-chip RC time constant: it writes `0x6a:2[6:5]` = 2, `0x6a:6[4:0]` = 2,
`0x6a:4[7:4]` = 7, 11 or 6 by mode, `0x61:8[2]` = 1 and `0x6a:4[0]` = 1,
pulses `0x6a:4[3]`, waits 100 us, reads `0x6a:5[5:0]` and clears
`0x61:8[2]` and `0x6a:4[0]`. `rc_cal()` turns the result into the receive
filter base code, `clamp((dout + 56) * 82 / 190 - 8, 2, 63)`, so the filter
codes follow process spread. Evidence: `wifi-lib/s/phy_analog_cal.s:12-144`,
`wifi-lib/c/phy_analog_cal.c:10-87`. On the C6 the HAL names the block
(`I2C_BIAS`); registers 0 to 3 at 2412 MHz read `cf 7c 1a 07` (bench
2026-10-09).

## Memory-mapped registers

### ESP32-S3 dump and DAC engine

The Wi-Fi MAC's debug dump engine moves raw baseband words between the radio
and one 64 KiB SRAM bank (see "SRAM bank ownership"). Its page, 0x60033000,
has no name in the HAL. Layout from upstream esp-sdr; the DAC side from
librftest's `dactrig()`.

**DUMP_CTRL, 0x60033d5c** (receive side)

| Bits | Name | Function | Values | Evidence |
|---|---|---|---|---|
| 31 | RUN | Enables the engine. | | *working* `lib/esp_sdr/src/esp_sdr_rx.c:300-310` |
| 30:28 | | Vendor `adctrig()` second argument: 0 = software trigger (bit 19); 5 and 6 make it send frames while it waits. | 0 | *decompile* `wifi-lib/s/mac_common.s:1272-1300`, `wifi-lib/c/mac_common.c:689-784` |
| 27:20 | | Vendor third argument; upstream calls it the source. | 0 | *decompile* as above; *upstream* `modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:164` |
| 19 | TRIGGER | Write 1, then 0, to start a one-shot capture. | | *working* `lib/esp_sdr/src/esp_sdr_rx.c:308-310` |
| 18 | DONE | Reads 1 once COUNT words are written. | library timeout 20 ms | *working* `lib/esp_sdr/src/esp_sdr_rx.c:311-314` |
| 17 | CIRCULAR | With RUN and no trigger, the engine writes continuously round a 16384-pair ring (`0x00024000` = bit 17 plus count 16384). In `adctrig()` the same bit skips the final count check. | | *working* `lib/esp_sdr/src/esp_sdr_ring.c:41-42, 876-881`; *decompile* `wifi-lib/c/mac_common.c:741` |
| 16 | 16 MS/s | Separate 16 MS/s path. `adctrig()` sets it unless bit 0 of its fourth argument is set. | 4096 samples in 256 us | *measured* `apps/capture/README.rst:56`; *upstream* (USRP B210) `modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:161-163`; *decompile* `wifi-lib/s/mac_common.s:1292-1294` |
| 15 | 40 MS/s | 80 MS/s halved, through the radio's decimation filter (each symbol spread over about +-8 samples, same MER as 80 MS/s). | 4096 samples in 102 us | *measured* `apps/capture/README.rst:53`, `doc/hardware-quirks.md:106-114` |
| 14:0 | COUNT | Words to capture. The library uses bits 13:0 (max 16380, `ESP_SDR_SAMPLES_MAX`); `adctrig()` writes count + 1 and checks a 15-bit written count, and the ring writes 16384 (bit 14), so the field is probably 15 bits wide. | 256 to 16380 | *working* `lib/esp_sdr/src/esp_sdr_priv.h:33`; *decompile* `wifi-lib/c/mac_common.c:652-653, 735-744` |

Neither bit 15 nor bit 16: 80 MS/s (4096 samples in 51 to 60 us). One-shot
sequence: DUMP_CTRL = 0, DUMP_CONFIG, bank select, `RUN | rate | count`, the
same with TRIGGER, the same without, poll DONE, DUMP_CTRL = 0, restore the bank
select (`lib/esp_sdr/src/esp_sdr_rx.c:246-326`). The engine can stop early
without any error flag: the library writes the sentinel 0xa5a0055a into the
first and last word and fails the capture if either survives (*measured*
`doc/hardware-quirks.md:131-134`, `lib/esp_sdr/src/esp_sdr_rx.c:287-293, 321-323`).

**DUMP_WRITE_INDEX, 0x60033d60**

| Bits | Function | Evidence |
|---|---|---|
| 14:0 | Words written: after DONE, `adctrig()` compares this 15-bit value with count + 1. | *decompile* `wifi-lib/c/mac_common.c:735-744` |
| 13:0 | In circular mode: the live write index, mod 16384, advancing whichever bank is selected. The ring times its bank switches on it. | *working* `lib/esp_sdr/src/esp_sdr_ring.c:40, 116-119`; *upstream* `modules/lib/esp-sdr/main/common/ring_capture.c:112` |

No read pointer is known for the DAC side; loop playback runs open loop on
CCOUNT (*measured* `.claude/memory/esp-sdr-zephyr.md:263-264`).

**DAC_TRIG, 0x60033d64** (transmit side, 8 bytes above DUMP_CTRL)

| Bits | Name | Function | Values | Evidence |
|---|---|---|---|---|
| 31 | RUN | | | *working* `lib/esp_sdr/src/esp_sdr_tx.c:146-156` |
| 27:20 | | `dactrig()` writes count & 0xff here; no effect on the rate. | | *measured* `lib/esp_sdr/src/esp_sdr_priv.h:46-48`; *decompile* `wifi-lib/s/mac_common.s:1726-1737` |
| 19 | TRIGGER | Pulsed: one pass. Held: the engine replays the first COUNT words back to back, gapless, until the register is cleared. | a retrigger leaves a hole of about 1.35 us | *measured* `lib/esp_sdr/src/esp_sdr_tx.c:197-199, 292-302`, `lib/esp_sdr/src/esp_sdr_priv.h:236-244` |
| 18 | DONE | | | *working* `lib/esp_sdr/src/esp_sdr_tx.c:158-167` |
| 16 | | No effect on the rate: there is no 16 MS/s DAC rate. | | *measured* `lib/esp_sdr/src/esp_sdr_priv.h:46-48` |
| 15 | 80 MS/s | The DAC has its own clock: 1 = 80 MS/s, 0 = 40 MS/s. | 40 MS/s about 4 dB less MER | *measured* `lib/esp_sdr/src/esp_sdr_priv.h:46-49`, `doc/hardware-quirks.md:116-117` |
| 13:0 | COUNT | Words to play. | 256 to 16380 | *working* `lib/esp_sdr/src/esp_sdr_tx.c:148-149` |

- Writing 0 stops the engine at once (`esp_sdr_dac_halt()`).
- In loop mode the DAC clock is exact (CPU stores to other banks do not slow
  it) and the engine reads whichever bank is selected at the moment, so a bank
  switch takes effect mid-pass (*measured* `lib/esp_sdr/src/esp_sdr_priv.h:236-244`,
  `.claude/memory/esp-sdr-zephyr.md:263-264`).
- Vendor `dactrig(count, a2, a3, ...)` fills bank 2 (0x3fcd0000) with a ramp,
  writes count to bits 13:0 and 27:20, sets bit 15 unless a2 is 1, sets bit 19
  (and leaves it set) only if a3 is not 0, sets RUN, polls its own bit 18, waits
  10 ms and clears DUMP_CTRL (0x60033d5c), not itself. It never writes the bank
  select (*decompile* `wifi-lib/s/mac_common.s:1667-1772`). The library skips
  the ramp so the caller's samples survive, and clears DUMP_CTRL after every
  transmit session as well (`lib/esp_sdr/src/esp_sdr_tx.c:222-223, 311-312`).
- The front end must be forced to transmit, or only a spur near -6 MHz leaks
  (see "Front end").

**DUMP_CONFIG, 0x60033d90**

| Bits | Function | Values | Evidence |
|---|---|---|---|
| 23:0 | Four 6-bit lane selectors (lane n in bits 6n+5:6n) choosing the internal sources packed into a dump word. | `0x000c2040` = sources 0, 1, 2, 3: receive I/Q | *working* `lib/esp_sdr/src/esp_sdr_priv.h:34-36`; *upstream* `modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:159` |
| 24 | Upstream probe flag (`rx_agc`). | 0 | *upstream* as above |

librftest's `mac_init()` writes 0x000c2040 here once (*decompile*
`wifi-lib/c/mac_common.c:594`); the library writes it before every capture and
ring run. The reading as lane selectors is upstream's.

**Wi-Fi MAC interrupt.** Not an engine register, but its companion: CPU
interrupt line 0 (`ETS_WMAC_INUM`,
`modules/hal/espressif/components/soc/esp32s3/include/soc/soc.h:212`) fires for
every packet the Wi-Fi receiver decodes at the tuned frequency, also while the
library owns the radio, hundreds of times a second on busy air.
`esp_sdr_ring_run()` disables it on its CPU for the run (*measured*
`doc/hardware-quirks.md:146-158`, `lib/esp_sdr/src/esp_sdr_ring.c:462-491`).

### ESP32-C6 dump engine

Same control bits at another address, from upstream (`families/c5_c6_c61`,
`targets/esp32c6/chip.h`); 80 MS/s only, no DAC side known (the C6 `dactrig()`
only writes a ramp into 0x40840000 and touches no register, *decompile* C6
`librftest.a` `mac_common.o`). Base 0x600a9000 has no name in the HAL.

| Address | Bits | Function | Values | Evidence |
|---|---|---|---|---|
| 0x600a9004 | 31, 19, 18, 13:0 | DUMP_CTRL: RUN, TRIGGER, DONE, COUNT as on the S3; no rate field. Write COUNT with DONE first to clear a stale DONE, as the stock sequence does. | 256 to 16380; 16380 samples take about 205 us | *working* `lib/esp_sdr/src/esp_sdr_priv.h:77-81`, `lib/esp_sdr/src/esp_sdr_rx.c:302-310`; *upstream* `modules/lib/esp-sdr/main/targets/esp32c6/chip.h:43-46` |
| 0x600a9004 | 17 | Circular ring, in upstream's C6 ring code. Not used here. | | *upstream* `modules/lib/esp-sdr/main/common/ring_capture.c:104, 1499-1505` |
| 0x600a9008 | 18:15 | Dump source; 15 = raw receive I/Q, as the stock `adctrig()`. | 15 | *working* `lib/esp_sdr/src/esp_sdr_priv.h:82-84`; *upstream* `modules/lib/esp-sdr/main/targets/esp32c6/chip.h:6, 39` |
| 0x600a9008 | low bits, read | Live write index in upstream's ring code (same address). | | *upstream* `modules/lib/esp-sdr/main/common/ring_capture.c:96, 1507` |
| 0x600a9014 | 24:0 | Lane selectors as S3 0x60033d90 (field cleared, then set). | `0x000c2040` | *working* `lib/esp_sdr/src/esp_sdr_rx.c:258`; *upstream* `modules/lib/esp-sdr/main/targets/esp32c6/chip.h:38` |
| 0x600a20b4 | 0 | Cleared before every capture, as the stock sequence; function unknown (upstream's ESP32-H2 notes call it the trigger source). | 0 | *working* `lib/esp_sdr/src/esp_sdr_rx.c:261`; *upstream* `modules/lib/esp-sdr/main/targets/esp32c6/chip.h:40`, `modules/lib/esp-sdr/docs/esp32h2.md:20` |
| 0x600a9804, 0x600a980c, 0x600a9814 | all; all; 18:0 | Documented MODEM_SYSCON clock registers (CLK_CONF, with CLK_DATA_DUMP_EN in bit 31 and CLK_DATA_DUMP_MUX in bit 21; CLK_CONF_POWER_ST; CLK_CONF1). Undocumented part: the engine needs them forced on before each capture, as the vendor's `rftest_init()`/`phy_set_clk_conf()` does. | 0xffffffff, 0xffffffff, 0x7ffff (written 9804, 9814, 980c) | *working* `lib/esp_sdr/src/esp_sdr_rx.c:255-257`; *upstream* `modules/lib/esp-sdr/main/targets/esp32c6/chip.h:35-36`; `modules/hal/espressif/components/soc/esp32c6/include/modem/modem_syscon_reg.h:21-27, 82-87, 151, 281` |

### SRAM bank ownership

| Chip | Register | Field | HAL says | Evidence |
|---|---|---|---|---|
| ESP32-S3 | 0x600c101c `SENSITIVE_INTERNAL_SRAM_USAGE_3_REG` | `MAC_DUMP_USAGE`, bits 3:0 | the field, no description | `modules/hal/espressif/components/soc/esp32s3/register/soc/sensitive_reg.h:101-107` |
| ESP32-C6 | 0x60095004 `HP_SYSTEM_SRAM_USAGE_CONF_REG` | `SRAM_USAGE`, bits 11:8 | "0: cpu use hp-memory, 1: mac-dump accessing hp-memory" | `modules/hal/espressif/components/soc/esp32c6/register/soc/hp_system_reg.h:46-62` |
| ESP32-C6 | same | `MAC_DUMP_ALLOC`, bit 16 | adds a 64 KiB offset | same, lines 63-68; the library leaves it 0, upstream's ring clears it |

| Chip | Bit | Bank | Use in esp_sdr |
|---|---|---|---|
| ESP32-S3 | 0 | 0x3fcb0000, 64 KiB | ring (`CONFIG_ESP_SDR_RING`) |
| ESP32-S3 | 1 | 0x3fcc0000 | second DAC and capture bank (`CONFIG_ESP_SDR_BANK1`), ring |
| ESP32-S3 | 2 | 0x3fcd0000 | capture bank; the one `adctrig()` and `dactrig()` use |
| ESP32-S3 | 3 | 0x3fce0000 | never: it holds ROM data |
| ESP32-C6 | 8 + n | 0x40800000 + n x 128 KiB | n = 2 (0x40840000, the stock `adctrig()` bank) is the capture bank |

The field is one-hot: bit n hands bank n to the engine, for capture and
playback alike; 0 gives every bank back to the CPUs. The library saves the
register and restores it after each use (`lib/esp_sdr/src/esp_sdr_rx.c:246-278`,
`lib/esp_sdr/src/esp_sdr.c:123-133`). Undocumented behaviour:

- While the engine owns a bank the CPUs cannot reach any of it, so nothing else
  may live there; the linker snippets reserve the banks and lower
  `_heap_sentry` (*measured* `doc/hardware-quirks.md:124-130`,
  `lib/esp_sdr/esp_sdr_bank_ring.ld:4-14`).
- CPU stores reach the DAC only if made while the CPU owns the bank; otherwise
  the DAC replays the old content (*measured*
  `lib/esp_sdr/src/esp_sdr_tx.c:392-396`, `.claude/memory/esp-sdr-zephyr.md:154-156`).
- The engine follows a change of the field while it runs, in both directions:
  the ring splits one continuous capture into gapless units, the DAC switches
  banks mid-pass (*measured* `doc/hardware-quirks.md:139-145`,
  `.claude/memory/esp-sdr-zephyr.md:177-178, 263-264`).
- ESP32-S3: bank 3 holds ROM data (0x3fceffxx); handing it to the engine
  wedges the board, USB and shell die (*measured*
  `.claude/memory/esp-sdr-zephyr.md:177-181`; *upstream*
  `modules/lib/esp-sdr/main/common/ring_capture.h:26`).
- ESP32-C6: the engine owns the whole 128 KiB block during a capture, although
  a capture uses at most 64 KiB, so the library's other buffers live in block 3
  (*measured* `.claude/memory/esp-sdr-c6.md:64-66`,
  `lib/esp_sdr/esp_sdr_bank_c6.ld:18-20`). On the RISC-V core the switch must
  land before the engine starts: a `fence rw, rw` and a read-back follow each
  write (*working* `lib/esp_sdr/src/esp_sdr_rx.c:262-265, 274-277`).

### AGC force

| Chip | Register | Bits | Function | Values | Evidence |
|---|---|---|---|---|---|
| ESP32-S3 | 0x6001c02c | 31:24 | Forced receive gain index. | | *decompile* `wifi-lib/c/phy_test.c:311-322`; *working* |
| ESP32-S3 | 0x6001c02c | 23 | Force: 1 holds the index, 0 hands the gain to the AGC. | | as above |
| ESP32-S3 | 0x6001c02c | 14:8 | Largest calibrated gain index, written by the PHY when it builds the gain table (capped at 82). Larger reads point at uncalibrated slots; the library treats them as 0. | 0 to 82 | *working* `lib/esp_sdr/src/esp_sdr_rx.c:29-34`; *decompile* `wifi-lib/c/phy_rx_gain.c:297-317`; *upstream* `modules/lib/esp-sdr/main/common/burst_gain.h:1-4, 29-37` |
| ESP32-C6 | 0x600a702c | 31:24, 23, 14:8 | Same layout. | index below 80 (library cap 79) | *working* `lib/esp_sdr/src/esp_sdr_priv.h:110-112`; *decompile* C6 `librftest.a` `phy_test.o` (`force_rx_gain`); *upstream* `modules/lib/esp-sdr/main/common/burst_gain.h:11-12, 36` |

Upstream calls the register AGCPWR_CTRL7. The PHY's `rom_agc_reg_init()` pulses
bit 23 with index 0x32 (*decompile* `wifi-lib/c/phy_reg.c:619-625`). S3 levels
with a 50 ohm load: AGC -25.7, index 0 -46.0, 40 -39.0, 60 -27.0, 82 -19.9 dBFS (*measured*
`.claude/memory/esp-sdr-zephyr.md:108-111`).

- On the S3, `force_rx_gain(force, index, 0)` also calls `bt_rx_force(0)`,
  which clears `0x6001c080` bits 7:6 and steps `0x60006110` bits 9:8 through
  2, 3 and 0. On the C6 it writes only 0x600a702c (*decompile*
  `wifi-lib/c/phy_test.c:240-324`, C6 `phy_test.o`).
- A forced gain on the S3 is occasionally lost after minutes of traffic; this
  register does not show it (the force also involves the registers above). The
  library re-forces after 20 ms (*measured* `doc/hardware-quirks.md:11-33`,
  `lib/esp_sdr/src/esp_sdr_rx.c:56-74`).
- A new forced index alone can leave stale receive state, so a gain change
  reruns the front-end setup (*upstream*
  `modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:107-113`;
  `lib/esp_sdr/src/esp_sdr_rx.c:164-166`).

### Front end: force TX on

ESP32-S3, FE block (`DR_REG_FE_BASE` 0x60006000; the HAL has the base, no
registers).

| Register | Bits | Function | Values | Evidence |
|---|---|---|---|---|
| 0x60006000 | 1 | Force TX on. Without a frame in flight the MAC never enables the transmitter; `force_txon_mode(1, ...)` sets the bit, `(0, ...)` clears it, the vendor tone generator does the same. | without: only a spur near -6 MHz leaks; with: a tone 35 to 37 dB over the TX-off noise | *measured* `.claude/memory/esp-sdr-zephyr.md:128-131`; *working* `lib/esp_sdr/src/esp_sdr_tx.c:84-87`, `lib/esp_sdr/src/esp_sdr_rx.c:84-86`; *decompile* `wifi-lib/c/phy_test.c:200-236`, `wifi-lib/c/phy_feature.c:839-847, 866-869` |
| 0x60006000 | 17:10 | TX gain memory index (inferred). `force_txon_mode(en, mode, ofs)` writes `(ofs + (mode ? 16 : 0) + bits[25:18]) & 0xff`; the tone generator writes `bits[25:18] + 9`. | | *decompile* as above |
| 0x60006000 | 25:18 | Base index read by both. | | *decompile* |
| 0x60006110 | 13:12 | 3 when `force_txon_mode()`'s mode is not 0, else 0. | | *decompile* `wifi-lib/c/phy_test.c:211-218` |
| 0x60006110 | 9:8 | Stepped 0x200, 0x300, 0 by `bt_rx_force(0)` (see AGC force). | | *decompile* `wifi-lib/c/phy_test.c:240-281` |
| 0x60006100 | 18:16 | Unknown; listed by upstream's filter probe. Could not be written from the shell (`devmem`). | | *measured* `.claude/memory/esp-sdr-zephyr.md:419`; *speculative* `modules/lib/esp-sdr/main/diagnostics/filter_probe.h:19-20` |

The library calls `force_txon_mode(1, 0, 0)` and `force_tx_gain()`, which
writes gain memory slot 0. That this is the slot the front end then uses holds
only while bits 25:18 read 0; the code does not check it
(`lib/esp_sdr/src/esp_sdr_priv.h:164-169`).

ESP32-C6, *decompile*, unused (the library has no C6 transmit path):
`force_txon(1)` clears `0x600a70dc` bit 16 and sets `0x600a0910` bit 9, then
bits 11:10; `force_txon(0)` clears them in reverse (C6 `libphy.a` `phy_reg.o`).
With a non-zero mode, `force_txon_mode(en, mode, ofs)` sets `0x600a0910` bits
13:12 and `0x600a28a0` bits 31:30 to 3 when `en` is set and to 0 otherwise.
`set_dump_mode(x)` clears `0x600a0958` bits 1:0 and writes `0x600a70b8` bits
2:0 = 1 when x is 0, else 0 (C6 `librftest.a` `phy_test.o`).

### ESP32-S3 USB PHY select

`RTC_CNTL_USB_CONF_REG`, 0x60008120 (documented, descriptions empty):

| Bits | Name | Function | Evidence |
|---|---|---|---|
| 20 | `RTC_CNTL_SW_HW_USB_PHY_SEL` | 1: software drives the USB PHY mux. | `modules/hal/espressif/components/soc/esp32s3/register/soc/rtc_cntl_reg.h:3071-3083`; mapping in `modules/hal/espressif/components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h:55-66` |
| 19 | `RTC_CNTL_SW_USB_PHY_SEL` | 1: internal PHY to USB OTG. 0: to USB-Serial-JTAG. | same |

Undocumented behaviour (*measured*, bench 2026-10-09): the OTG driver sets
both, they live in the RTC domain and survive the software reset after a DFU
update. An image that does not start OTG then stays off the USB bus (neither
OTG nor USB-Serial-JTAG) until a power cycle, unless it clears both bits early
in boot (`doc/hardware-quirks.md:256-264`). The repository's images start OTG
with `app_usb_init()`; the clearing fix exists only in a scratch tool.

### ESP32-C6 modem reset

`MODEM_SYSCON_MODEM_RST_CONF_REG` (0x600a9810) and `MODEM_LPCON_RST_CONF_REG`
(0x600af024) are documented. Undocumented: the modem subsystem keeps the
previous firmware's state across a chip reset (USB-Serial-JTAG, esptool). The
library writes 0xffffffff then 0 to the first and 0xf then 0 to the second at
`PRE_KERNEL_1`, before the Wi-Fi driver starts, through
`modem_syscon_ll_reset_all()` and `modem_lpcon_ll_reset_all()`, as ESP-IDF's
`esp_system_reset_modules_on_exit()` does on a restart. Evidence: *measured*
bench 2026-10-09 (symptoms under "ESP32-C6 stale modem state"); *working*
`lib/esp_sdr/src/esp_sdr.c:29-48`;
`modules/hal/espressif/components/hal/esp32c6/include/hal/modem_syscon_ll.h:325-329`,
`modules/hal/espressif/components/hal/esp32c6/include/hal/modem_lpcon_ll.h:266-270`,
`modules/hal/espressif/components/esp_system/port/soc/esp32c6/system_internal.c:45-46`.

### Analog I2C master

**ESP32-S3**, base 0x6000e000. The HAL defines only 0x6000e040, bits 17 and
18 of 0x6000e044, and bit 16 of 0x6000e048
(`modules/hal/espressif/components/soc/esp32s3/include/soc/regi2c_defs.h:12-26`).
All *decompile*.

| Address | Bits | Function | Evidence |
|---|---|---|---|
| 0x6000e000 (master 0), 0x6000e004 (master 1) | 7:0 block, 15:8 register, 23:16 data or read result, 24 write, 25 busy, 26 unknown (always set) | One command word per access, then poll bit 25: block, register and bit 26 for a read (0x04000000 base), plus data and bit 24 for a write (0x05000000 base). The result is in bits 23:16. | S3 ROM `rom_chip_i2c_readReg_org` (0x400354fc), `rom_chip_i2c_writeReg`; `wifi-lib/c/phy_i2c.c:260-279` |
| 0x6000e044 (`ANA_CONFIG_REG`) | one read-enable bit per block | Before a read the ROM writes the inverse of the block's bit (clear = readback enabled): 0x61 bit 22, 0x62 11, 0x63 9, 0x64 8, 0x65 16, 0x66 17, 0x67 10, 0x68 15, 0x69 18, 0x6a 13, 0x6b 12, 0x6d 14. | ROM table at 0x3ff193dc (`rom_get_i2c_read_mask`), `rom_chip_i2c_readReg_org` |
| 0x6000e048 (`ANA_CONFIG2_REG`) | 16:4, one master bit per block | Set = master 0, clear = master 1: 0x64 bit 4, 0x63 5, 0x67 6, 0x62 7, 0x6b 8, 0x6a 9, 0x6d 10, 0x68 11, 0x65 13, 0x66 14, 0x61 15, 0x69 16. The ROM's `get_i2c_hostid()` writes the field from 0x1ff40 (0x62 to 0x64 on master 1), libphy's from 0x1fe00 (0x62, 0x63, 0x64, 0x67, 0x6b), on every access. | ROM table at 0x3ff193a8 (`rom_get_i2c_mst0_mask`), `rom_get_i2c_hostid` (0x400354bc); `wifi-lib/c/phy_i2c.c:218-233` |

**ESP32-C6**: `I2C_ANA_MST` at 0x600af800 is in the HAL (I2C0_CTRL 0x600af800,
I2C1_CTRL 0x600af804 with busy in bit 25, ANA_CONF1 0x600af81c as the read
mask, ANA_CONF2 0x600af820 as the master select;
`modules/hal/espressif/components/soc/esp32c6/include/modem/i2c_ana_mst_reg.h:15-45, 105-150`).
The ROM uses the same command word as the S3, including bit 26, which the HAL's
own access code does not set
(`modules/hal/espressif/components/esp_hal_regi2c/esp32c6/regi2c_impl.c:112-180`);
its `get_i2c_hostid()` keeps the bits of ANA_CONF2 outside 17:4 and ORs in 0x1fe00
(*decompile* C6 ROM 0x40003d4a, 0x40003d88).

**ESP32-S3 frequency hop engine** (*speculative* field meanings, *decompile*
flow). The S3 PHY can tune Wi-Fi channels with a hardware I2C sequencer in the
same block instead of a software calibration. `set_channel_rfpll_freq()` takes
this path when `phy_param + 0x120` bit 5 is set, which `get_rf_freq_init()`
does at PHY init after filling an 85-entry table (2400 to 2484 MHz, capacitor
codes interpolated between calibrations at 2400 and 2464 MHz); otherwise it
calls `set_rf_freq_offset()`. So `set_chanfreq()` on a channel frequency
probably loads a table entry without a fresh VCO calibration
(`wifi-lib/s/phy_rfpll.s:762-798`, `wifi-lib/c/phy_hw_freq.c:466-523, 712-751`).

| Address | Bits | Function (speculative) |
|---|---|---|
| 0x6000e0c4 | 7:0 | table memory address (entry x 3 + word for writes) |
| 0x6000e0c4 | 8, 9 | start pulse; memory write strobe |
| 0x6000e0c4 | 19:16, 23:20, 24 | init parameters (2, 4), enable at init |
| 0x6000e0c4 | 25 | 1 = hardware frequency setting off (`phy_dis_hw_set_freq()`) |
| 0x6000e148 | 31:0 | table write data: word 0 = reg 1 + (reg 2 image << 8) of block 0x62, word 1 = the block 0x63 bytes |
| 0x6000e150 | 27:20 | channel index (MHz - 2400) |
| 0x6000e170 | 23:17 | current channel, read back |
| 0x6000e130 | 17 | module reset (active low) |

## Sample word formats

| Word | Chip | Bits 9:0 | Bits 19:10 | Bits 31:20 | Evidence |
|---|---|---|---|---|---|
| receive, in the dump bank | ESP32-S3 | Q | I | unused here; upstream reads 27:20 as gain metadata (unverified) | *measured* `include/esp_sdr/esp_sdr_rx.h:209-225`, `.claude/memory/esp-sdr-zephyr.md:136-138`; *upstream* `modules/lib/esp-sdr/main/common/ring_capture.c:290, 857` |
| receive | ESP32-C6 | Q (assumed) | I (assumed) | | same helpers; not checked against a tone (`.claude/memory/esp-sdr-c6.md:175`) |
| transmit, played by the DAC | ESP32-S3 | I | Q | 0 | *measured* `include/esp_sdr/esp_sdr_tx.h:84-88` |

- Each field is 10-bit two's complement, -512 to 511.
- The receive orientation was found by detuning only the receive LO against a
  known carrier: with I taken from the low field, a tone below the LO showed up
  above it. Upstream's code comments call the low field I
  (`modules/lib/esp-sdr/main/targets/esp32s3/s3_unpack.S:7`, carried into
  `lib/esp_sdr/src/esp_sdr_ring_fir.S:12`), and its docs say the raw ring comes
  out spectrally inverted so hosts conjugate
  (`modules/lib/esp-sdr/docs/iq-stream.md:32-35`): the same finding from the
  other side. The ring keeps upstream's kernels, filters `j conj(z)` and swaps
  the outputs (`lib/esp_sdr/src/esp_sdr_ring.c:222-224`).
- Packed forms: IQ8 is bits 19:12 then 9:2 (I then Q); IQ10 packs bits 19:0 of
  two words into 5 bytes (`lib/esp_sdr/src/esp_sdr_rx.c:548-576`).

## Vendor library functions as used

| Function | Chip | Called as | What it does | Evidence |
|---|---|---|---|---|
| `set_chanfreq(mhz, cbw)` | S3 libphy | `(channel ? mhz : 2412, cbw)`, cbw 0 or 2 | MHz to channel (`rom_mhz2ieee`), then `chip_v7_set_chan(chan, cbw)`: tunes the PLL (hop table or full calibration), passes cbw to `rom_bb_bss_cbw40`, which selects the analog filters. Mode 2 passes +-18 MHz end to end, mode 0 less; no 80 MHz mode. | *working* `lib/esp_sdr/src/esp_sdr.c:109-113, 232-244`; *decompile* `wifi-lib/c/phy_rfpll.c:392-428, 466-474`; *measured* `doc/hardware-quirks.md:56-65` |
| `set_rf_freq_offset(xtal, mhz, khz)` | S3 libphy | `(0, PLL MHz, kHz + offset)` | The first argument is the crystal selector (1: 26, 2: 32, other: 40 MHz); 0 selects 40 MHz, right for the XIAO. Clears `0x62:11[6]`, writes block 0x63, restarts and waits for the calibration, then runs `rfpll_cap_init_cal()`. | *working* `lib/esp_sdr/src/esp_sdr.c:112`, `lib/esp_sdr/src/esp_sdr_priv.h:138`; *decompile* `wifi-lib/c/phy_rfpll.c:62-82, 319-341`; *measured*: the bench SDM values fit 40 MHz |
| `chip_v7_set_chan(mhz, mode)` | C6 libphy | `(channel ? mhz : 2412, cbw)` | Channel setup; converts MHz through mhz2ieee and loses off-grid requests, hence the calibration channel plus `phy_set_freq()`. | *working* `lib/esp_sdr/src/esp_sdr.c:114-120`; *upstream* `modules/lib/esp-sdr/main/targets/esp32c6/chip.h:12-19` |
| `phy_set_freq(mhz, khz)` | C6 libphy | `(PLL MHz, kHz + offset)` | Stores khz at `phy_param + 0x20` and calls `set_rf_freq_offset(phy_param[81], mhz, khz)` (crystal selector from the PHY). | *decompile* C6 `libphy.a` `phy_rfpll.o` |
| `stop_tx_tone`, `rom_pbus_workmode`, `rom_pbus_xpd_tx_off`, `rom_pbus_xpd_rx_on`, `rom_set_rxclk_en`, `rom_pbus_xpd_tx_on` (C6: `ram_stop_tx_tone`, `pbus_workmode`, `rom_pbus_xpd_tx_off`, `ram_pbus_xpd_rx_on`, `set_rxclk_en`) | both | RX: `stop_tx_tone(1)`, `workmode()`, `xpd_tx_off()`, `xpd_rx_on(1)`, `set_rxclk_en(1)`. TX (S3): `workmode()`, `xpd_rx_on(0)`, `xpd_tx_on(1)`. | Forces the analog chain and the receive clock on with the MAC idle, so the engine sees live ADC data (sequence from upstream `prepare_rx()`). Settle 3 ms after a retune; 50 us without one is enough for packet links. | *working* `lib/esp_sdr/src/esp_sdr_rx.c:78-98`, `lib/esp_sdr/src/esp_sdr_tx.c:68-89`; *upstream* `modules/lib/esp-sdr/main/targets/esp32s3/receiver.c:92-106`; *measured* `doc/hardware-quirks.md:162-164` |
| `force_rx_gain(force, index, bt)` | both librftest | `(manual, index, 0)`; index 40 while the AGC is in charge | See "AGC force". | `lib/esp_sdr/src/esp_sdr_rx.c:41-54` |
| `force_tx_gain(gain, bb, dig)` | S3 librftest | ladder step, dig 0 | Turns TX power tracking off, writes TX gain memory slot 0 (`rom_set_tx_gain_mem(0, 1, ...)`) and the digital gain. bb is 0, 0x20, 0x80, 0xa0 or 0x100. The 18-step ladder is the vendor's table in `phy_tx_gain.o` `.rodata` (gain at 0x04, bb at 0x16, power at 0x3a in quarter dB): 46.3 dB range, monotonic, 4.24 units per dB. | *working* `lib/esp_sdr/src/esp_sdr_tx.c:30-66`; *decompile* `wifi-lib/c/wifi.c:2229-2245`, `wifi-lib/o/phy_tx_gain.o`; *measured* `.claude/memory/esp-sdr-zephyr.md:144-150` |
| `force_txon_mode(en, mode, ofs)` | S3 librftest | `(1, 0, 0)` for TX, `(0, 0, 0)` for RX | See "Front end"; ends with `rom_force_txon`. | *working* `lib/esp_sdr/src/esp_sdr_tx.c:84-87`; *decompile* `wifi-lib/c/phy_test.c:200-236` |
| `adctrig()`, `dactrig()` | S3 librftest | not called | Reference for the dump engine (above). | *decompile* `wifi-lib/c/mac_common.c:630-844` |
| `esp_wifi_set_ps()`, `esp_wifi_set_promiscuous()`, `esp_wifi_set_channel()` | both | `WIFI_PS_NONE`; `true`; `(1, WIFI_SECOND_CHAN_NONE)` | The engine and the PHY calls need the Wi-Fi driver up. NULL mode accepts a channel only in promiscuous mode. | *working* `lib/esp_sdr/src/esp_sdr.c:153-166` |

For reading the decompile: the S3 PHY calls ROM helpers through the function
table `g_phyFuns` (ROM image at `.data_phyrom`, 0x3fcef3d8). Slots used above:
0x28 `get_data_sat(value, max, min)`, 0x5c `mhz2ieee`, 0x6c `bb_bss_cbw40`,
0xbc `force_txon`, 0x154 `get_i2c_read_mask`, 0x158 `get_i2c_mst0_mask`,
0x15c `get_i2c_hostid` (libphy's), 0x160 and 0x164 `enter/exit_critical_phy`
(empty), 0x168 `chip_i2c_readReg_org`, 0x16c and 0x18c
`chip_i2c_readReg/writeReg` (libphy's), 0x188 `i2c_readReg(block, host, reg)`,
0x190 `i2c_writeReg(block, host, reg, data)`, 0x194
`i2c_readReg_Mask(block, host, reg, msb, lsb)`, 0x198
`i2c_writeReg_Mask(block, host, reg, msb, lsb, data)`, 0x1d4 `chan_to_freq`,
0x200 and 0x204 `phy_en/dis_hw_set_freq` (libphy's), 0x20c `write_pll_cap`
(libphy's), 0x210 `set_tx_gain_mem`. The C6 libphy uses 0x50 `i2c_readReg`,
0x58 `i2c_writeReg`, 0x5c `i2c_readReg_Mask` and 0x60 `i2c_writeReg_Mask`
with the same arguments. Evidence: *decompile* S3 ROM image,
`wifi-lib/c/phy_init.c:128-165`, C6 `libphy.a` `phy_rfpll.o`.

## Measured PLL behaviour

Bench 2026-10-09, one board of each chip, frequencies are the PLL's (the LO is
PLL / 1.2 behind the divider). Code is the capacitor code (reg 5 plus
256 times `0x62:7[2]`), reg 12 the raw `0x62:12`.

| ESP32-S3 PLL MHz | Code | reg 12 |
|---|---|---|
| 2170 to 2192 | 383 | 0x04 (below the window) |
| 2193 | 383 | 0x00 |
| 2210 | 371 | 0x00 |
| 2300 | 293 | 0x00 |
| 2412 | 208 | 0x00 |
| 2500 | 148 | 0x00 |
| 2600 | 88 | 0x00 |
| 2700 | 34 | 0x02 |
| 2750 | 9 | 0x02 |
| 2780, 2781 | 0 | 0x02 |
| 2782 and up | 0 | 0x0a (above the window) |

| ESP32-C6 PLL MHz | Code | reg 12 |
|---|---|---|
| 2110 to 2138 | 351 | 0x05 (below the window) |
| 2140 | 351 | 0x01 |
| 2412 | 189 (187 in another run) | 0x00 |
| 2413 | 187 | |
| 2437 | 177 | |
| 2600 | 102 | 0x00 |
| 2800 | 29 | 0x02 |
| 2884 | 3 | 0x02 |
| 2888 to 2900 | erratic: 324, 430, 337, 321 | 0x09 |
| 2904 | 0 | 0x02 |
| 2908 and up | 0 | 0x0a (above the window) |

| | ESP32-S3 | ESP32-C6 |
|---|---|---|
| VCO window, `0x62:12[3:2]` = 0 (PLL) | 2193 to 2781 MHz (2780 in another run) | 2140 to 2884 MHz |
| PLL locks on air | 2180 to 2793 MHz | 2128 to at least 2856 MHz (limit of the S3 transmitter) |
| LO without -ERANGE | 1828 to 2781 MHz | 1784 to 2884 MHz |
| LO on air | 1817 to 2793 MHz | 1774 to at least 2856 MHz |

- Below the window the calibration pins the code at 383 (S3) or 351 (C6),
  above it at 0; 430, seen on the C6 in its erratic band, shows that all 9
  bits exist. Repeated calibrations at one frequency land a few codes apart.
- The window sits about 12 MHz inside the lock limits, so -ERANGE errs on the
  safe side (`include/esp_sdr/esp_sdr.h:31-38, 89-104`, `README.md:56-63`).
- The limits are the same with and without the divider, so they are VCO limits,
  per chip and untested over temperature. Nothing locks directly on the S3 in
  200 to 1841 or 2800 to 6000 MHz. Below 2210 MHz the library always uses the
  divider, which reaches the VCO's lower limit / 1.2 (`lib/esp_sdr/src/esp_sdr.c:65-76`;
  `tests/regression/lo_tuning` guards it, `doc/testing.md:143-175`).
- `0x62:12[1]` is set from about 2700 MHz up on both chips, `0x62:12[0]` on the
  C6 below about 2412 MHz; their meaning is unknown.

### ESP32-C6 stale modem state

After the native 802.15.4 radio ran (`apps/wpan`) and the chip was only reset,
not power cycled:

| Item | Clean | Stale |
|---|---|---|
| Receive floor at fixed gain 40 | rms about 2 | rms 40 to 50 (about 25 dB up), at every gain and frequency |
| PLL for 2412 MHz through the vendor channel path | 2412 MHz, `0x63` regs 3 to 6 `08 33 33 01` | about 2404 MHz, `08 11 11 00` |
| `0x62` regs 1 and 5 (code) at 2412 MHz | 0xbb (187) | 0xbf (191) |
| `0x61:4` (`I2C_ULP_OCODE`) | 0x53 | 0x4f |
| `0x61:9` | 0x59 | 0x5a |

A power cycle or a deep sleep clears it, and so does the modem reset at
`PRE_KERNEL_1` (see "ESP32-C6 modem reset"). Evidence: *measured* bench
2026-10-09, `doc/hardware-quirks.md:35-54`.
