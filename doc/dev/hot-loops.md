# Hot loops

<!-- nav-start -->
Previous: [DMA chains for I/Q](dma-chains.md) · Up: [Reference](index.md) · Next: [Ideas from elsewhere](ideas-from-elsewhere.md)
<!-- nav-end -->

This page ranks the places where a classic size-coder or demoscene trick could still speed up a
hot loop of the library or the apps: replay instead of compute, precomputed pointer lists, guard
bands sized to the measured timing, a deeper unroll. It gives each item's location in the tree,
what it costs today, the trick, the expected gain, the risk and the plan.

Nothing here is built or measured. Repo figures cite a file and line or a doc page. Everything
else is marked **(estimate)** or **(check)**. The locations were checked against the tree at the
time of writing.

## Summary

- **The repo already uses most of the classic toolbox.** It has SWAR (box16 on the C5), a
  magic-multiply bit spread (`iq8_low()`, `esp_sdr_gapless.c:561`), saturation tables (`sat_lut`),
  mirrored FIR history so a window never wraps (`txp`, `esp_sdr_paced.c`), sentinel words with
  binary search (`esp_sdr_mark.h`), 128-bit sentinel fills (`fill_sentinels()`,
  `esp_sdr_ring.c:160`), precomputed GFSK phase-step rows per 3-bit history (`gfsk_mod.c`),
  half-sine slot templates for 802.15.4 (`ieee154_dsp.h`) and zero-overhead loops. The gains left
  are in a few places.
- **Three tricks are worth doing:**
  1. **Replay instead of compute.** For GFSK at h = 0.5 (BLE) a symbol's 40 output words are one
     of 32 rows, so TX becomes a table copy: about 6.7 down to 1 to 1.5 cycles a sample
     **(estimate)**.
  2. **Cheaper slot assembly for the 802.15.4 ACK on the S3 ring CPU.** A shift register for the
     chips, a next-template offset table and 128-bit copies: about 85 down to 20 to 25 cycles a
     slot **(estimate)**.
  3. **Guard bands sized to measured lateness.** The sentinel windows allow 2000 (S3) or 400 (C6)
     samples of switch lateness. Measured lateness is 60 to 137 (S3) and about 30 (C6).
- **One more on the S31:** compute paced TX words a block ahead and leave only the store in the
  timed loop.
- **Not worth it:** self-modifying code, executable packers, boot-time generation for flash size,
  packed formats in the hot path.
- **Bytebeat-style sources** are useful as deterministic test transmitters, not as a speed-up.

## Ranked table

