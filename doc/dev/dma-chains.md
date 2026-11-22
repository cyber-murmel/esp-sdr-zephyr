# DMA chains for I/Q

<!-- nav-start -->
Previous: [ZigBit reference](zigbit.md) · Up: [Reference](index.md) · Next: [Hot loops](hot-loops.md)
<!-- nav-end -->

This page is a roadmap, not a description of working code. It asks whether descriptor lists
(GDMA chains, rings and re-linking, REGDMA) can become one project-wide way of moving I/Q samples.
It says how each chip moves samples today, ranks the four places where a list would replace a CPU
copy, sketches one common layer (`esp_sdr_dma`) and lists the experiments that decide each lever.
It sits in the reference part because it plans work; the guides of Part 2 describe what exists.

Nothing here is built or measured, except where a figure cites a file or a page. Estimates are
marked **(estimate)** and claims not yet checked against the code or a board are marked
**(check)**. Hal paths are relative to `modules/hal/espressif/components/`.

## Answer

- **The radio's own sample mover is not a GDMA.** On every chip the capture and DAC playback use
  the Wi-Fi MAC's dump engine. It writes or plays one whole SRAM bank, takes that bank from the
  CPUs while it runs, and has no descriptors. Lists cannot replace it. They can only work around
  it: before playback, after capture, and on the links to the host.
- **GDMA rings are already in the sample path on two chips.** The C5 diagnostic ring (AHB GDMA, 16
  nodes of 3584 B, `lib/esp_sdr/src/esp_sdr_diag.c:46`) and the S31 diag16 ring (AXI GDMA, 4096 B
  nodes with a header hole, `include/esp_sdr/esp_sdr_diag16.h:48`) share one pattern: a circular
  list, length write-back and an interrupt that walks the list. The C6's SDIO link already sends a
  queue of descriptors over the caller's buffers with no copy
  (`apps/osmosdr_serial/src/link_sdio.c`, SDIO-slave DMA through `sdio_slave_ll_*`, not a GDMA).
  That code is the seed of the common layer.
- **Four places where lists would replace CPU copies** (the levers, below).
- **Where it does not help:** the S3 ring and the S3 gapless TX. Both run on the dump engine's
  banks, and their CPU work (FIR, unpacking, interpolation) is computation, not copying.
- **Order:** S31 re-linking first (lowest risk, known hardware), then the C6 gather (largest
  gain), then S3 slot gathers, then the REGDMA spikes. The S3 slot gather is the part that can
  be tried without another board, so it is planned next.

## Status of the levers

| # | Lever | Chip | Gain (estimate) | Status |
| --- | --- | --- | --- | --- |
| 1 | GDMA gather of kept windows into staging | C6 | whole bursts from about 50 us to the staging limit | deferred: needs the C6 badge |
| 2 | Hand ring nodes to USB by re-linking | S31 | no 32 KiB copy per batch | deferred: needs the S31 board |
| 3 | TX waveforms as a gather of slots | S3 | 25k down to 3 to 6k cycles for the ACK tail | planned next (S3), as experiments 5 and 8 |
| 4 | REGDMA register lists for paced TX and RX | C6, C5, S31 | lifts the C6's 250 kS/s TX ceiling | deferred: needs the C6 (first), then C5 and S31 |

Lever 3 has a decision gate: the gather goes into the tree only if it beats the CPU build of the
ACK tail (see [Hot loops](hot-loops.md), item 1) in CPU cycles. Otherwise the numbers are
documented and the CPU build stays.

## How each chip moves I/Q today

