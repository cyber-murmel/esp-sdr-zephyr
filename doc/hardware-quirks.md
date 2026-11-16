# ESP32-S3 SDR: hardware quirks and how they are handled

Behaviour of the ESP32-S3 radio, its undocumented capture/DAC engine and the
toolchain that matters when you build on the `esp_sdr` library, found while
developing the apps. Each entry says what happens, how it shows up, and where
the code deals with it. Link measurements: two XIAO ESP32S3 boards through a
30 dB attenuator; C6 entries: one XIAO ESP32C6.

## Radio

### The forced receive gain gets lost

**What happens.** A manual receive gain (`esp_sdr_rx_set_gain()`, forced
through the vendor blob's `force_rx_gain()`) is occasionally lost after
minutes of traffic, on either board. The Wi-Fi AGC then takes over. With
nothing on air it winds the gain up, and the quiet noise floor rises by up
to 30 dB.

**How it shows up.** Carrier sense reads a busy channel all the time. A
sender waiting for an ACK never transmits again. A receiver clips strong
frames and decodes bursts of corrupted headers. It looks like hardware
damage, but re-applying the gain cures it at once. The cause is unknown,
and comparing the AGC force register (`0x6001c02c`) does not detect it:
`force_rx_gain()` writes other registers too.

**How it is handled.** The library re-forces the gain at a capture when the
last force is more than 20 ms old (`esp_sdr_rx_gain_refresh()` in
`lib/esp_sdr/src/esp_sdr_rx.c`, `GAIN_REFRESH_MS`). Each re-force costs 14 to
51 us. `esp_sdr_get_stats()` counts them, and `link status` prints them on
its engine line. A turnaround back to receive also forces the gain. A ring
run re-forces between units: every 20 ms when it sends bursts, every second
when it only receives, since a re-force is longer than the slack of short
units (`apps/chancap`'s 128 us units dropped a few pairs at each one).

When debugging this kind of fault: any `link set` in `apps/link` re-applies
the whole radio configuration, which hides it.

### The C6 keeps the previous firmware's modem state across a chip reset

**What happens.** A chip reset of the ESP32-C6 (through USB-Serial-JTAG, as
esptool and every port open do) leaves its modem subsystem as the previous
firmware left it. ESP-IDF resets it on a software restart
(`esp_system_reset_modules_on_exit()`), a chip reset does not. After
`apps/wpan` had run the C6's native 802.15.4 radio, `esp_sdr` on the next
firmware received with a noise floor about 25 dB up (rms 40 to 50 instead of
2 at fixed gain 40, the same at every gain index and frequency), and the
vendor's channel tuning put the PLL MHz off (2404 instead of 2412 MHz in its
fractional divider). A power cycle or a deep sleep cleared it.

**How it shows up.** Tones from another board land at the wrong offset or
not at all at Wi-Fi channel frequencies, and captures look like a much
noisier receiver. `tests/regression/lo_tuning` failed its link check whenever
it ran after `tests/regression/ieee802154_link`.

**How it is handled.** On the C6 the library resets the modem subsystem at
`PRE_KERNEL_1`, before the Wi-Fi driver brings the radio up, as ESP-IDF's
restart path does (`esp_sdr_modem_reset()` in `lib/esp_sdr/src/esp_sdr.c`).

### 40 MHz mode widens the analog filters

The PHY's channel bandwidth mode (`esp_sdr_set_channel_bw()`, the second
argument of the blob's `set_chanfreq()`) selects the analog filter. In its
40 MHz mode (2) the TX and RX path passes +-18 MHz within +-3 dB end to end:
a 36 MHz OFDM frame decodes error free. The 20 MHz mode (0) cuts it off.
`apps/link` defaults to `CONFIG_APP_CBW=2`. The spectrum edge of a
single-carrier signal is a misleading measure of this, since it mixes the TX
filter, the RX filter and the pulse shaping. Measure the channel per
subcarrier instead (`ofdm rx -v`).

### The transmitter compresses before the DAC words clip

Wide multicarrier signals distort in the analog TX path long before the
10-bit DAC words reach full scale. At an RMS amplitude of 120 DAC units, 116
subcarriers drop to 16 dB MER; at 60 to 90 they reach 26 to 29 dB. The
digital clip counter stays at 0, and lowering the RX gain does not help,
which places it in the transmitter. The OFDM default is `amp 90`.

### Receive gain headroom

Through the 30 dB attenuator at TX step 4, RX gain 32 is the working point
and 44 saturates the receiver (MER 4 dB).

### MER ceilings

| Signal | MER |
|---|---|
| Single carrier QAM, 20 Mbaud, 80 MS/s | about 31.5 dB, with or without attenuation |
| OFDM 16 MHz, DAC at 80 MS/s | about 31 dB |
| OFDM, DAC at 40 MS/s, 16 / 22 / 26 / 30 MHz | 27 / 25 / 24 / 23 dB |

The MER ceiling rules out 256-QAM on OFDM (about 33 dB needed) and 1024-QAM
on a single carrier.

### Carrier and sample clock offset

The two boards differ by about 19 to 21 kHz at 2412 MHz (about 8 ppm), and
the sample clocks by the same ratio. Within a frame the estimate can be off
by a few kHz, so phase tracking must follow a frequency error, not only a
phase. `apps/link/src/ofdm.c` takes the residual offset from the phase step
between its two pilot symbols and tracks it with a second order loop
(`track()`, `RATE_GAIN`). The QAM receiver derives the sample clock offset
from the carrier offset.

### Looped frames are not phase coherent across copies

Phase noise over one loop period makes copies of a looped frame differ in
phase. Decode one contiguous copy (OFDM), or measure the phase step at the
wrap from the overlap of two copies (QAM, `j->r.wrap` in `qam.c`).

### Receiving at 40 MS/s

`ESP_SDR_RATE_40MSPS` captures go through the radio's decimation filter,
with the same MER as 80 MS/s and half the samples to process. The filter
spreads each symbol by about +-8 samples, so an OFDM FFT window that starts
only a quarter of the prefix after its beginning picks up the previous
symbol. `ofdm_rx_begin_period()` measures the window position from the
pilots' phase slope across the subcarriers and moves the window to the
middle of the prefix ("fine timing").

The DAC also runs at 40 MS/s (`esp_sdr_tx_loop_begin(ESP_SDR_RATE_40MSPS)`),
at about 4 dB less MER than 80 MS/s.

## Capture and DAC engine

The capture and DAC playback use the Wi-Fi MAC's undocumented dump engine
(layout from upstream esp-sdr, see `lib/esp_sdr/src/esp_sdr_priv.h`).

- **A bank is off limits while the engine owns it.** The engine writes or
  reads one 64 KiB SRAM bank, selected one-hot in `MAC_DUMP_USAGE`. While it
  does, the CPUs cannot reach any of that bank, so nothing else may live
  there. The linker snippets reserve the banks (see Memory); a link error
  "DRAM image overlaps the ... bank(s)" means the image grew into one.
- **Short captures go unreported.** The engine can stop early without an
  error. `capture_locked()` writes a sentinel into the first and last word
  and fails the capture if either survives. Checking every word cost 1.2 ms
  for a full bank.
- **Capture length.** At most 16380 samples (`ESP_SDR_SAMPLES_MAX`, a 14-bit
  count). A looped frame must fit twice into one window so that a whole copy
  is always present. That caps the bytes per received window: what matters
  is the payload bits per captured sample.
- **Circular capture.** With `DUMP_CTRL` 0x24000 the engine writes a
  16384-pair ring continuously and exposes its write index at 0x60033d60;
  switching the bank select while it runs splits the stream into gapless
  units. `lib/esp_sdr/src/esp_sdr_ring.c` (from upstream) rotates through
  banks 0 to 2 and proves continuity with sentinel windows: the switch must
  come within 2000 pairs (125 us at 16 MS/s) of its threshold, and a bank
  must be filtered before the engine comes round to it again.
- **The C6's gapless ring needs the radio without the Wi-Fi driver.** The C6
  engine takes a whole 128 KiB block, and the rotation needs two (blocks 1
  and 2): a Wi-Fi image fills blocks 0 and 1. `CONFIG_ESP_SDR_PHY_ONLY` brings
  the PHY up through the HAL (`esp_timer_init()`, the Wi-Fi power domain and
  modem clock, `esp_phy_enable()`, `phy_wifi_enable_set()`) in about 70 ms and
  the image fits block 0. Without the driver nothing sets a receive gain (the
  level stays some 20 dB low until one is forced) and its periodic receive
  glitch is gone. `lib/esp_sdr/src/esp_sdr_gapless.c` runs with interrupts
  off; everything on its path must be in IRAM (`ESP_SDR_GAPLESS_HOT`), since a
  flash cache miss of thousands of cycles made switches late. A unit is
  readable for about one unit only, so kept windows are copied raw into a
  staging ring at once (2.5 cycles a sample) and packed to int8 later (9 to
  13); the window readings with their decisions cost 300 to 450 cycles, the
  markers into the next block 2000 per unit.
- **The Wi-Fi MAC interrupt must not run inside the ring.** The ring masks
  interrupts on its CPU and opens short windows for the kernel timer and
  IPIs (`irq_window()`). CPU interrupt line 0 is the Wi-Fi MAC's
  (`ETS_WMAC_INUM`), and it fires for every packet the Wi-Fi receiver decodes
  at the tuned frequency, hundreds of times a second on busy air. Its handler
  is far longer than a window, so the ring abandoned units until the run
  failed: the receive path then lost whole frames, in bursts that came and
  went with the neighbours' traffic and flipped with unrelated code changes.
  `esp_sdr_ring_run()` now disables line 0 on its CPU for the run and serves
  the interrupt afterwards. `ring irq windows` and `lines` in `wpan status`
  show how many windows ran and which lines were pending (0x40 is the tick
  timer; 0x41 would be the MAC). With the fix, abandoned units stay at 0 even
  with the C6 sending back to back.
- **Loop playback wraps only while the trigger is held.** Loop playback
  (`esp_sdr_tx_loop_*()`) holds the trigger bit; pulsing it plays one pass.
  Retriggered bursts (`esp_sdr_tx_play_for()`) leave a hole of about 1.35 us
  each.
- **Turnaround.** The default TX/RX switch with a retune takes about 3 ms.
  `esp_sdr_set_turnaround()` with 50 us and no retune works for packet
  links.
- **Analog I2C must take the PHY's lock.** The vendor PHY serializes its
  analog I2C traffic with `regi2c_enter_critical()`. Every `rom_chip_i2c_*`
  sequence in the library takes it too, because on two cores the Wi-Fi PHY
  code may run one at the same time.

## Memory

On the S3 the engine's 64 KiB banks and the `ESP_SDR_HIGH_RAM` region sit at
the top of internal DRAM, laid out by the linker snippets
`lib/esp_sdr/esp_sdr_bank*.ld` (picked in `lib/esp_sdr/CMakeLists.txt`). Bank 2
(0x3fcd0000) is always the capture bank. `CONFIG_ESP_SDR_BANK1`, which
`ESP_SDR_TX` and `ESP_SDR_RING` select, adds bank 1 (0x3fcc0000) as the
second DAC and capture bank, and the ring takes banks 0 to 2 (from
0x3fcb0000). Each snippet lowers `_heap_sentry` to its lowest bank, so the libc
heap stays below it, and fails the link if the DRAM image grows into a bank
(see "A bank is off limits" above). `ESP_SDR_HIGH_RAM` places buffers from the
capture bank's end up to `DRAM_USER_END` (0x3fce9704).

- **Internal RAM is the scarce resource.** Besides the banks, the region
  above the capture bank (`.esp_sdr_high`, `ESP_SDR_HIGH_RAM`) holds `apps/link`'s
  QAM transmit context and OFDM receive context, with about 128 bytes left.
  "high buffers beyond user DRAM" means it is full. Large buffers that are
  not timing critical go to PSRAM (`EXT_RAM_BSS_ATTR`).
- **Why past `user_dram_end`.** Zephyr's `user_dram_end` (0x3fce4f00) only
  protects MCUboot's loader while the image loads; the ROM's download-mode
  buffers above it are free at runtime, so NOLOAD buffers can use the whole
  range (about 38 KiB).
- **Three banks for the ring** move the DRAM limit to 0x3fcb0000.
  `apps/osmosdr` and `apps/wpan` fit by trimming the network stack and moving
  buffers and stacks into the high region (their READMEs). The Wi-Fi driver
  peaks at about 15.5 KiB of kernel heap with its default receive buffers,
  12 KiB with 2 (`doc/multiapp.md`).
- **Not zeroed at boot.** Neither `.esp_sdr_high` nor PSRAM `.bss` is
  cleared. `apps/link` clears what needs it at init (`link_mac_init()`).
- **PSRAM is slow for strided or per-sample work.** RS units and payload
  bytes may live there. Hot buffers (FFT work buffers, the RS codeword being
  decoded, which goes on the stack) should not.
- **Compare-and-set is not atomic in PSRAM.** S32C1I on external RAM lets
  both CPUs succeed: 2 x 200000 `atomic_inc()` from the two CPUs on one PSRAM
  counter lost counts. A spinlock, `k_msgq`, `k_mem_slab` or `atomic_t` in
  PSRAM breaks under SMP. Espressif's HAL emulates it
  (`esp_cpu_compare_and_set()`), and so does the Zephyr branch `west.yml`
  pins ("soc: espressif: esp32s3: emulate atomics in PSRAM"): every atomic in
  the PSRAM range goes through a lock in internal RAM.
- **No flash access from a thread whose stack is PSRAM.** A flash read or
  write (the flash API, so also MCUboot's `boot_is_img_confirmed()`) parks
  the other CPU and suspends the cache, and PSRAM goes with it: the thread's
  register window spills read back as 0xbad00add, and it double-faults with
  the other CPU still parked. Before the watchdog is armed (at boot, before
  USB starts) the board then hangs for good. A multiapp app's `main()` runs on
  a PSRAM stack, which is why the prelude turns its `app_usb_init()` into a
  call that does nothing.

## ESP32-C5

Measured on one Waveshare ESP32-C5-Zero (ESP32-C5HF4 rev v1.0, 48 MHz
crystal, 4 MB flash, no PSRAM), board `waveshare_esp32c5_zero/esp32c5/hpcore`
(`boards/waveshare/esp32c5_zero/`: console on USB-Serial-JTAG, 4 MB flash
with Espressif's partitions at the C5's 0x2000 boot offset, PSRAM disabled,
simple boot). The receive items were measured without transmitting; the transmit items with a
XIAO ESP32-C6 on the other end of a 30 dB coax attenuator.

- **The capture engine is the C6's with a rate field.** Raw source 15 in
  `0x600a9008` bits 20:17, rate index in bits 23:21, a 17-bit count, lanes in
  `0x600a9018` (as librftest's `adctrig()`). The six indices 0 to 5 give
  80, 40, 20, 10, 8 and 4 MS/s: 16380 samples took 208, 412, 822, 1640, 2050
  and 4098 us, and the count field (`0x600a9008` bits 15:0) advances by 10
  samples per 2.5 us at index 5. `ESP_SDR_RATE_20MSPS` to `_4MSPS` are C5
  only.
- **The dump gate register stalls the bus.** Any access to `0x600a20b4` (the
  C6's dump source gate, in the Bluetooth baseband) takes 1.64 ms and reads
  0. Captures work without it, so the C5 build skips it; with it, every
  capture took 1.7 ms longer.
- **The owner bit takes a whole 128 KiB block.** `SRAM_USAGE` (`0x60095004`
  bits 11:8) = 2 with `MAC_DUMP_ALLOC` (bit 16) puts the samples at
  0x40830000, 64 KiB into block 1, but the CPU loses all of block 1
  (0x40820000 to 0x4083ffff): reads return the last word it read there,
  writes are dropped, nothing faults. Blocks 0 and 2 stay with the CPU.
  Block 2 holds the ROM's data, so block 1 is the only capture bank.
- **The image is split around the bank.** `esp_sdr_bank_c5.ld` is a
  `RAM_SECTIONS` snippet: code and initialized data link below 0x40820000,
  the bank takes block 1, and every section linked after the snippet
  (logging and kernel object data, the rest of `.noinit`, `.bss`, the libc
  heap) moves to block 2. The network stack's and the Wi-Fi driver's
  `.noinit` go below the bank to balance the halves. Margins with the Wi-Fi
  driver's defaults: `apps/capture` leaves 8.3 KiB in block 0 and 21 KiB of
  libc heap in block 2; `apps/rfprobe` 4.9 KiB and 20 KiB. "code and data
  overlap the capture bank" means block 0 is full; "will not fit in region
  `sram0_0_seg`" means block 2 is.
- **Tuning.** `phy_set_chanfreq()` takes any MHz above 14 and runs a full
  channel setup, including 5 GHz. Wi-Fi bursts received with the LO off the
  channel grid (2433, 2444, 5190, 5200 MHz) centre on 2437, 5180 and 5220 MHz
  within one 0.31 MHz bin. The PHY takes its 5 GHz path from 4500 MHz up.
  Below about 1100 MHz the PLL's SDM word goes negative, so those requests
  tune nothing. No PLL lock or VCO window flag is known on the C5, so
  `esp_sdr_set_freq()` never returns -ERANGE there.
- **Receive DC grows with gain, and the AGC idles high.** The largest
  calibrated gain index is 77 at 2412 MHz and 83 at 5500 MHz. With a forced
  index up to 60 the DC stays within about 10 of 512. At 65 to 77 it reaches
  20 to 50, and at 5500 MHz with index 77 it reaches 78. Under the AGC
  (`ESP_SDR_RX_GAIN_AUTO`) it measured +8 to +230 on I at 2412 MHz, varying from boot
  to boot. Force a gain for measurements.
- **The antenna switch.** GPIO26 drives the board's RF switch (200 kOhm
  pull-down, so the chip antenna while the pin floats).
  `CONFIG_WAVESHARE_ESP32C5_ZERO_EXT_ANTENNA` drives it high at boot
  (`board.c`), which selects the U.FL, as `CONFIG_XIAO_ESP32C6_EXT_ANTENNA`
  does on the XIAO C6; `capture` and `rfprobe` set it for the coax bench. At
  gain 60 over 2412 to 2472 MHz, low gave ambient Wi-Fi up to -1 dBFS
  (clipping), high at most -16 dBFS with the U.FL on coax and -30 dBFS with
  it open. The library does not drive the pin.
- **Gapless receive through the modem diagnostic bus.** `ESP_SDR_DIAG`
  (`esp_sdr_diag.h`; the method is ESPsoup's, github.com/pit711/espsoup-firmware, itself after
  ESP-SDR's S31 streaming and Twotoz/C5VRX) streams 40 MS/s without a gap. The pieces, all
  measured:
  - The dump engine runs free: source 15, rate index 0, count `0x1ffff` (the counter is 17 bits
    and counts above 65535 never reach DONE, so it never stops), started with RUN and a trigger
    pulse, then the SRAM usage register is written back so that the CPU owns block 1 again. The
    engine writes nothing into it afterwards, but the diagnostic bus keeps carrying the dump word.
  - `MODEM_DIAG[9:0]` (signal index 106 + lane) is the Q field of the dump word and `[19:10]` the I
    field, as in the capture's words. Eight lanes (a window of 4 bits of each, `lsb` to `lsb + 3`
    and `10 + lsb` to `10 + lsb + 3`) go through the GPIO matrix to eight pads (output enabled
    by `esp_rom_gpio_connect_out_signal()`, input enabled) and straight back into
    `PARL_RX_DATA0..7` (signal index 48). No wire; the pads really toggle at up to 40 MHz, so
    nothing may be connected to GPIO 0, 1, 3, 4, 5, 7, 10 and 23 (ESPsoup's pads; the board
    files here use none of them, the C5-Zero's schematic was not checked).
  - PARLIO RX takes PLL_F240M / 6 = 40 MHz, 8 bits wide, rising edge, LSB first, a soft
    delimiter with EOF at the ring length: every second sample of the 80 MS/s bus, as a Q nibble
    (bits 3:0) and an I nibble (bits 7:4) per byte. The lane order was settled against the 10-bit
    capture: I + jQ gives a tone at +f, as the capture does. No filter before the sampling:
    narrow the low-pass to 20 MHz or less.
  - A GDMA RX channel (AHB, channel 0, trigger 9, owner check off) runs over a circular ring of
    16 descriptors of 3584 bytes (the length field holds 4095 at most) in the lower half of block
    1 (57344 bytes at 0x40820000). The hardware writes the received length back into every
    finished descriptor; the interrupt walks the ring from the last one seen and clears the
    length, so interrupts that merge lose nothing. The last 8 KiB of that half hold the
    descriptors and the consumer's buffers (the `osmosdr_serial` image has 144 bytes of RAM
    margin without the mode).
  - Measured: 40 MB/s for 40 s, 0 skipped blocks, +0.15 ppm against the system timer (a byte
    lost per ring wrap would show as -17 ppm). box16 (a boxcar of 16, SWAR on four words) and
    a third-order CIC cost 4.1 to 4.4 CPU cycles per byte at 240 MHz (budget 6.0); with the USB
    service of `osmosdr_serial`, 5.7.
  - The nibble window needs the level: a signal that exceeds `8 << lsb` counts wraps. At RX
    gain 60 on a hot link the front end clips and every window is useless; from index 48 up the
    C5 clips a tone that sits 10 dB below full scale at index 40.
  - Only the lane word matters (an ablation with a tone from the C6 present): `0x600a9018` =
    lanes 24 to 27 with bit 24 set. Without bit 24, or with other lane numbers, the bus carries
    no I/Q. RUN cleared after the start, source 0 or 3 instead of 15, either clock force
    (`0x600a9c04`, `0x600a0800` bit 2) cleared, or `0x600a901c` zeroed: the I/Q stays. The
    selector pair `0x600a9404`/`0x600a9408` reads 0 on the working path.
  - USB: the decimated samples reach the sink in chunks every 51 us. The USB-Serial-JTAG FIFO
    takes one 64-byte packet per visit; serving it once per 8 KiB ring block (205 us) capped the
    stream near 310 kB/s and lost a quarter of the samples at 250 kS/s.
  `osmosdr_serial` takes the mode with `-DEXTRA_CONF_FILE=diag.conf` (`CONFIG_APP_RX_DIAG`); its
  250 kS/s stream ran 60 s with no index jump, and `tests/regression/stream_gapless` runs both
  receive paths against a C6 transmitter.
- **Transmit.** Two paths, both after the vendor CW turned the chain on and its tone stopped:
  - The TX DC value register (`0x600a0c08`, `esp_sdr_tx_paced_*()`), written by the CPU. A store
    costs 15 CPU cycles, a load 18. The library's poll costs about 177 cycles per sample (its
    loads from `.bss`, code from flash), so it keeps the rate up to 1.35 MS/s; a loop that only
    stores one prepacked word per sample keeps it exactly up to 8 MS/s (ceiling about 8.8). Both
    see stalls of 3 to 36 us now and then even with interrupts locked (cause unknown); pacing on
    absolute targets turns them into jitter, not lost samples.
  - The DAC engine (`0x600a900c`, `esp_sdr_replay_*()`, `ESP_SDR_REPLAY`): plays up to 16383
    words (I 9:0, Q 19:10) out of the dump bank, at 80 MS/s, or 40 MS/s with bit 17 (upstream's
    layout; the C6 uses bit 15 instead). Bits 14 to 16 are no rate field on the C5: with them the
    pass time is not proportional to the count, and some codes never set DONE. A repeat count
    (bits 27:20, with LOOP) plays the passes back to back: 255 passes of 16000 words took
    12240047 cycles against 12240000 for the samples. Repeat count 0 with LOOP plays until RUN
    clears, and its seam is invisible (a C6 capture folded at the loop period shows no step
    above 1 degree where one lost sample would make 31.5). Restarting a finished pass from the
    CPU leaves a hole of 35 to 37 samples at 80 MS/s (about 0.45 us: the poll, and about 0.25 us
    until the engine plays), so periodic content uses the repeat count. The engine owns all of
    block 1 while it plays: no diagnostic-bus ring meanwhile.
  - The level follows the samples times the DAC scale (`0x600a0c04` bits 23:8, one byte per
    branch; the tone stop leaves full scale, the library sets 15 as upstream). The power step of
    the vendor tone does nothing on either path: steps 0 to 15 gave the same level. The chain
    compresses above amplitude times scale of about 1000 to 1500 (third harmonic at -16 dBc with
    400 x 15); at 50 x 15 the image is 32 dB down and the third harmonic 46 dB. Once the chain
    came up 28 dB louder with the same settings, a clean tone, not reproduced since.
  - Half duplex only: while the chain transmits, the receive output freezes at a constant.
    Turning the receive power and clock back on (`phy_pbus_xpd_rx_on()`, `phy_set_rxclk_en()`)
    revives the receiver and stops the transmitter.
  - Host-fed transmit at 250 kS/s starves on the C5 too (USB OUT delivers about 239 kS/s): 125
    kS/s is the reliable rate on both chips. `ESP_SDR_RFTEST` links the C5 librftest for
    `force_rx_gain()`.

## ESP32-S31

The VIEWE board is at the end of this section. Measured on one ESP32-S31-Function-CoreBoard-1 (ESP32-S31 rev v0.0, 40 MHz crystal, 16 MB flash,
16 MB octal PSRAM, PCB antenna), board `esp32s31_function_coreboard/esp32s31/hpcore`, single core
(Zephyr holds CPU1 in reset), simple boot. Receive, a carrier and the CPU-paced transmit. The method is ESPARGOS esp-sdr's S31
firmware at 9b61e7465, where noted otherwise it differs from it. Its PHY offsets belong to
esp-phy-lib b88e4b76e090, the blob this repo fetches. The `lib/esp_sdr` code is `esp_sdr_s31.c` and
`esp_sdr_diag16.c`.

- **`0x20104ddc` bit 4 turns the receiver on.** Without it the ADC gives only its own noise, rms 1
  to 2 counts at any gain, and nothing else looks wrong: the PHY calibrates, the AGC register
  reads back the forced index, bursts have the right length. Upstream never writes it because its
  Wi-Fi driver's start does; without the driver it must be set. librftest's `mac_init()` does, among
  much that the receive path does not need (the last thing it writes, `0x20104c5c`, takes the signal
  away again). Do not call `rftest_init()`: it stops the system timer under Zephyr.
- **Bursts:** the engine stays in its circular mode and a burst stops before it wraps: the finite
  mode writes at TCM offset 0, over the image (`esp_sdr_rx_api` checks the code's CRC around bursts).
  It owns the TCM's whole upper 128 KiB group while it writes, ROM data at the top included: hence
  interrupts off, code and stack below the group, no ROM or PHY calls, up to 4.2 ms at 4 MS/s. No done
  flag: the write index counts. The first 256 samples are the engine settling. Rates: rate field 0, 1,
  3, 5, 7, 9 for 80, 40, 20, 10, 8, 4 MS/s, whose durations match to the microsecond.
- **Tuning:** 2200 to some 2900 MHz; no divided LO, so `-ERANGE` below. A retune rebuilds the gain
  tables at the LO, keeping the IQ correction from startup, whose loopback would transmit. The
  crystal's harmonics land within a few kHz of where they belong from 2280 to 2800 MHz.
- **Gain:** a forced index only reads as itself when its low-table word is mirrored into the high
  table. Quiet-band noise rises monotonically with the index, 22 dB over the table (max 71 here).
- **DC:** `esp_sdr_rx_dc_calibrate()` and the ring's `dc_correct` move the two baseband DC DACs,
  whose effect on I and Q is measured each time, down to about half an 8-bit step. Any gain change,
  retune or ring start undoes it: the ring's phase check passes through gain 0.
- **Ring:** the modem's diagnostic bus carries I[9:2] and Q[9:2], looped through 16 pads into
  PARLIO, into an AXI GDMA ring in TCM. 60 s at each of 40, 20, 10, 5 MS/s: no skips, no gaps, 0 ppm
  against the system timer. The nodes leave a 16-byte hole for a block header, with burst 16 (a frame
  ends mid-ring: its length field holds 65535 bytes, less than the ring, and nothing is lost there).
  Lower rates are every n-th sample, so 10 and 5 MS/s alias under a 13 MHz floor of the low-pass.
- **Pads:** GPIO 20 to 25, 36 to 40, 42 to 44, 47, 48. Upstream's GPIO 33 and 34 instead of 47 and
  48 are USB-Serial-JTAG's, so it would go away while streaming. Not 35, 45, 46 or 49: lines on them
  come out mixed with others, so Q's rms reaches 2.5 times I's whatever the phase.
- **The sampling phase, which upstream does not handle:** PARLIO's clock comes from 160 MHz, the bus
  runs at 80 MHz, so about one start in two samples it on its edge, at every rate and on either
  clock edge. Some lines then take the next sample's bit, Q's bits 5 and 3 above all: under signal Q's
  rms reaches 2.5 to 12 times I's, and a value crossing zero comes out around +-34 and +-42.
  Restarting the divider nearly always lands on the same phase; one period of the divider plus one
  comes 6.25 ns late, half a bus period, which moves it, about every other time.
  `esp_sdr_diag16_run()` looks first with the receiver off (`0x20104ddc` bit 4 clear), where the ADC
  gives its noise alone, under a step rms, whatever is on the air. Only Q's zero crossings show the
  fault, about 85 % of them: at LO 2403, where the DC is near zero, 8160 pairs on a wrong phase show
  1235 to 3289 values of 32 and up, the right one none, at every gain tried. 16 crossings with at most
  2 large values decide; I's do not count. The DC depends on the LO, though (-2.0 on I and +0.85 on Q
  at 2437), and a look with Q on one side of zero proves nothing. Then it looks with the receiver on,
  at gains where Q crosses, in moments with nothing on the air: a Wi-Fi frame hides the wrong values.
  Over 15 LOs from 2300 to 2800 MHz that took 5 of 16 starts: 4 passed, one ended in
  `stats.phase_bad` (`-EIO` with `cfg.phase_required`). On its own,
  as this did at first, the look on air gave up on 3 starts in 24 on a busy channel.
- **HS USB:** the only OTG connector is the USB-A socket J4, VBUS always driven. Zephyr's DWC2 driver works, buffer DMA
  from TCM included. The stream's IN endpoint needs more than the SoC's one packet of TX FIFO: with
  `DIEPTXF1` at 128 words every IN token during a refill gets a NAK, and the link stops at exactly
  16.384 MB/s, 4 packets a microframe. From 256 words it carries 40.157 MB/s, 20 MS/s plus block
  headers, without a gap. DMA burst length and `USBTRDTIM` (0, 5 and 9 tried, the driver's value left
  alone) hardly matter. The FIFOs' power-on depth is 512 words and
  896 words are free in all (`GHWCFG3`). A freshly reset core reads 0 in `DIEPTXFn`: it is in host
  mode until forced.
- **The AGC idles high:** on a quiet band it winds up to `AGC_THRESH_REG`'s index, which the PHY sets to
  the table's top (71 here): noise at 10 8-bit steps rms, -22 dBFS, with the DC 5 to 9 steps off. The
  library caps it at index 60: 2.5 steps rms, -34 dBFS, DC some 2 steps. `AGC_GAIN_MAX` and `AGC_INIT_INDEX`
  change nothing there.
- **A marginal cable looks like a dead PHY:** the host reports high speed, then `-71` at every
  transaction, because the chirp is slow enough to survive what the data is not. The tell is
  `GINTSTS.SOF` (sticky, nothing clears it) staying 0, and full speed working (`DCFG.DEVSPD` 1 and a
  soft reconnect, possible over JTAG). `adj_vref_sq` boots as 8 where the HAL's header says 2:
  lowering it lets a bad cable decode some packets, but a good cable needs no change. Host mode over
  JTAG with a USB stick in J4 tests the chip's path with no cable at all.
- **Transmit, a carrier:** `esp_sdr_tx_tone_start()` runs the vendor's carrier test (librftest's
  `wifiscwout()`) without its Wi-Fi channel: the ROM's `phy_txcal_debuge_mode_()` (the PHY bus in
  debug mode, the TX front end on, a TX gain), then libphy's `phy_start_tx_tone_step_new(1, kHz, att,
  0, 0, 0)` at the LO the library tunes. `phy_txcal_work_mode()` ends it, and the next capture
  receives as before (the floor within 1.5 dB). Attenuation is in quarter dB, from the vendor's least,
  24, to the generator's most, 127. Heard on a XIAO ESP32-S3 a few cm from the VIEWE board: 1.4 dB a
  step of 6, 21 dB from the strongest to the weakest, carriers from 2420 to 2700 MHz, all 21 ppm low
  against that board's crystal. librftest's `fedump_wr_txmem()` may be the S3's replay memory
  under another name, untried.
- **Transmit, samples: the TX DC value register (`0x20100c08`, I 9:0, Q 19:10), paced by the CPU**
  (`esp_sdr_tx_paced_*()`). Measured on the VIEWE board (rfprobe and
  `tests/integration/esp_sdr_tx_paced_api`), heard on a XIAO ESP32-S3 a few cm away
  (`tmp/s31/tx/paced4.py` to `paced6.py` in the workspace):
  - Chain on is the carrier's start at the LO as tuned, then libphy's `phy_stop_tx_tone_new()`:
    both tones' enables off and the DAC's source back to data, which hands the DAC to the TX DC
    value, the chain left on. No retune. The ROM's `phy_stop_tx_tone()` tail-calls
    `phy_dac_scale_set(1)` and is not used. Chain off is the carrier's stop, then receive as at
    startup: at a fixed gain the floor afterwards is within 1.5 dB of before.
  - The level follows the samples only: neither `0x20100c04` (the DAC scale bytes) nor the tone's
    attenuation changes it. Sample I 100 is -21.6 dBFS on the XIAO at gain 10, the same on Q and
    for -100.
  - A store to `0x20100c08` costs 48 CPU cycles at 320 MHz, a load too (TCM 6 and 5, the cycle
    counter 8). A loop writing prepacked words keeps 2 MS/s exactly and 5 MS/s with a few late
    writes; at 8 MS/s a store no longer fits its 40 cycles. The library's poll, at -O2 from IRAM,
    costs about 220 cycles a write at x8: 250 kS/s x4 (320 cycles a write) and 125 kS/s x8 are
    the highest interpolated rates, 250 kS/s x8 falls behind (181k inputs a second).
  - **Keep the timed loop in IRAM.** rfprobe's loops and input callback run from flash and see
    late writes now and then, up to 2255 cycles (7 us) with interrupts locked; the same loop in
    IRAM shows none in 40 s, and the integration test's IRAM stream keeps every rate with a worst
    lateness of 27 cycles. The C5's unexplained stalls above came from flash code as well; that
    they are the same cause is untested.
  - The clock and the sign check out: a 100 kHz tone at 2 MS/s lands 100.08 kHz above the S31's
    own LO line in the same capture (a 300 MHz clock would put it at 93.75), on the + side; I and
    Q need no swap.
  - Spectrum, a 31.25 kHz tone at 8000 of 32767 (dBc): the I/Q image at -f -25 at every rate,
    factor and level (no TX IQ correction acts on this path, so it is the largest spur from x2
    on); input-rate images -17 at x1, -53 to -57 from x4; the DAC-rate hold images (1 MHz at 250
    kS/s x4 and 125 kS/s x8) -30, as a zero-order hold predicts; LO leakage -43 to -54; third
    order -46 to -57.
  - Compression is the S31's: the knee has the same shape at two XIAO gains 10 dB apart. From
    24000 to 32767 the tone grows 0.5 dB and the third-order product at -3f rises from -31 to
    -20 dBc (-41 at 16000). Full scale is 16384 (256 DAC counts); the library maps 32767 to 511.
  - Pacing the TX DC store from the CPU is the only way found: ETM has no task that writes a
    register value.


### VIEWE PLCM-UES31S043H800V480C-U

The same chip on a 4.3 inch 800x480 module (`ues31s043h800v480c_u/esp32s31/hpcore`): CH340C on the USB-C
socket, no USB-Serial-JTAG connector: OpenOCD needs a cable wired to GPIO 33 and 34 on header H1, not tried.
The panel was first read back on the pads, not
looked at: at a pixel clock of 500 kHz the CPU can follow PCLK and compare all 384000 pixels of a pattern.

- **PCLK:** the vendor's `pclk_active_neg` means data changes on PCLK's rising edge and the panel takes it
  on the falling one (`lcd_ck_out_edge` set: PCLK is high in a data cycle's first half): in Zephyr's terms
  `pixelclk-active = <1>`.
- **VSYNC_END comes as the VSYNC pulse starts**, at the frame's first clock and not at its last pixel.
- **An underrun moves the picture for good.** The DMA and the controller only meet at the start: pixels the
  panel missed are still in the DMA's stream, and every later frame shows that much later (IDF's restart only
  exists on the S3). The panel driver restarts both in the next VSYNC event. Less than all of it does not do:
  a DMA and FIFO reset under a running controller, as on the S3, left the picture 1328 pixels off, and a
  stop and start without `lcd_reset` 44 to 48 off.
- **Start the controller first.** In IDF's order (DMA, 1 us, controller) the picture was right with idle
  PSRAM at 200 MHz, and one pixel late with PSRAM at 40 MHz (10 us did not help) or with the CPU in PSRAM at
  200 MHz. With the controller first, the FIFO fills in the first blanking and all of these are exact.
- **The underrun interrupt fires per pixel**, 190000 a second with PSRAM at 40 MHz under load, which took 19 %
  of a copy's speed. It stays off until the restart.
- **PSRAM:** at 200 MHz the CPU reads 81 MB/s through the cache and copies 42.8 MB/s within PSRAM, with the
  panel taking its 50 MB/s and no underrun; at 40 MHz 18.6 and 7.8 MB/s, an underrun in every frame. The
  tuning runs with the cache off, so its DQS scheme must be in RAM (`mspi_timing_by_dqs` in the linker
  script): out of it the boot hangs at "Enter psram timing tuning".
- **Pads for the ring:** GPIO 41 is not bonded. The library's defaults are the RS485 transceiver, the touch
  controller and the ADC keys here, and with GPIO 47 and 48 (the amplifier's enable, the codec's MCLK) Q's top
  two bits come out late, 1.6 % of them wrong and no clean phase found. GPIO 2 to 17, the panel's data lines,
  are clean: `esp_sdr_diag16_api` 11 of 11, 20 MS/s over USB without a gap, which leaves the panel dark.
- **The ring or the panel, not both:** every run resets the whole AXI GDMA group, which takes the panel's channel
  off the LCD, leaves the ring's pads undriven and, on these pads, drives the panel's data lines: the panel gets
  no pixels until a reboot. `esp_sdr_diag16.c` refuses to build with the panel driver. Lines 12 to 15 of the
  ring would also leave on `LCD_DATA_OUT_PAD_OUT0..3`, the panel's blue bits. Not tried.
- **A display's stream:** what is on the panel comes from bursts. The analog filter is all that stands before
  their rate, so the band's image at the input rate's distance arrives at full level. At 8 MS/s with the
  LO 2 MHz off, the line at 2400 MHz showed at 2408 and at 2392 as strong as at 2400 (-45 dBFS, floor -83),
  and LO - 6 MHz is inside the filter's narrowest setting. So: 20 MS/s, the LO 5 MHz below the centre, the
  filter at 13 MHz. The filter, from that line at a fixed gain and 80 MS/s, dB below its level in the pass band:

  | Setting | 6 MHz | 7 | 8 | 9 | 10 | 12 | 14 | 16 | 20 |
  |---|---|---|---|---|---|---|---|---|---|
  | 13 MHz | 2 | 10 | 19 | 23 | 31 | over 40 (the floor) | | | |
  | 15 MHz | 0 | 1 | 5 | 14 | 19 | 31 | over 40 | | |
  | 17 MHz | 0 | 1 | 1 | 0 | 7 | 20 | 26 | 36 | |
  | 21 MHz | 0 | 2 | 0 | 2 | 1 | 8 | 18 | 28 | over 40 |
  | AUTO | 0 | 1 | 1 | 1 | 2 | 8 | 18 | 28 | 31 |

  AUTO, the PHY's own setting, therefore cuts the outer parts of a 40 or 80 MS/s burst. The widest, 54 MHz, keeps
  a line within some 3 dB out to 25 MHz and some 7 dB down at 30 (one burst each, with a few dB of scatter).
- **The panel is in the receiver's band.** The board takes the pixel clock from the audio PLL (APLL) at 25.1 MHz
  (`clock-source = "apll"`), which leaves the APLL's own setting a fraction. Its sigma-delta noise takes the
  clock's multiples down to the floor; what it does past 250 kHz was not measured. dBFS over 250 kHz at a fixed
  gain of 48, the clocks interleaved, two to three rounds each (`tmp/s31/viewe/f22.out` to `f24.out`):

  | Pixel clock | Its multiples (2350, 2375 MHz) | Between them (2337 to 2512 MHz) |
  |---|---|---|
  | PLL_F160M, 25 MHz (160 / 3.2 / 2) | lines, -52 and -55 | -64 to -67 |
  | APLL, exactly 25 MHz (no fraction) | humps of -59 and -61, some +-0.4 MHz wide | -65 to -66 |
  | APLL with a fraction (25.1 MHz, and four others) | the floor, -62 to -65 | 0 to 3 dB higher |

  From PLL_F160M the fractional divider's pattern adds weaker lines every 5 MHz (2355 MHz: -68); from the APLL
  there are none. Under the AGC, PLL_F160M's 2350 MHz line read -37, the APLL's window -58. The lines at 2400
  and 2600 MHz (-46 to -52 and -57) do not follow the pixel clock and drop 10 to 12 dB when the panel's DMA
  stops: likely the PSRAM's 200 MHz under the panel's reads. 2400 MHz is also the crystal's 60th harmonic, -55
  with the panel stopped.

  The panel's 22 pads run at the weakest drive (`drive-strength = <5>` in the board's pinctrl), the picture
  unchanged. Measured from PLL_F160M, three interleaved rounds each:

  | Panel's pads | 2350 MHz | 2375 MHz | 2355 MHz | Floor, gain 48 | Floor, AGC |
  |---|---|---|---|---|---|
  | Default drive (20 mA) | -47 (-28) | -48 (-29) | -60 | -58 to -66 | -46 to -47 |
  | Weakest drive (5 mA) | -52 (-39) | -55 (-46) | -68 | -64 to -67 | -54 to -57 |
  | Controller stopped | under -85 (-79) | under -85 (-80) | | -67 to -69 | -56 to -62 |

  (Lines at gain 48, under the AGC in brackets.) With the DMA stopped and the clocks running (PLL_F160M, default
  drive) the floor came back 0 to 5 dB and the pixel clock's lines stayed. Moving the clock at each tuning would
  add nothing to the APLL's fraction: any move needs one.

  Stopping the controller for every burst, at a frame's start, and restarting it after gave back up to 3.5 dB
  of floor (0 to 3.5 between 2337 and 2687 MHz) and took 2400 and 2600 MHz down 6 to 7 dB, but the panel
  glitched and faded in and out while it ran: not usable.

## CPU and toolchain

- **No hardware float divide.** A float `/` calls `__divsf3`. In hot loops,
  compare ratios by cross multiplication (`sc_scan()` in `ofdm.c`). An
  integer `%` by a constant that is not a power of two is a real division
  too; use a power-of-two size and a mask.
- **`lrintf()` is a library call**, out of line in flash. Two per sample in
  the OFDM transmit path cost about 7.5 ms per frame. Round inline (`emit()`
  in `ofdm.c`).
- **Many accumulators spill.** Loops with four or more running sums get
  them spilled to the stack (about 100 cycles per step for the sliding
  Schmidl-Cox sums). Do less work per step instead: the OFDM search scans
  coarsely first.
- **Contraction is off.** Zephyr's ISO C mode disables fused multiply-adds.
  `apps/link` turns them on for its DSP files (`-ffp-contract=fast`).
- **Hot code belongs in IRAM** (`IRAM_ATTR`): flash code goes through the
  cache, which the other core and PSRAM traffic contend for.
- **esp-dsp's S3 FFT kernel assembles empty without the target macro.**
  `apps/link` builds only `dsps_fft2r_fc32_aes3_.S` from the `esp-dsp` west
  project, with `-DCONFIG_IDF_TARGET_ESP32S3=1` (hal_espressif's
  `sdkconfig.h` lacks it), and its own twiddle table in esp-dsp's layout (cos,
  sin, bit reversed; one table built for N serves every smaller size). From
  flash it is still 1.76 times faster than the same algorithm in C from IRAM
  (85.7 against 150.6 us for 256 points). The S3 mask ROM has the twiddle
  table, but no FFT code.
- **The ring CPU runs with interrupts masked except for short windows.** The
  ring runs on CPU 1 under `arch_irq_lock()`: on SMP, `irq_lock()` takes a
  global lock and would stall the other CPU. Nothing may be pinned to that CPU meanwhile,
  no flash writes may happen (code from flash would stall), and the vector
  registers it uses are not part of any thread context.
- **The ring filter's budget** at 16 MS/s is 15 cycles per pair (240 MHz):
  unpack 3.0, the fs/4 mix 1.6, stage 1 4.5, stage 2 2.6 (decimation 64),
  the app's packing about 1. The multiply-accumulate chain on the single
  `accx` accumulator, not the instruction count, limits stage 1.
- **RS(255,223) decoding cost.** About 49 us for a clean codeword, 220 us
  with 2 errors and 400 us with 10. With many corrections, RS decoding
  dominates a receiver.
- **The ESP32-P4's PIE SIMD** (128-bit vectors q0 .. q7, the 40-bit lane
  accumulators QACC, the scalar XACC) needs Espressif's assembler
  (`-march=..._xespv2p1`, the PIE of the P4 before v3; older binutils called it
  `xesppie`); the Zephyr SDK's knows no PIE. Rules found on the
  WHY2025 badge's P4 (v1.0), all applied in `apps/common/src/esdr_pfbq_pie.S`:
  - PIE and the hardware loops are off until their state CSRs are set
    (0x7f2 and 0x7f1 to 1: illegal instruction otherwise); unaligned vector
    access needs bit 1 of `esp.movx.w.cfg`, and costs 3 to 4 cycles.
  - PIE instructions take x8 .. x15 and x24 .. x31 only, `esp.movx.w.sar`
    included (`t0` is "illegal operands").
  - `esp.lp.setup` with a label needs a relocation only Espressif's linker
    knows (Zephyr's links it as another type, even with `-mno-relax`): write
    it as a word, `0x402b | id << 7 | rs1 << 15 | ((end - .) >> 1) << 20`.
    Binutils 16.2 with `xesploop` resolves the offset itself and emits none.
  - `esp.srcmb.s16.qacc` shifts with floor and saturates only with select 1
    (0 wraps); `esp.cmul.s16` modes 0 and 1 write the low and the high two
    complex lanes (so it works in place), exact sums floored by SAR;
    `vadd`/`vsub`/`vmul` saturate. Loading QACC (`esp.ldqa`, `esp.mov.s16.qacc`)
    gives a rounding addend for free.
  - Nearly every PIE instruction issues in one cycle, a multiply-add with a
    fused load too (the core issues one instruction a cycle, so fusing halves
    the cost); a vector used by the next instruction stalls one.
  - The vector state is in no thread context: one thread per core may use it.
    The enables and the vector, accumulator and loop registers are per core,
    so under SMP that thread is pinned before it starts: moved,
    it would trap where PIE is off, or carry on with the other core's state.
- **The ESP32-S31's PIE is CPU1's only, and version 2.2** (`-march=..._xesploop_xespv2p2`,
  binutils 16.2; ESP-IDF says the same in its `vectors.S`). On CPU0 the state CSR 0x7f2 reads
  0 whatever is written and a PIE instruction is illegal; on CPU1 it reads 0x22000000 plus the
  state. The P4's encodings (2.1) and 3.0's are other instructions there: misaligned stores and
  exception 31. So PIE needs SMP and a thread pinned to CPU1. The hardware loops (0x7f1, their
  state in `mstatus.XS`) work on both cores. On the VIEWE board's S31 (rev v0.0), esp-dsp's P4
  kernels assembled for 2.2 against its C versions (`tmp/s31/viewe/apps/piecheck`, 1024 points):
  the 16-bit dot product exact and 24 times faster; the float FFTs, which use the FPU and the
  hardware loops only, exact on either core and 1.5 times faster (radix 4: 88000 cycles); but
  the 16-bit FFT off by up to 146 counts against a float reference where the C version is off by
  2, and the decimating FIR wrong in every output (it also returns a register it never sets):
  some instruction differs from the P4's, not found yet.
- **The ESP32-P4's branch prediction** (MHCR, CSR 0x7c1: return stack,
  predictive jumps, BTB) is set by ESP-IDF's start code on both cores. In
  Zephyr only the flash cache restore set it, for the core doing the flash
  operation, so the second core could run without it; the SMP port sets it
  on both at start.
- **The ESP32-P4's core-1 boot address and stall survive warm resets** (both
  sit in the always-on domain): after an image that started core 1, the ROM can
  release core 1 into whatever code now occupies that address, and ESP-IDF's
  restart leaves core 1 stalled. MCUboot clears the address, resets core 1 and
  releases the stall first (`soc/espressif/esp32p4/hw_init.c`). On the WHY2025
  badge, with an MCUboot without the first two steps the first reboot of an SMP
  image wedged the chip (USB stayed enumerated, nothing answered); without the
  release, a single-core image hung early after an SMP
  image's reboot, before its USB came up, while a cold boot was fine.
- **The ESP32-P4's memcpy, memmove and memset are ROM routines** (the
  `esp32p4.rom.libc*.ld` scripts) at some 5.5 cycles a byte. Moving the
  channelizer's 1.5 kB history cost 13 cycles a sample until a PIE copy took
  over; copy hot data with words or vectors.
- **The ESP32-P4's float registers are not switched by default.** Zephyr's
  SoC defconfig turns `CONFIG_FPU` on without `CONFIG_FPU_SHARING`, so a
  thread that preempts another in float code leaves its own register values
  behind. `apps/osmosdr_bridge` sets `CONFIG_FPU_SHARING=y`; the lazy switch works
  on the v1.0 silicon (Zephyr decodes the trapping instruction from `mtval`,
  which the P4 fills, so ESP-IDF's FPU reason CSR erratum does not matter).
- **The in-order P4 core waits on float results.** An independent `fmul.s`
  issues every cycle, a dependent `fadd.s` waits 4. Interleave independent
  chains (the detector's decisions: four channels as written-out scalars, a
  per-channel threshold instead of byte flags) and keep loop bounds in locals:
  with `-fno-strict-aliasing` every float store reloads struct fields.

## Building and flashing

- Every S3 app except `apps/capture` needs `west build --sysbuild` (MCUboot,
  USB DFU). A plain build of an existing sysbuild directory fails with a
  cached-source mismatch.
- Kconfig values passed with `-D` stay in the CMake cache. An old override
  (for example `-Dlink_CONFIG_APP_SPS=8`) persists until set again.
- Update over USB DFU with `west dfu -d <build dir> [-s <usb serial>]`.
  The signed image grows in steps of 64 KiB (flash MMU page alignment), so
  its size is no evidence of a new build.
- Host udev rules match USB product IDs: a build with a new PID cannot be
  opened (or DFU-detached) until the rule covers it. The apps share
  2fe3:0005; `west dfu` finds only 2fe3:0005 (app) and 2fe3:ffff (DFU).
- A shell port serves one process: a host tool holding it (such as a running
  `link_perf.py run`) makes another tool's open of the same board fail.
- An S3 image put on by DFU must start USB OTG (`app_usb_init()` in
  `apps/common`, which also arms the trial watchdog) or clear
  `RTC_CNTL_SW_HW_USB_PHY_SEL` and `RTC_CNTL_SW_USB_PHY_SEL` early in boot.
  Otherwise it stays off USB until a power cycle: the OTG driver sets both
  bits and a software reset keeps them.
- esptool reaches an S3 whose app owns OTG once the app's shell command `usj`
  has handed the port to the USB-Serial-JTAG (`apps/common`); its reset at
  the end boots the app, which takes the port back. Hence `west dfu
  --esptool` writes MCUboot, the image and multiapp's volume in one esptool
  run: a sysbuild `west flash` runs esptool once per image, and the first
  one's reset leaves the second without a port.
- Gate an update on the build's exit status (a pipe into `grep` hides it),
  else `west dfu` flashes the previous image.
- The Hackaday Communicator badge has a reset button but no boot button. When
  no app answers `usj` any more, ROM download mode needs test pad TP1 (IO0)
  shorted to ground while pressing reset. A sysbuild `west flash` over its
  USB-Serial-JTAG does work for the first install, before an app owns OTG.
- On the WHY2025 badge the ESP32-C6 (`why2025_badge/esp32c6/hpcore`) reaches
  USB-C port J8 only while the ESP32-P4 drives its GPIO2 high: run a P4 image
  built with `CONFIG_WHY2025_BADGE_USB_TO_C6=y` and without esp-hosted (which
  resets the C6), then flash the C6 over J8 with `west flash`. The C6 also
  drives both backlights: its images hold them on, the P4 cannot.
- The WHY2025 badge's LoRa module is an Ai-Thinker Ra-01SH (SX1262), not the
  Ra-01H (SX1276) its schematic names: an SX1276 driver reads a constant 0xA2
  (the SX126x status byte) and hangs in its image calibration loop. On the
  Ra-01H footprint the Ra-01SH's BUSY, DIO1 and RXEN pads are unconnected and
  pad 5 (C6 GPIO3, the Ra-01H's DIO0) is TXEN. The board's `semtech,sx1262`
  node has neither `busy-gpios` nor `dio1-gpios`: the Zephyr driver waits out
  each command's busy time and polls the IRQ status. Zephyr's `lora/send` and
  `lora/receive` samples run between it and the Communicator in both
  directions (-78 and -87 dBm at a metre, 8 dB SNR).

## Debugging

- **JTAG on an image that owns OTG.** On a running app, `usj` takes the USB
  device off the bus and selects the USB-Serial-JTAG: the PC sees the app go
  and the USB-Serial-JTAG come, and OpenOCD and esptool work until the next
  boot of the app. On a hung one, a shutdown handler (or a `PRE_KERNEL_1` hook
  in a diagnostic build) that clears the two PHY select bits above hands the
  PHY over at the next restart; the PC sees no disconnect then, and a
  `USBDEVFS_RESET` ioctl on the port's `/dev/bus/usb` node makes it
  enumerate.
- **A reset with no crash record** (`multiapp crash`, `link status`) is the TG0
  watchdog's first stage: a CPU wedged with interrupts masked.
- **Stacks.** With `CONFIG_INIT_STACKS`, `kernel thread stacks` after
  exercising every path; a measured peak up to 80 % of the size is enough
  margin. An overflowing shell stack shows as 100 % on the logging stack below
  it. Xtensa frame sizes are in the disassembly as `entry a1, N`.