| # | Hot loop | Where | Today | Trick | Gain (estimate) | Risk | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | S3 ACK and burst waveform build, CPU 1 between ring polls | `ieee154_dsp_tx_gen()` (`lib/ieee802154/ieee154_dsp.h:109`), `GEN_SLICE` (`esp_sdr_ring.c:1202`) | about 85 cycles a 20-word slot, 25k cycles (105 us) for the ACK's variable part ([IEEE 802.15.4](../modem/ieee802154.md)) | chip shift register, template offset table, 128-bit copy | 3 to 4x on the build: 6 to 8k cycles instead of 25k; frees CPU 1 for box4 at short units | low: bit-exact against today's words | planned next (S3) |
| 2 | GFSK modulator (Ubertooth TX, BLE advertising) | `emit_symbol()` (`apps/ubertooth/src/gfsk_mod.c:317`), `gen_steady()` (`gfsk_mod.c:344`) | 6.7 cycles a sample, a static count; 40 MS/s needs 267 MHz (`gfsk_mod.h:45`) | whole-symbol rows: 8 histories x 4 start phases x 40 words (h = 0.5), copied | 4 to 6x; first block in about 0.1 ms instead of 0.46 | low to medium: 5 KB of RAM per deviation, tight on the S3 | planned next (S3) |
| 3 | C6 gapless markers | `fill_marks()` (`esp_sdr_gapless.c:357`), `prepare()` (`:400`), guards (`:47` to `:49`) | 1536 words a unit, about 2000 cycles, 9 % of the CPU ([Rings](../samples/rings.md)) | size `LATE_LIMIT` and the guards to the measured lateness (about 30 in IRAM) | about 1300 cycles a unit; per-unit cap about +15 to 20 % | low to medium: a late switch becomes a ring error and a restart | deferred: needs the C6 badge |
| 4 | C6 gapless copy into staging | `copy_words()` (`esp_sdr_gapless.c:1039`) | 2.5 cycles a word (3.6 on the badge), unrolled by 8 | unroll by 32, or a computed entry into one unrolled block | 2.5 down to about 2.15 cycles a word, cap +10 to 15 % | low: about 300 B more IRAM | deferred: needs the C6 badge |
| 5 | S3 ring sentinels | `geom_init()` (`esp_sdr_ring.c:74`) | `late` 896 (1024-pair units) up to 2000; 2944 to 7168 sentinel words a unit; measured worst 60 to 137 pairs late | `late` near 256 | about 0.3 to 0.6 cycles a pair of the 15 | low to medium: same failure mode as 3 | planned next (S3) |
| 6 | S31 paced TX | `esp_sdr_tx_paced_poll()` (`esp_sdr_paced.c:434`) | about 220 cycles a write at x8; DAC writes capped at 1 MHz ([Paced](../samples/paced.md)) | interpolate a block ahead into TCM so the timed loop only stores | DAC rate toward the 2 MS/s store-only ceiling; 250 kS/s x 8 becomes possible | medium: one block of latency; needs a fill-in schedule | deferred: needs the S31 |
| 7 | C6 packer, shift 0 and 1 | `pack_run()` (`esp_sdr_gapless.c:571`), `sat_lut` (`:532`) | 9 to 13 cycles a sample, a 2 KB table in block 0 | branchless SWAR clamp instead of the table | same speed; frees 2 KB of block 0 | low | deferred: needs the C6 badge |
| 8 | Ubertooth GFSK demodulator | `apps/ubertooth/src/gfsk_groups.S`, `gfsk_slice.S` | 53 of 60 cycles a sample open ([GFSK](../modem/gfsk.md)) | per-mode kernel variants (static, not self-modifying) | 1 to 2 cycles a sample | costs IRAM the S3 does not have (1.4 KB left); skip | deferred: skip for now, with the board |
| 9 | Paced RX fill (C6, C5) | `esp_sdr_rx_paced_fill_iq8()` (`esp_sdr_paced.c:214`) | 59 of 80 cycles a sample at 2 MS/s, three register reads a sample | read Q once and fix the I/Q skew on the host with a fractional delay | maybe 59 to about 45, so 2.5 to 3+ MS/s | high: the skew must be constant and corrected exactly, or the image rises | deferred: needs the C6 or C5 |

Items 1, 2 and 5 of this table are planned next, on the S3 boards on the desk. Items 3, 4, 6, 7, 8
and 9 are deferred with the board they need.

## 1. ACK and burst slots on the S3 ring CPU

**Today.** The ring (CPU 1) builds the burst waveform between its polls, 400 words a slice (a
comment gives "about 1700 cycles with slot copies", `esp_sdr_ring.c:1201`), about 4.25 cycles a
word. For each slot `ieee154_dsp_tx_gen()` calls `ieee154_dsp_chip()` (a bounds check, a divide by
32 and a variable shift, `ieee154_dsp.h:96`), indexes the template as `tmpl[s & 1][cur][prv]` and
copies 20 words in scalar groups of 4 (`ieee154_dsp.h:122`).

**The trick.** Walk a precomputed pointer list instead of decoding the same thing every frame.

- Keep the chips in a 32-bit shift register: `cur = w >> 31`, `w <<= 1`, reload every 32 slots.
  A final zero word folds "past the end" in, so there is no bounds check.
- The next template address is `base + off[state][next_chip]`, where `state` is the parity and
  the two chips and `off` holds 18 x 2 precomputed byte offsets.
- Copy each 80 B slot with 5 PIE `ee.vld.128.ip` and `ee.vst.128.ip` pairs. The templates and
  bank 2 are 16 B aligned. `apps/ubertooth/src/ubt_pie_copy.S` is the model, but inline it: a call
  per slot costs more than the copy.
- Count: 5 to 6 cycles to load and store a slot, plus 6 to 10 for the state, so **about 15 to 25
  cycles a slot instead of 85 (estimate)**.

**Why it matters.** The build runs on the same CPU as box4, so cutting it by 3 to 4x removes one
of the loads that stop box4 at 1024-pair units. Profile with `esp_sdr_prof` first to see how much
of the unit-boundary overrun is the build (**check**).

**Test.** The words must equal today's, for every `index` and `n` chunking. The unit test of
`ieee154_dsp` (`tests/unit/ieee154_dsp`) decodes frames but compares no golden words, so a
golden-word test comes first: keep today's function as the reference and compare over random
frames and chunkings.

**Status.** Planned next (S3), with the golden-word test first.

## 2. GFSK as replayed symbols