| Chip | Path | What moves the samples | CPU work on each sample | What limits it | Lever |
| --- | --- | --- | --- | --- | --- |
| S3 | Ring, 16 MS/s into 3 banks (`esp_sdr_ring.c`) | Dump engine, circular, banks switched by CPU 1 | FIR or box4 reads the bank in place; chancap's raw sink unpacks to int16 | CPU 1 filter budget, unit overhead | none for the data |
| S3 | TX stream and generator (`esp_sdr_tx_dac.c`) | Dump engine plays bank 0 or 1; CPU flips banks on a cycle (`LOOP_LEAD`, `LOOP_STEP` at line 290) | Interpolation or the generator fills the idle bank | Fill time | none for interpolation; gather for precomputed waveforms |
| S3 | Burst from a ring run (`burst_run()`, `esp_sdr_ring.c:1285`) and the 802.15.4 ACK | Dump engine plays bank 2 | `ieee154_dsp.h` copies 20-word slots: about 85 cycles a slot, 105 us for the part after slot 416 ([IEEE 802.15.4](../modem/ieee802154.md)) | CPU 0 time before the verdict | **3** |
| C6 | Gapless 80 MS/s (`esp_sdr_gapless.c`) | Dump engine, circular over blocks 1 and 2, units of 12288 samples (154 us) | Kept windows copied raw into staging at 2.5 to 3.6 cycles a sample, then packed to int8 (9 to 20 cycles) | About 4000 samples a unit can be copied before the block is reused ([Rings](../samples/rings.md), "What limits a burst") | **1** |
| C6 | Triggered snapshots (`esp_sdr_trig.c`) | Dump engine ring in block 2, stopped on a trigger | Read after the stop, packed | 4 ms dead time | gather into SDIO blocks **(minor)** |
| C6, C5 | Paced RX (`esp_sdr_paced.c`) | CPU reads the IQ estimator sums every sample | 59 of 80 cycles a sample at 2 MS/s ([Paced](../samples/paced.md)) | CPU | **4** |
| C6, C5, S31 | Paced TX | CPU stores the TX DC value register every sample | 15 to 48 cycles a store plus the FIR; the C6 stops at 250 kS/s | CPU; "ETM has no task that writes a register value" (`doc/samples/paced.md:138`) | **4** |
| C5 | Diag ring (`esp_sdr_diag.c`) | Diag bus, PARLIO RX, AHB GDMA ring of 16 x 3584 B | box16 and CIC at 4.1 to 4.4 cycles a byte (budget 6.0) | CPU decimation, USB-Serial-JTAG | already a ring; move it into the common layer |
| C5 | Replay (`esp_sdr_replay.c`) | Dump engine plays block 1, REPEAT and LOOP in hardware | none while it plays | 16383 words; block 1 is lost to the CPU | none |
| S31 | diag16 (`esp_sdr_diag16.c`) | Diag bus, PARLIO RX, AXI GDMA ring, 128 KiB of 4096 B nodes with a header hole (`lib/esp_sdr/Kconfig:237`) | osmosdr_hs `memcpy()`s 8 nodes (32 KiB) per transfer into `in_buf` (`apps/osmosdr_hs/src/main.c:140`) | CPU copy at 40 MB/s; overrun safety | **2** |
| S31 | Dump ring (`esp_sdr_dumpring.c`) | Dump engine over TCM groups; turns park the other core | PIE copies 4 words a load | Turns (up to 21 us) | none (engine) |
| C6 to P4 | SDIO (`link_sdio.c`) | SDIO device-side DMA over a queue of descriptors on the caller's buffers | none: blocks go out as they are | About 13 MB/s within a transfer | already zero-copy |
| P4 | Bridge (`osmosdr_bridge`) | SDIO host, then bulk IN | Blocks pass as they are (a 16-block pool) | C6 | none needed |

## The four levers

### 1. C6 gapless: gather kept windows by GDMA

**Today.** Each unit, the CPU reads the estimator's 10 us windows, decides which to keep (with one
window before and one after) and copies them raw from the block the engine has just left into
staging (`copy_words()`, `esp_sdr_gapless.c:1037`). Each block stays readable for about one unit
(154 us), so whatever the CPU cannot copy in that time is lost. That cap is about 4000 samples a
unit, so bursts arrive whole only up to about 50 us, and no 802.15.4 frame (an ACK lasts 352 us)
arrives whole.

**Staging size.** The staging ring defaults to 48 KiB (`lib/esp_sdr/Kconfig:183`, range 8 to 96),
that is 12288 samples or 154 us at 80 MS/s. The upper halves of blocks 1 and 2 add 2 x 64 KiB once
their canaries hold (`esp_sdr_gapless.c:149`). In all that is 176 KiB, 45056 samples, 563 us at
80 MS/s **(estimate from the Kconfig default)**. That holds a whole ACK (28160 samples). The page
[Rings](../samples/rings.md) still quotes the older 64 KiB ring (205 us) and 196 KB in all.

**With descriptors.**

- At each unit switch the CPU writes one descriptor pair per kept run: source is the run in the
  released block, destination is its staging slot. A 10 us window is 800 words, 3200 bytes, so it
  fits one descriptor (4095 bytes at most). Longer runs split at 4092.
- It starts a memory-to-memory channel and goes back to the estimator. The copy no longer competes
  with the window readings for the CPU.
- The unit's deadline still holds. The chain has to finish within a unit. Completion is the EOF
  interrupt or a poll of the length write-back. A chain that misses it is marked lost, as a late
  copy is today.
- **Gain (estimate).** If memory-to-memory moves 1 word every 2 bus cycles at 80 MHz (**check**:
  the C6 GDMA clock and throughput), a unit's 12288 words copy in about 300 us. That is too slow
  for everything, but 6000 to 8000 words a unit are plausible, 1.5 to 2 times today's cap. With
  the CPU copy beside it the cap could double or triple. A run longer than one unit then goes on
  until staging fills.
- **Packing stays on the CPU.** It works from staging with no deadline, so it limits the average
  kept rate, not the length of a burst. The threshold servo already holds what is kept short of
  the link.

**Risks to check first.**

- Can the GDMA read a block the engine has just released, and does it see the engine's data (the
  CPU does, for one unit)? Ownership is set in `HP_SYSTEM_SRAM_USAGE`. A read of a block the
  engine still owns has to fail safely.
- Bus contention: the engine writes the other block at 320 MB/s while the GDMA moves internal
  SRAM. Markers that prove each unit starts where the last ended would show a disturbed engine.
- Channel count. The C6 has 3 GDMA pairs. SDIO has its own DMA, so one pair should be free
  **(check)** against PARLIO and SPI use in the badge image.
- The C6 hal has `hal/gdma_ll.h` only (`esp_hal_dma/esp32c6/include/hal/gdma_ll.h`). It has no
  `ahb_dma_ll.h`, which the C5, S31 and P4 do have. A C6 backend uses the `gdma_ll` calls.

### 2. S31 osmosdr_hs: hand ring nodes to USB by re-linking

**Today.** The diag16 ring is 128 KiB of 4096 B nodes (32 nodes), each a wire-format block with
the header left as a hole. `batch_send()` copies 8 nodes (32 KiB) into one of 3 `in_buf`s and
submits that to USB (`apps/osmosdr_hs/src/main.c:140`, `IN_FLIGHT_MAX` 3), because the ring never
waits and the GDMA would overwrite a node USB is still reading. At 40 MB/s the copy is the app's
main CPU cost (**check** with `esp_sdr_prof`).

**With re-linking.**

- Keep a pool of spare nodes the size of `in_buf` (3 x 8 nodes = 96 KiB). That is the memory the
  copy uses today, so it needs no extra RAM.
- When a batch of 8 filled nodes is ready, write the header into each hole as now. Then point
  those 8 descriptors' `buffer` fields at 8 spare nodes and submit the original 8 to USB. When the
  transfer completes, the original nodes return to the pool.