**Today.** `gen_steady()` computes each sample as accumulator plus step row, a LUT lookup and a
store: 5.5 instructions a sample in the unrolled loop plus 46 a symbol (`gfsk_mod.h:45`). That is
fine for bursts but not real time at 40 MS/s on one core.

**The trick.** A symbol's start phase is `D x (sum of earlier bits)` plus whole IF periods. The IF
is 4 MHz at 40 samples a symbol, so its periods cancel.

- At h = 0.5 (BLE) D is a quarter turn: **4 start phases**. With the 3-bit history that is 32
  rows of 40 words, 1280 words, 5 KB. Build them once with the existing `emit_symbol()`.
- The modulator becomes "pick a row, copy 40 words": 10 vector load and store pairs, about 0.5
  cycles a word plus about 15 a symbol: **1 to 1.5 cycles a sample (estimate)**.
- At h near 0.32 (Bluetooth BR, ESB at 1 Mbit/s) rounding h to 1/3 gives 6 phases. The deviation
  error is about 4 % (**check** against nRF24 tolerance).
- Expect 1 LSB differences against today's words, because today's IF accumulator rounds
  differently from an exact quarter-turn grid. Compare EVM with `gfsk_ref.c`, not bit equality.
- **RAM** is the cost on the S3. Keep one table per deviation in use, built at mode start.

**Test.** `tests/unit/ubertooth_tx` (EVM, deviation, chunking) must hold for the replay path.

**Status.** Planned next (S3).

## 3 and 5. Guard bands sized to the beam

Both rings re-mark a window ahead of every unit switch. The window must cover the worst lateness.

- **C6.** `START_GUARD 512`, `END_GUARD 1024`, `LATE_LIMIT 400` (`esp_sdr_gapless.c:47`). The
  source comment says the switch comes within 30 samples with the run in IRAM (`:43`). A
  `LATE_LIMIT` of 96 allows `START_GUARD` 192 and `END_GUARD` 320: the build assertion at `:57`
  needs `END_GUARD > 2 x LATE_LIMIT + 64`, so the 256 an earlier estimate used does not pass.
  That is 512 words a unit instead of 1536, saving about 1300 cycles of the 24640 in a unit
  **(estimate)**. The copy gets about 10000 cycles a unit today (4000 samples x 2.5), so that is
  **about 500 more samples a unit (estimate)**.
- **S3.** `geom_init()` derives `late = MIN(2000, sg - 128)`, 896 at 1024-pair units, and
  `eg = MIN(4096, 2 x late + 128)` (`esp_sdr_ring.c:85`). Measured worst is 60 to 137 pairs
  ([Rings](../samples/rings.md)). With `late` 256 and `eg` 640, sentinels fall from 2944 (1024
  pairs) or 7168 (3072 pairs) words a unit to about 1000 only if `sg` shrinks with `late`. `sg`
  comes from the unit length (`MIN(3072, unit)`), so `geom_init()` has to change as well
  (**check**). At 0.25 to 0.5 cycles a word that saves **about 0.3 to 0.6 cycles a pair**.
- **Risk.** A switch later than the guard makes `find_first()` return `NOT_FOUND`: a ring error
  and a restart, not silent corruption. So shrink in steps and log the lateness first. The stats
  have `late_max` (`esp_sdr_ring.c:1529`) but no histogram. Keep a Kconfig option for the old
  values. Wi-Fi MAC interrupts and flash cache misses were the past causes of lateness; both are
  excluded on these paths.

**Status.** S3 (item 5): planned next, with a lateness histogram and a Kconfig fallback. C6
(item 3): deferred: needs the C6 badge.

## 4. C6 copy: a deeper unroll

`copy_words()` is unrolled by 8 (`esp_sdr_gapless.c:1039`). The rv32imac core has no
post-increment or hardware loops, so each pass pays 2 pointer adds, a count add and a branch: 4
cycles over 8 words, the 0.5 above the 2-cycle load and store floor.

- An unroll of 32 brings that to about 0.13. A computed jump into one 32-word block handles the
  tail in the same code.
- Gain: 2.5 down to about 2.15 cycles a word, **about +15 % on the per-unit cap**, stacking with
  item 3. Together they take the cap from about 4000 to 5000 to 5500 samples a unit, and whole
  bursts from about 50 us to about 65 us **(estimate)**.
- That is small next to the GDMA gather ([DMA chains](dma-chains.md), lever 1), but it costs an
  afternoon and has no hardware unknowns. Do it before the gather.

**Status.** Deferred: needs the C6 badge.

## 6. S31 paced TX: precalculated playback

The timed loop should only store a ready value.