- **It is safe without stopping the ring.** The re-pointed descriptors belong to nodes the DMA has
  just finished. It reaches them again only after a wrap, 24 nodes and about 2.4 ms at 40 MB/s
  later. A fence (`barrier_dmem_fence_full()`) before returning the owner bit is enough. The
  owner check is off, so the check that matters is the existing `esp_sdr_diag16_node_valid()`
  margin, done before the swap instead of after the copy.
- **A batch must be contiguous for one USB transfer.** After a swap it no longer is. Either submit
  8 transfers of one node (the DWC2 driver queues them; **check** the per-transfer cost at high
  speed), or give the pool whole 32 KiB slabs and swap 8 contiguous nodes at a time.
- Zephyr's DWC2 driver already does buffer DMA from TCM ([Transports](../samples/transports.md)),
  so USB reads the nodes in place.
- The same swap gives the C5 diag ring a zero-copy path for a raw dump into PSRAM or RAM. Its USB
  is full speed only, so that would be bursts, not a stream.

### 3. S3: TX waveforms as a gather of precomputed slots

**Today.** The 802.15.4 ACK is built from 18 half-sine slot templates of 20 words
(`IEEE154_DSP_SLOT`, `lib/ieee802154/ieee154_dsp.h:29`), copied by the CPU into the burst bank by
`ieee154_dsp_tx_gen()` (`ieee154_dsp.h:109`): about 85 cycles a slot, 105 us for the part that
depends on the sequence number and FCS, 251 us for a whole ACK ([IEEE 802.15.4](../modem/ieee802154.md)).

**With descriptors.**

- Precompute the slot table once in internal RAM. Each symbol becomes one descriptor pointing into
  the table. The other side of the memory-to-memory pair is one chain over bank 2 (4092 B each).
- The CPU writes 12 bytes a slot instead of copying 80. For the variable part (289 slots) that is
  about 3.5 KB of descriptors and perhaps 3 to 6k cycles instead of 25k **(estimate)**. The copy
  runs in the background.
- **The fixed head stays fixed.** A per-PAN head lives in bank 2 for the whole run, as now
  (`doc/modem/ieee802154.md`, "Precomputed waveform"). Only the tail is gathered.
- The same gather could build a whole frame (802.15.4, BLE advertising, later ESB) without
  running `gfsk_mod`.

**Limits.**

- The S3's GDMA reaches bank 2 only while the CPUs can, so not during playback. The late CRC
  decision of a shockburst-style ACK would still need the tail-bank flip or the cut.
- S3 memory-to-memory throughput and descriptor fetch cost are unmeasured. With 20-word slots the
  per-descriptor overhead could dominate. Experiment 5 measures it at descriptor sizes from 80 B
  to 4092 B.
- The S3 has 5 GDMA channel pairs, shared with SPI, LCD_CAM and I2S in apps that use them
  **(check)**. USB OTG has its own DMA.
- The S3 hal's `gdma_ll.h` has a memory-to-memory switch (`mem_trans_en`,
  `esp_hal_dma/esp32s3/include/hal/gdma_ll.h:376`). The repo does not use it yet.

### 4. REGDMA: register lists for paced TX and RX

**Why REGDMA.** Paced TX writes the TX DC value register once a sample and paced RX reads the IQ
estimator's sums once a sample. [Paced](../samples/paced.md) says ETM has no task that writes a
register value, and a memory-to-memory GDMA to a peripheral address is untested: ESP-IDF's async
memcpy allows only RAM, and PMS/APM may block it (experiment 8). REGDMA (the PAU, present on C5,
C6, P4 and S31, not on the S3) walks linked lists of register writes and backups in RAM. That is
the paced path's job.

**What the hal confirms** (read from the sources, nothing run):