- Today `esp_sdr_tx_paced_poll()` does the input callback, the history update and a 2 x 8-tap FIR
  between stores: about 220 cycles a write on the S31, where the DAC register store alone costs
  48 ([Paced](../samples/paced.md)). A loop that only stores prepacked words reaches 2 MS/s exactly.
- Proposed: fill a block of DAC words (one interpolated input chunk) into TCM, and give the timed
  loop a fill quota per gap. On paper at 2 MS/s there are 160 cycles a write: 48 for the store,
  40 to 60 for an 8-tap complex MAC from TCM **(estimate)** and the rest for the poll.
- That could lift interpolated TX from 250 kS/s x 4 to 250 kS/s x 8, or 500 kS/s x 4.
- On the C6 and C5, paced TX is limited by the host link (USB-Serial-JTAG delivers about
  239 kS/s, [Transports](../samples/transports.md)), not the CPU. There this only helps on-chip
  sources (tones, beacons, precomputed packets).

**Status.** Deferred: needs the S31.

## 7. C6 packer: compute instead of table

The gapless image must fit SRAM block 0, so RAM, not flash, is the C6's scarce resource. The 2 KB
`sat_lut` serves only shifts 0 and 1. A SWAR clamp does the same job: sign-extend both 10-bit
fields with a shift pair, shift right by the gain shift and clamp with `(v + 128) >> 8` tests.
That is about 8 to 10 operations a sample, the same as the table path. Speed stays about the
same and 2 KB of block 0 come free. Do it only if block 0 gets tight again.

**Status.** Deferred: needs the C6 badge.

## 8. Self-modifying code: why not

Patching gain, NCO step or thresholds as immediates into the loop body is the classic 6502 and 68k
trick. It does not fit here.

- **Registers were the reason for it.** Xtensa has 16 visible AR registers plus 8 vector
  registers, and rv32 has 31. The kernels here already keep their constants in registers.
- **The S3 can write IRAM through the DRAM alias**, but it needs an `isync`, and memory
  protection may forbid it. On the C6 and C5, PMP marks text read-execute. Fighting that for 1 to
  2 cycles a group is not worth it.
- **Static specialization gives the same gain:** kernel variants generated by macros and picked by
  a function pointer at mode start. Every variant costs IRAM, which the S3 Ubertooth image does
  not have (1.4 KB and 504 B left, [GFSK](../modem/gfsk.md),
  [Ubertooth internals](../apps/ubertooth-internals.md)). So table item 8 is a skip for now.

## 9. Paced RX fill

`esp_sdr_rx_paced_fill_iq8()` reads three registers a sample. Reading Q once and removing the
fixed I/Q skew on the host with a fractional delay could bring 59 cycles to about 45. The skew
must be constant and corrected exactly or the image rises, so the risk is high. Deferred: needs
the C6 or C5 and a measurement of the skew first.

## Not worth it

| Idea | Why not |
| --- | --- |
| Executable compression (Crinkler or kkrunchy style) | Flash is plentiful, and unpacking to RAM costs the RAM that is scarce. |
| Boot-time table generation for flash size | The tables are already built at boot: `gfsk_lut_init()`, `gfsk_tab_init()`, `ieee154_dsp_tmpl_init()`, `fir_design()`, `sat_lut_init()`. |
| A rotation recurrence instead of `sinf` | Removes libm from an image, which saves flash, not time. Only if an image loses libm otherwise. |
| Packed and delta formats in the hot path | A link-budget question for captures, not a hot-loop trick. |
| Copper-list style sequencing as new work | The right model for REGDMA and an S3 sequencer; the work is in [DMA chains](dma-chains.md). Use (time, write) pairs when writing the list format. |

## Procedural test signals

Bytebeat-style one-line formulas (`t*(t>>5|t>>8)` and relatives) give deterministic, broadband,
cheap test waveforms. The C5 replay (16383 words, played in hardware) and the S31 store-only paced
TX can loop them as known references for `tests/regression/stream_gapless` and the DAC work. They
help because both ends can regenerate them, not because they are fast.

## Suggested order

1. Item 1 (slot assembly), with the golden-word test.
2. Item 5 on the S3, measured with the ring stats.
3. Item 2 when a GFSK burst source needs real-time speed.
4. Items 3 and 4 together on the C6 with the badge: log the lateness histogram, shrink the guards,
   unroll the copy, re-run the badge survey to measure the cap.
5. Item 6 only when someone needs faster S31 TX.

<!-- nav-start -->
Previous: [DMA chains for I/Q](dma-chains.md) · Up: [Reference](index.md) · Next: [Ideas from elsewhere](ideas-from-elsewhere.md)
<!-- nav-end -->