| Claim | Where | Result |
| --- | --- | --- |
| Node types | `soc/include/soc/regdma.h:119` | continuous and address-map (backup and restore ranges), write (value and mask), wait (condition), plus branch variants; a 10-bit word count and an end-of-list flag in the head |
| Link format, builders | `esp_hw_support/port/regdma_link.c:30` ff. | `regdma_link_new_*()` allocate the nodes; a write node holds its value inline, so a sample stream needs one node per sample **(check)** |
| ETM start tasks | `soc/<chip>/include/soc/soc_etm_source.h` | `REGDMA_TASK_START0..3`: C6 128 to 131, C5 127 to 130, P4 161 to 164, S31 297 to 300; done events `REGDMA_EVT_DONE0..3` |
| ETM tasks for GDMA | same files | C6 `GDMA_TASK_IN_START_CH0..2` 159 to 161 and `OUT_START` 162 to 164; C5 143 to 148; not found by that name on S31 and P4 **(check)** |
| Read interval | `soc/esp32c6/register/soc/pau_reg.h:212` | `PAU_READ_INTERVAL`, bits 6:0, default 32, in `PAU_REGDMA_BKP_CONF_REG`; only the C6 file was read |

Everything else the earlier desk notes claimed about REGDMA stays unverified: that the interval
paces the nodes, that a list can loop back to its start, the write rate (perhaps 1 to 5 M writes a
second **(estimate)**) and the node size (the head and four words, about 20 B, **(check)**). The
note behind those claims is not in the repo.

**TX.** Pack the FIR output as a list of write nodes to the TX DC value register, paced by the
read interval or restarted per block by a GPTimer through ETM. That would take the C6 from 250
kS/s (CPU-paced, interrupts locked) to whatever REGDMA sustains. At about 20 B a node a 32 KiB list
holds about 1600 samples, so the CPU refills one list while REGDMA plays the other.

**RX.** Backup nodes copy the estimator's sum registers into RAM, one node per sample. The CPU
takes the differences later in bulk, without polling.

**Unknowns (the spike decides).**

- Pacing jitter: a boxcar sample that lands late shifts its window.
- Whether REGDMA's bus access reaches the modem's registers at all.
- Whether the PMU owns REGDMA in a PHY-only image. The regdma links of the PHY and the modem
  already exist for sleep retention (`REGDMA_PHY_LINK`, `REGDMA_MODEM_FE_LINK` in `regdma.h`).

## The common layer

One small library, `lib/esp_sdr/src/esp_sdr_dma.c` with `include/esp_sdr/esp_sdr_dma.h`, over the
hal's LL calls (`ahb_dma_ll.h`, `axi_dma_ll.h`, and `gdma_ll.h` for the S3 and C6), in IRAM, with
no interrupts required on the paced paths. Only the S3 backend is planned next.

| Piece | What it does | Replaces |
| --- | --- | --- |
| `esdr_dma_ring` | A circular descriptor list over N nodes with an optional header hole. A completion walk from the last node seen uses the length write-back, so merged interrupts lose nothing. Gives a produced count and a node-valid check | The duplicate ring code in `esp_sdr_diag.c` and `esp_sdr_diag16.c` |
| `esdr_dma_ring_swap(ring, node, spare)` | Re-points a finished node's descriptor at a spare buffer and returns the old one, with the margin check and a fence | `memcpy()` in osmosdr_hs `batch_send()` |
| `esdr_dma_gather(ch, dst, dst_len, src[], len[], n)` | Memory-to-memory: a chain over scattered sources and one over a contiguous destination. Returns when started; `esdr_dma_wait()` or EOF | The C6 staging copy, the S3 slot copies |
| `esdr_dma_chain` | A queue of descriptors over caller buffers, sent as one chain when idle and retired on EOF | The send queue of `link_sdio.c` (keep its LL calls, share the queue logic) |
| `esdr_regdma_list` (experimental) | Builds write and backup nodes for one register, at a pace | The paced TX and RX loops, if the spike works |
| Backends | S3 and C6 `gdma_ll`; C5 AHB GDMA; S31 and P4 AXI GDMA (and AHB GDMA for peripherals on the AHB) | |

Rules it enforces:

- Descriptors and buffers live outside any bank or group the dump engine can own. The linker
  snippets `esp_sdr_bank_*.ld` already check images; the descriptor sections join that check.
- No GDMA touches a bank while the engine owns it. The layer refuses a source or destination
  inside an owned bank, as the CPU code already avoids it.
- One channel claim per app. The S31's "every run resets the whole AXI GDMA group" (it kills the
  panel) means the layer resets only its own channel, never the group.

## Starting and stopping transfers

The movers run on their own once started. What has to be on time is the start and the stop.

- **C5, C6, P4, S31:** ETM can start or stop GDMA channels and REGDMA lists from timer and GPIO
  events with no CPU. The task ids are in `soc_etm_source.h` per chip (table above).
- **S3:** no ETM and no REGDMA. CPU 1 already starts transfers on a cycle with interrupts locked
  (`wait_until_locked()` in `esp_sdr_tx_dac.c`, `burst_run()` in `esp_sdr_ring.c`). A shared
  `esdr_seq` with "at cycle t, write register r" entries is the only sequencer the S3 needs.
- Branching on a CRC or waiting on ring positions stays in the apps that need it until a second
  user appears.

## Experiments, in order

| # | Experiment | Chip, board | Done when | Status |
| --- | --- | --- | --- | --- |
| 1 | `esdr_dma_ring` extracted from diag16 and diag, no behaviour change | S31, C5 | `tests/regression/stream_gapless` and osmosdr_hs 300 s at 20 MS/s unchanged | deferred: needs S31 and C5 |
| 2 | osmosdr_hs with `esdr_dma_ring_swap()` instead of `memcpy()` (slabs of 8 nodes) | S31 | 40.157 MB/s for 300 s with no bad block; CPU time per second down by the copy's share (`esp_sdr_prof`) | deferred: needs the S31 |
| 3 | C6 GDMA memory-to-memory: SRAM to SRAM throughput, and reading a block the engine just released while it writes the other | C6 (badge) | MB/s; data matches a CPU copy; markers unchanged | deferred: needs the C6 badge |
| 4 | C6 gapless with the gather beside the CPU copy | C6 and P4 | longest whole burst over 50 us; an 802.15.4 ACK decoded whole on the P4 | deferred: needs the C6 badge and P4 |
| 5 | S3 GDMA gather of the 802.15.4 ACK tail into bank 2 | S3 | bit-identical bank to the CPU's; CPU 0 cycles for the tail under 10k; ACK late count not worse | planned next (S3) |
| 6 | REGDMA spike: one write node to a GPIO register, then to the TX DC value register, looped, paced by the read interval and by ETM | C6 | write rate and jitter on a logic analyser or in a capture; a tone heard | deferred: needs the C6 |
| 7 | REGDMA backup of the estimator sums | C6 | samples equal to the CPU-paced ones at 250 kS/s | deferred: needs the C6 |
| 8 | GDMA memory-to-memory into a peripheral register | C6, S3 | writes land, or a PMS/APM fault is recorded | S3 part planned next; C6 part deferred: needs the C6 |

The badge and the P4 board are not in `tests/regression/hardware-map.yaml` (it lists the XIAO C6, the C5-Zero, the S31 board and the two S3 boards), so experiments 3 and 4 also need a map entry.

## Open questions

- Whether the S3 or the C6 modem has a diagnostic bus the repo has not found. With one, PARLIO
  (C6) or LCD_CAM (S3) plus a GDMA ring would give them the C5's or S31's gapless path.
- GDMA throughput, memory-to-memory, on each chip.
- REGDMA's pacing and its reach into the modem registers.
- The C6 dump engine's "four-slot mode", mentioned under "Chasing the engine" in
  [Rings](../samples/rings.md). If it takes several bank addresses, it is the closest the engine
  comes to a descriptor list.

<!-- nav-start -->
Previous: [ZigBit reference](zigbit.md) · Up: [Reference](index.md) · Next: [Hot loops](hot-loops.md)
<!-- nav-end -->
