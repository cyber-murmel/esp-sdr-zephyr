# Transmit stream library (lib/esp_sdr/src/txs)

<!-- nav-start -->
Previous: [S31 to host at 80 MS/s through a Colorlight 5A-75B](s31-fpga-bridge.md) · Up: [Reference](index.md) · Next: [Network services on the S31 multiapp host](network-services.md)
<!-- nav-end -->

Stage 1 of the continuous I/Q transmit stream (design: the S31 transmit stream plan): the parts that
need no hardware. Plain C, no Zephyr kernel calls, tested on native_sim (`tests/unit/txs`). Stage 3
(`esp_sdr_dacstream`, a CPU1 thread and the ownership writes) calls into it.

| Header | Contents |
|---|---|
| `txs_pos.h` | Position model: the 32 bit cycle counter to the 64 bit index of the word the DAC plays now. cpw 4, 8, 16, 24. Needs one read per 2^32 cycles. |
| `txs_flip.h` | Flip scheduler: two groups of 32768 words, the free window `[next, next + 32768)`, `txs_flip_poll()` returns FILL, ZERO, IDLE, FLIP or OFF; point of no return, late flip accounting, stop NOW and DRAIN, hang passes. |
| `txs_jit.h` | Jitter cursor: packets by absolute index into the free window, gap zeroing, holes refilled by reordered packets, duplicates, late, waiting, far; close at the point of no return (underrun); restart index. |
| `txs_credit.h` | Credit limit and the 64 byte status record. |
| `txs_wire.h` | VITA 49.2 signal data, TSI none, TSF sample count, 16 byte header; parser and header writer. |
| `txs_expand.h` | Kernel table (`ref`, `fast`, later `pie`): 8+8 to DAC words, ready words, zero fill. |

Loop of the CPU1 thread, as the end to end test (`tests/unit/txs/src/test_sim.c`) drives it:

```
txs_flip_init(&f, &cfg, first)   returns -1 for a margin below lock_lead + chunk * zcost + overhead
loop:
  pos = txs_pos_update(&p, counter)
  op  = txs_flip_poll(&f, &p, j.cursor)
  FILL: take the first queued packet that is not waiting (idx, n); cw = min(n, chunk);
        end = txs_flip_fit(&f, &p, j.cursor, idx, idx + cw)   (a reordered packet filling a hole
        costs time and saves no zeroing: fit knows; end <= idx: nothing fits, poll again);
        txs_jit_packet(&j, idx, end - idx, &place); zero place.zero, expand place.put with the
        kernels. Whatever the verdict, `used` words are dealt with: advance the packet by used.
        A packet left with words (a tail past the window's end, also after LATE and DUP, or WAIT)
        waits for the next flip.
  ZERO: txs_jit_close(&j, f.mode == TXS_DRAIN, &span); zero span
  FLIP: lock, spin to txs_flip_aim(&f, &p) (the due time less write_lat), write the ownership
        (the DAC takes group_of(f.next)), unlock; then
        txs_pos_update(&p, counter); play = txs_pos_word_after(&p);
        txs_flip_done(&f, play, j.cursor, j.win_data);
        if f.dac_group is not the group written (a flip missed by groups), write f.dac_group;
        txs_jit_flip(&j, &f); let the waiting packets try again
  OFF:  engine and chain off
```

Rules the tests pin down:

- **Late and early flips.** A flip late by L words plays L words of the old content; the window is
  still the right one afterwards. A flip early by E words does the same for E words. All counted in
  `stale_words`. The spin to the due time ends 1 to 13 cycles late and the write takes 40 to 51
  cycles: aim at `due - write_lat` (`txs_flip_aim()`, write_lat the least the write takes). A flip
  counts as late (`late_flips`) only beyond `tol` words; within it, `slack_flips`. The model has
  no better knowledge than its own clock: where the engine's group switch lands within a word
  is not visible to the CPU.
- **Missed flips.** A flip missed by whole groups (a hung thread) plays groups * 32768 stale words:
  the old window's group is replayed, and so is the group of the window playing at `play`, which
  was never refilled. The window moves on by `groups`. The ownership to write is `f.dac_group`, the
  group of the window playing at `play`: not the group of the old `next` (they differ whenever
  `groups` is even). During DRAIN a missed flip ends in OFF (`TXS_STOP_MISSED`): the zero window
  was never played.
- **Point of no return.** `rest * zcost + margin` cycles before the flip. At it the rest of the
  window is zeroed, so the timeline never shifts. A write [lo, n) is allowed by
  `txs_flip_may_write()` (the largest by `txs_flip_fit()`) when the time to the due time covers its
  words at `cost`, a gap before it at `zcost`, the zeroing of what remains and the margin. Words
  below the cursor (a hole filled by a reordered packet) cost the same and shorten nothing. An
  empty write, or one outside the window, is always fine, so a packet that only waits or is late
  never blocks the queue.
- **Margin rule.** `margin >= lock_lead + chunk * zcost + overhead`, `write_lat <= lock_lead`,
  `cost > zcost`; `txs_flip_init()` returns -1 otherwise.
- **Underruns.** A window closed short, or handed over short after data was seen, is an underrun
  (`txs_jit.underruns`): the sender resyncs on that count.
- **Credit.** The limit is `max(win_end, contig, play) + depth`, `win_end` the end of the open window
  (`txs_jit.hi`: the group being filled while the other plays). Words inside the open window are
  written the moment they arrive, so only what lies beyond its end waits in buffers, and the limit
  does not follow the fill: it rises by G at each flip. Before the first data word the limit is the
  window's end alone (the host fills the first window first); with no window published
  (`win_end` 0) it falls back to `contig + depth`. One rule: depth is the buffers' capacity in words
  of the session's smallest packet, less two packets (one split by a window's end, one in progress),
  `(TX_BUFFERS - 2) x max_words` = 13338 words (19 packets of 702; w32 6669; it was 7020 with the pool of 34
  buffers). The status goes at
  every flip and at least every 250 us. The queue peaks at depth / packet + 2 buffers because the
  fill is faster than the arrival (a sender capped at 900 Mb/s sends a packet in 13.4 us, the fill
  takes 8.6). `txs_jit_init()` takes `ahead` and `depth` and returns -1 unless `ahead >= depth`: a
  sender that keeps to its credit never starts a packet past `hi + depth`, so packets beyond
  `hi + ahead` are a rogue sender's and are dropped as FAR. Simulated (`test_round_trip`,
  `test_sim.c`): 300 and 600 us round trip, credit updates every 250 us and at each flip, 20 and 40
  MS/s: no loss, late word or underrun with 12 buffers except 600 us at 40 MS/s, which needs the
  depth `rate x (RTT + 120 us)` (45 buffers, not available).
- An 8+8 packet carries an even number of samples (the payload is whole 32 bit words).
- The payload of a frame sits at 2 mod 4 (Ethernet 14, IPv4 20 or IPv6 40, UDP 8, VITA 16); the
  kernels take 0 and 2 mod 4 and read nothing past the payload (tested under the address
  sanitizer, `unit.esp_sdr.txs.asan`).
- Words played after a late OFF (an engine switched off after the end of the DRAIN window) are not
  counted by the scheduler.

## Threading

The scheduler, the jitter cursor and the position model belong to the CPU1 thread; the position
model reads that core's cycle counter (`mcycle` is per core), so it is updated there and nowhere
else. For the other core (CPU0):

- A stop: `txs_flip_request(&f, TXS_STOP_DRAIN or TXS_STOP_NOW)` (one atomic word, NOW wins); the
  CPU1 thread takes it in `txs_flip_poll()`. Never call `txs_flip_stop()` from another core.
- The status: the CPU1 thread builds the record (`txs_status_make()` reads the live state and the
  play index) and publishes it with `txs_snap_write()`; CPU0 reads with `txs_snap_read()` (retry on
  false) and sends it. A 64 bit load is two loads on RV32: a plain read of the live state tears,
  the sequence lock does not. Counters for a shell command are read the same way.

## Placement

The hot functions carry `ESP_SDR_CODE(PACED_CHUNK, ...)` (scheduler, cursor) and
`ESP_SDR_CODE(PACED_SAMPLE, ...)` (kernels), the nearest existing categories. Everything of this
module that the CPU1 thread calls is PACED_CHUNK or PACED_SAMPLE now, the wire parser, the status
record, the snapshot, `txs_flip_aim()` and `txs_flip_stop()` too (they ran from flash in the first
board runs). What it calls outside the module: `net_buf_unref()` (the thread gives each
packet back) is Zephyr's, and with `apps/txstream_eth/ram_netbuf.ld` it, `net_buf_destroy()`, the fixed
allocator's `fixed_data_unref()` and its const callback tables run from IRAM (230 B of group 0), not from flash.

## Cycle notes (RV32, no 64 bit hardware)

The first board run spent about 1700 cycles on a 128 word step besides the kernel. The hot path no
longer multiplies or shifts 64 bit values:

- `txs_pos_update()`: the remainder sum and the shift in 32 bits (one carry), the 64 bit add of the
  index only.
- `txs_pos_cycles_to()`: a 32 bit multiply when the distance is below 2^26 words, else the old
  64 bit one.
- `txs_flip_may_write()`: offsets from the window's start in 32 bit (products of at most 32768 words
  and 32767 cycles); the old 64 bit version stays as the path for a fill index outside the window
  or costs above 32767.
- `txs_flip_fit()`: one `may_write` test for the whole range, the binary search only when it does not
  fit (the usual case is one test instead of 7 to 10).
- `txs_flip_poll()`: a plain load of the request word, the atomic exchange only when set.

The unit tests (`tests/unit/txs`, native_sim/native/64, also under the address sanitizer) pin the
results to those of the 64 bit code. `tests/unit/txs/src/test_sim.c` also has the window dynamics of
the real configuration (`test_real_config_*`): a sender in step with the clock needs 3000 words
(150 us) beyond a group of lead, the credit depth is the stall it rides out (12 buffers of 702 words:
300 us, 19: 500 us, 34: 1100 us), and a status every 175 us (depth / rate / 2) costs the sender
nothing, one every 351 us blocks it on the credit hundreds of times as often. Proposal for stage 3:
a `TXS_CHUNK` and a `TXS_SAMPLE` category in `esp_sdr_place.h` and the chip headers (IRAM for the
CPU1 thread, no flash fetch in the window). The kernel tables (const, read once per packet) stay in
flash.

## Board findings and what the app does about them (txstream_eth)

- **The ownership write and the `stale_words` of one word per flip.** The statistic is an artefact
  of the model, not words the DAC played twice. The flip records `p.last = tw + write_lat` (the
  counter at the write plus the latency) and aims at `due - write_lat`: the two cancel, so what is
  left of a flip's lateness is the spin's granularity (0 to 15 cycles, less than a word at 20 MS/s
  with 16 cycles a word), which the account shows as one word. The CPU can not see when the DAC
  switches group, so there is no "real stale word" to count; `tx cfg lat 60, 66, 70, 76` (the write
  takes 59 to 94 cycles, mean 61 to 72) gave identical counters. The app still measures the write
  before each start (64 writes of the value already there) and aims by the mean; `tx cfg lat N`
  fixes it. The `slack` counter (a flip within `tol` words) therefore says little: read `late`
  (beyond `tol`) and the worst flip's breakdown.
- **A flip a few words late** (spin exit up to 249 cycles) came from the loop: a turn did a pull
  (parse, 500 to 1500 cycles) and then a fill step, but the lock lead only covers one step. A turn
  does one thing, a step or a pull, since the position is updated again after the status and
  statistics publishes of that turn (before it, the poll and the fit ran on the position from before
  them, and the publishes cost a few thousand cycles). The stats keep the worst flip: its index,
  time, where the lateness arose (entered past the open spin's start: a step overran; open spin
  ended late: an interrupt on CPU 1; lock late) and the turn before it. The read back (`verify`) does
  32 words a turn.
- **The first window** starts at a multiple of 351 words (the injector's payload is prepared once):
  the words before it are counted apart (`align_gap`), `lost` is what follows.
- **An underrun** is classified when the window closes short: packet words waiting (the fill was too
  slow; the oldest one's age tells arrival late or processing slow), a packet in the ring not taken,
  or nothing there (lost, or not sent yet). Loss does not make a fill late.
- **Reorder**: the injector waits for as many free buffers as the batch has packets (it dropped the
  packets it could not get a buffer for before). That changed nothing in the counts: the late words
  of a `reorder 4` run were identical to those before (9,658,116), so the wait for free buffers
  was not their cause, and the cause is not known. The cursor recovers reordered packets
  (`test_real_config_reorder`); what is left unrecovered in the model is the batches that complete
  within a margin of the flip.
- **Ready words** (`word32` at 2 mod 4): the plain loop cost 9 cycles a word (a load, a shift pair
  and a store each time, with the TCM's load to use stall); now four words a turn with the loads
  first, and a PIE copy (`txs_pie_copy`, 8 words a turn) in the pie set.

## The end of a run, and what is off when

- The **engine** (DAC trigger) is turned off by whoever ends the run first: the flip thread at its
  end for every reason (stop now, drain, missed, hang passes, an early failure), the deadman for a
  thread that does not. The **chain** is turned off by the control thread (the main thread) next, then
  the deadman's timer, then the injector (200 ms, then aborted), then, for a deadman stop, the flip
  thread is aborted: the report comes before the abort, because the abort of a thread spinning with
  interrupts locked (`tx hang lock`) waits for it and may not return; the state stays "ended" and a
  new start is refused (-EBUSY) until a reset.
- `esp_sdr_replay_start()` leaves `esp_sdr_lock` (a mutex) owned by the caller until
  `esp_sdr_replay_stop()`: both run on the control thread. A stop from another thread fails to
  unlock (-EPERM, now logged) and leaves the lock with the starter. Outside this app the same pair
  is used by `apps/rfprobe` and `apps/micropython/cmod/esdr_local.c` (and the replay API test).
- A buffer is on the ring, or on the ring and the flip thread's list for the instructions of a hand
  over, or on the list; `tx_drop()` takes it out of the list before it is released, so the abort at
  any instruction leaks at most one buffer and never releases one twice (the flush at the end
  releases each buffer once).
- The RF gate (`tx tone ... rf`): the library's tuned frequency and LO offset, not what `tx init`
  last succeeded at, with the tone on both sides of the carrier (k x rate / 351) and a guard of 1 MHz
  from 2400 and 2483.5 MHz (`txs_limits.h`, tested). `tx init` clears its record before it tunes.

## Network protocol

The PC feeds the board over UDP; the host tool is `apps/txstream_eth/tools/vrt_tx.c`, the messages
are defined once in `apps/txstream_eth/src/tx_proto.h` (plain C, big endian, `txp_*_put()` returns
the length or -1 for a buffer that is too small, `txp_*_get()` returns 0 or -1 for a wrong length or
magic; the values are checked by `txp_go_valid()`). The header is shared by the board, the tool,
the fake board (`tools/tx_fake.c`) and the test (`tools/test_tx.c`; `make -C apps/txstream_eth/tools
test`).

**Ports.** Control UDP 4992 on the stack, the cookie protocol of `apps/stream_eth/src/ctlproto.h`
(`VRTS`, `VRTC` + cookie, `VRTK`, `VRTB`, `VRTT`, `VRTX`) unchanged: renewal every 250 ms, 5 s
silence ends the session. Data UDP 4994, hook only, 1420 byte packets (16 header + 1404 payload:
702 iq8 samples or 351 w32 words), the format of `txs_wire.h`, stream ID = the session's. Data
and control come from **one** socket of the PC (the hook accepts the subscriber's address and port).
Status goes from 4992 to the subscriber. IPv6 first, IPv4 too.

**Messages** (after the cookie handshake; `cookie` is the 8 bytes of VRTC; the PC's messages are
honoured only from the subscriber and only with the right cookie):

| Message | Dir | Bytes | Layout (offset: field) |
|---|---|---|---|
| `TXGO` | PC to board | 24 | 0: "TXGO", 4: cookie[8], 12: rate u8 (20 or 40 MS/s), 13: fmt u8 (0 iq8, 1 w32), 14: flags u16 (bit 0 RF asked, bit 1 verify CRC), 16: seconds u32 (cap, 0 none), 20: nonce u32 |
| `TXAK` | board to PC | 28 | 0: "TXAK", 4: nonce u32 (of the TXGO or TXEN), 8: code u16, 10: flags u16 (bit 0 RF granted), 12: stream_id u32, 16: max_words u32 (per packet), 20: depth u32 (credit depth, words), 24: group u32 (window, 32768) |
| `TXEN` | PC to board | 16 | 0: "TXEN", 4: cookie[8], 12: nonce u32. Drain and stop; answered by a TXAK (code 0, other fields 0), then TXST with the ENDED flag |
| `TXPG` | PC to board | 24 | 0: "TXPG", 4: cookie[8], 12: seq u32, 16: t_ns u64 (the PC's CLOCK_MONOTONIC) |
| `TXPO` | board to PC | 24 | 0: "TXPO", 4: seq u32, 8: t_ns u64 (echoed), 16: b_ns u64 (the board's free running clock in ns when the ping was handled, any epoch) |
| `TXST` | board to PC | 76 | 0: "TXST", 4: seq u32, 8: the 64 byte `txs_status_put()` record (session, play, contig, credit, restart, underruns, lost, late, stale; the layout is checked against the library by the test), 72: flags u32 (bit 0 RUNNING: play advances, bit 1 ENDED, bit 2 RF on) |

TXAK codes: 0 OK, 1 BUSY (a run goes on, or ended and not reset), 2 bad rate, 3 bad fmt, 4 bad
flags, 5 RF refused (not armed by `tx arm <minutes>` on the UART, or `txs_rf_ok()` says no), 6 not the
subscriber, 7 no resource. A TXGO retried with the same nonce while the run it started goes on is
answered with the same TXAK (the PC resends every 300 ms until it gets one); after that run ended the
same nonce is a stale duplicate and gets code 7 (nothing starts). A silent PC (no VRTS
renewal for 5 s) ends the run; no data for 200 ms during a run drains to zeros and stops (hang rule).
`session` of the status record is the stream ID.

**TXST** goes from a CPU0 thread or timer that reads the `txs_snap`, built once and reused, on
every flip and at least every 250 us while subscribed and a run exists (`play` is the first word
until RUNNING; `credit` before the first packet is the first window's end, then the end of the open
window + `depth`; `restart` as `txs_jit_restart()`). TXPO is answered at once from the same thread;
`b_ns` is any free running clock of the board (cycle counter or `k_uptime` in ns) read when the
ping is handled; only its differences matter (drift of a few tens of ppm is fine).

**Why the pings.** The PC's play index estimate and its lead depend on the one way delays, which
are not equal in both directions and not constant (100 to 600 us on a busy network). TXPG/TXPO give
the least round trip (half of it is the base delay) and, with `b_ns`, the spread of the PC to board
delay alone; the TXST records give the spread of the other direction.

**The sender (`vrt_tx`)**, pacing in `tools/tx_pace.h`, tested against a model board (`tx_board.h`):

- Estimate: `t0 = min over 100 ms of (arrival - play x ns_per_word) - owd_min`, play(t) = (t - t0)
  / ns_per_word. Error in the tests: 0 to 5 words at 20 MS/s, up to 80 at 40 MS/s with 20 ppm drift.
- **Credit limited, line rate capped** (not sample rate paced): the board writes the open window on
  arrival and the credit rises by G at each flip, so the sender sends everything up to the credit as
  soon as it knows it, in bursts of at most 4 packets back to back (`TX_BURST`, `src/tx_hold.h`; the pause after a burst makes up a late wakeup up to 30 us), each burst of n packets followed
  by a pause of n x (wire bits / `--max-mbps`, default 900) so that the average stays under the cap
  (a 1506 byte frame is 12.05 us at 1 Gb/s, 13.4 us charged at 900). A stream that does not fit the cap
  is refused (w32 at 40 MS/s is 1.3 Gb/s; iq8 at 40 MS/s about 660 Mb/s). The sender sleeps on a
  condition variable while it waits for credit and is woken by every TXST. The lead (words ahead of
  the estimated play index) is only a floor that warns (`lead_low`, `min_lead`, under G + 1000).
- What the network may be: after a flip the board needs the rest of the new window, G - depth words,
  by the next flip. So `D_status + D_data <= G/rate - ceil((G - depth)/packet) x line_ns - 300 words
  - one packet` (`txp_delay_budget_ns()`), plus the sender's own wakeup lateness at each of its 3
  or 4 wakeups a flip. From the simulation (constant one way delay both ways, bisection):

  | rate | depth | formula (status + data) | measured, exact sender | measured, up to 40 us late per wakeup |
  |---|---|---|---|---|
  | 20 MS/s | 7020 (before the PSRAM step) | 1115 us | 1138 us | 1033 us |
  | 20 MS/s | 13338 (now) | 1235 us | 1254 us | 1149 us |
  | 40 MS/s | 7020 (before the PSRAM step) | 303 us | 323 us | 206 us |
  | 40 MS/s | 13338 (now) | 424 us | 439 us | 334 us |

  vrt_tx warns when the delays it measures (least delay plus the measured spread of each direction)
  exceed the budget. The credit is never exceeded in any test (`over_credit 0`, board `far 0`).
- With the measured link (ping RTT 0.175 / 0.31 / 1.15 ms, modelled as 88 to 150 us one way and a
  575 us one way spike in both directions for 2 ms every 100 ms), 3 s: 20 MS/s depth 13338 had no
  underrun, depth 7020 29 (27 in steady state); 40 MS/s depth 13338 145, depth 7020 239. A 1.15 ms RTT
  spike now fits 20 MS/s; at 40 MS/s it needs a depth the board cannot hold (the budget at depth
  G is about 0.6 ms).
- Board side receive burst: the sender never puts more than 4 packets back to back (bursts follow each other a few us apart at the 900 Mb/s cap) on the wire (12.05
  us each, so the board's CPU1 at 8.6 us a packet keeps up: the model queue is 1 packet). Network
  jitter can merge two bursts into one run of 32 or more at the receiver, still at the line rate, so the
  queue stays at 1; only a stall of the board's CPU fills the 16 descriptor ring (a 5 ms stall puts
  the whole backlog, 57 packets at 20 MS/s, 133 at 40 MS/s, in the queue: the ring overflows and
  the credit rule cannot prevent it).
- A run that starts after a wait for the arming (`--rf`, TXGO refused with code 5 until `tx arm`) is the run of a
  start without it: the refusals and the pings leave nothing in the pacing (`test_wait()` in `test_tx.c` checks
  the state; `loopback_test.sh` runs it against `tx_fake -R 3`). Two things of that wait were not: the control
  loop's once a second report was timed from the program's start, so after the wait it printed the wait's seconds
  at once (three lines of "sent 0 words": no stalled run, only lines) and, worse, woke the loop at once, every pass,
  for the whole wait (the loop spun at 100 % of a core, then went to sleeping on the sockets when the run started:
  a thread that has just used 3 s of CPU may wake late behind the other tasks of the host, and a status taken a
  millisecond or two late is what the first board run with a wait showed: status delays up to 1969 us against
  389 us in a run without it, more than depth 7020 carried, 53 resyncs and 52,499 words lost in the first half
  second; this is the most probable cause, found by reading the code and the log, not yet confirmed by a second
  run on the board). The loop now keeps its report time ahead of the clock until the run starts. And a resync at the
  edge of the credit (the usual place of a sender that is credit limited: room under a packet) jumped to
  `credit - packet`, below the sender's own index: the last packet's words were sent again (the board counted
  19,986 duplicates, 377 per resync on average); it waits for the next flip's credit instead (`txp_pace_plan()`,
  tested in `test_wait()`).
- Resync (a stall of the board or of the PC): a packet that would arrive before play + (least delay +
  100 us) is late for any window, so the sender jumps to `max(restart of the board, that horizon)`,
  never beyond the credit, and continues; the gap is zeroed by the board. A 5 ms board stall costs 3
  underruns at 20 MS/s (7 at 40), a 50 ms PC stall one per window played without data (31 at 20
  MS/s); none later than 20 ms after the end of either; the lead is back in the band 0.3 ms after the
  first status that follows. A delay spike (a stalled ping answer counts as one) raises the lead
  for the next second, capped at 1 ms of delay.
- Threads: the main thread is the control thread (poll, handshake, renewals, pings every 5 ms,
  statuses into the estimator under a mutex, a line a second); the sender thread plans, fills 6
  packets and sends them with one `sendmmsg` (or one GSO `sendmsg`, `--gso`), then sleeps (absolute
  `clock_nanosleep`) until `--spin-us` (30) before the wake time and spins the rest. `--rt` tries
  SCHED_FIFO 20. About 4800 wakeups a second at 20 MS/s, 9600 at 40, 200 pings and 4000 statuses a
  second on the control thread. The tone is a table lookup (a few ns a sample).
- Before TXGO the tool sends pings for 0.5 s (100 pongs) so that D_max is known when the run starts.
- Samples: `tone:kHz[:amp]` is a pure function of the stream index (phase continuous over any gap),
  `file:PATH` (raw iq8 pairs or w32 words, `--loop` repeats it, else zeros), `stdin` (a 16 MiB ring;
  after a resync it continues where it was, so it is delayed by the gap). Amplitude in iq8 counts
  (1 count = 4 DAC counts of 511; w32 tones use the same scale): default 1; above `--max-amp` (4) the
  tool refuses (exit 2) unless `--allow-amp`; files are scanned first; stdin needs `--allow-amp`.
  The board does not clip.
- Exit when the board ends the run: TXST with ENDED, or not RUNNING for 200 ms after it was, or no TXST for 2 s in
  the run: exit 1 at once ("the board ended the run").
- Exit codes: 0 clean (no underrun, lost or late word, no resync, no send error), 1 not clean or no
  contact, 2 usage or amplitude refused, 3 another host has the stream or the board is busy, 4 the
  board refused TXGO (RF, rate, flags).

**Tests.** `make -C apps/txstream_eth/tools test`: `test_tx` (address and undefined behaviour
sanitizers: messages and bounds, status layout against `txs_status_put()`, header against
`txs_wire_parse()`, the tone, the sliding minimum, 14 simulations of the pacing at 20 and 40 MS/s)
and `loopback_test.sh` (vrt_tx against `tx_fake`, which parses the packets with the board's
`txs_wire_parse()` and checks every payload against the tone; nothing leaves the machine).

**Reusing the core.** `apps/txstream_eth/tools/tx_core.h` (with `tx_pace.h`, `tx_proto.h`) is the whole
session, credit, pacing and header logic as plain C (static inline, no sockets, clocks, threads,
globals or printing; the caller passes CLOCK_MONOTONIC ns and serialises calls). `vrt_tx.c` is only the
CLI around it. The Soapy `VrtLink` would use it as: `txc_init()` at open; a control thread (or the
existing receive thread) loops `txc_poll()` (send each datagram), `txc_rx()` for every datagram from
port 4992, sleeps until `txc_wake()`; `writeStream()` blocks (condition variable signalled by the
receive thread on `TXC_EV_STATUS`) until `txc_plan()` grants packets, copies CS8 straight into the
payload after `txc_header()` (iq8 is cs8 as is), sends, and advances; END_BURST calls `txc_stop()`
(TXEN goes out from `txc_poll()`, `TXC_DONE` when the board reports ENDED); `readStreamStatus()` maps
`txc_counts()` deltas (underruns, late, lost, resyncs) to `SOAPY_SDR_UNDERFLOW`/`TIME_ERROR` events.

## Board side (apps/txstream_eth)

`src/tx_net.c` (hook, control thread, status thread), `src/tx_sess.c` (the TXGO and TXEN state
machine, no Zephyr), `src/tx_filter.h` (the hook's filter, no Zephyr; both are tested on the host in
`tests/unit/txs`). The cookie protocol is `apps/stream_eth/src/ctlproto.c` compiled as it is.

- **Hook** (IRAM, receive interrupt, CPU 0). Frame for our MAC, IPv4 without options and not a fragment
  or IPv6 with UDP next, UDP to port 4994: the stream's. Then the length (UDP, IP and frame agree, at
  least a VITA header and a word), a run that takes data, the subscriber's address, port and our
  address, the session's stream ID: all well, the buffer goes to `tx_stream_put()` (one count in
  flight, refused when no run or the ring is full), else it is dropped there and counted by reason (`tx
  net`: no run, source, stream ID, length, stream busy). Any other frame is the stack's, so a frame to
  port 4994 that does not match never reaches it (no ICMP port unreachable at the packet rate of a
  sender that has not noticed the end).
- **Control** (thread `tx_ctl`, CPU 0, priority 10, flash). `net_context` sockets on 4992 for IPv4 and
  IPv6 (the source MAC from the frame, as in stream_eth). The subscriber's TX messages have a budget of
  3000 a second of their own, everything else the protocol's. TXGO, TXEN and TXPG need the cookie and
  the subscriber (TXAK code 6 otherwise for TXGO and TXEN). TXGO: `tx_sess_go()`, RF only after `tx
  arm`, `tx init`, a carrier that leaves the whole sample rate (a tone at k = 175) inside the band
  (`txs_rf_ok()`); a random stream ID a session (`sys_rand32_get()`); `tx_stream_start()` with source
  net; the data is taken from then on. 5 s without a renewal, a stop (VRTX), a takeover (VRTT) or the
  loss of the address drain the run and end the session. A run on air is drained when `tx disarm` is
  typed or the arming runs out.
- **Status** (thread `tx_stat`, CPU 0, priority 6, above the network threads, the control thread and the
  shell; it sleeps a kernel tick at a time, `CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000`, 100 us, and a ping
  wakes it at once). As a spinning thread at the lowest priority it measured 3,300 to 6,700 cycles a
  status with 0.6 to 1.1 ms worst, because every other thread preempted it, and 1 to 2 % of the reads
  of the stream's record failed: the CPU 1 writer is inside its 16 stores for a few hundred cycles each
  90 us, and eight quick retries fell all inside it (now it retries for 10 us). TXST at every flip (the
  stream counts the statuses it published after a flip) and every 200 us while a run exists, then every
  20 ms with the ENDED flag until the subscription ends (intervals in ticks: the 32 bit cycle counter
  wraps in 13 s); TXPO for every TXPG (`b_ns` is the board's clock when the datagram came to the
  stack). RF is flagged from the start of an RF run (the chain is on in the prestart), RUNNING when the
  engine plays. The header is the subscriber's template, the record comes raw from the `txs_snap`
  (`txs_snap_read_raw()`), the MAC does the DMA from six slots (`dwmac_send_frames()`,
  `CONFIG_ETH_DWMAC_TX_FAST`). `tx net` shows the cycles of a status by part (header, record, seal,
  queue), the worst one, and the failures by kind (no slot, record not read, not queued).
- **Kernel.** A run from the network takes the PIE kernel when it is built, and the fast one if PIE does
  not pass its golden check at the start (`tx_cfg.fallback`); `tx kern` overrides. The scalar kernel is
  slower than a line rate burst (7,120 cycles a packet). `tx net` names the kernel of the last run.
- **Test tone.** `src/tx_tone_gen.h` holds constant integer tables (351 iq8 samples and DAC words, k = 18, amplitude 1 and
  4 counts), made by `tools/gen_tone_table.py` (`--check`). A computed tone differed between the board (soft float)
  and the host at the 18 samples where the cosine or sine is exactly +-0.5 (angles of 60 degrees; one LSB of I or Q
  off, as the verify of the first board runs showed): the tables round those away from zero, no libm on either side.
- **Verify.** TXGO flag 1 makes the stream read its bank back against the board's own tone
  (`tx_tone_gen.h`: k = 18 per 351 samples, 4 DAC counts, the `tx tone` defaults); `vrt_tx --source board
  --verify` sends exactly those samples (a function of the stream index). Words that are not zero and not
  the tone are counted wrong; `tx net` and the end of run report show read back, wrong and zero words.
  Other sources with `--verify` count wrong words (vrt_tx warns).
- **Limits and the hold.** A run is at most 3600 s whatever TXGO asks. The stream holds at most `TX_HOLD` = 25
  buffers of the pool (ring and thread together, 16 + 16 places; the pool has 43, the MAC 16, two stay for the stack: a
  build assert), `tx_stream_put()` refuses a 26th (counted "stream busy"). One configuration, `src/tx_hold.h`,
  with static asserts, for the board and the host: hold = depth packets + burst + 2, so the credit depth
  is `TX_DEPTH_PKTS` = 25 - 4 - 2 = 19 packets (13338 words iq8, 6669 w32; TXAK's depth), the host sends
  at most `TX_BURST` = 4 packets back to back, and the two are the packet the window's end splits and the one
  being placed. At the worst moment the 19 packets wait for the next flip while a burst arrives. (With the
  first pool of 34 the numbers were hold 16, depth 10 packets = 7020 words: a cap of 12 refused the first window's
  packets of a burst, a burst of 14 with 10 waiting needed 26.) More buffers
  (a bigger pool) raise the hold and with it the depth. A refused start (BUSY, no resource) changes nothing of
  the last run.
- **Flip to status delay** (`tx net`): the figures are the run's own. The status thread starts them over when
  `run_gen` rises (a started run) and counts only a flip stamp that lies between the run's start and now, so a
  stamp left by the last run (19.8 s at most, once) is no longer read.
- **Verify, wrong words.** The report names what the wrong words look like: how many are the window's previous
  content (not written yet), how many per 512 words of the window, and the first two (index, place in the
  window, what the bank had, what the tone says). The verify runs only in a turn the poll has no work for and
  with the ring empty, 16 words a turn.
- **Arming belongs to one subscriber.** `tx arm <minutes>` is refused without a subscriber, and arms (under the
  control lock: a takeover cannot come between) the subscriber there is, printing its address, port and MAC. A
  takeover (VRTT), a new subscriber, VRTX or the 5 s silence ends the run and disarms: `tx arm` is needed
  again on the UART. The arming running out or `tx disarm` drains a run on air (once). `vrt_tx --rf`: after
  VRTK it sends TXGO; on code 5 it keeps the subscription, prints once that it waits for `tx arm` on the
  board's UART with its own address, retries TXGO every second and exits 4 after `--arm-wait` seconds (60).
- **Threads and CPUs.** `CONFIG_SCHED_CPU_MASK_PIN_ONLY` makes every thread start on CPU 0 alone (main, the network
  threads, the shell, the log thread, `tx_ctl`, `tx_stat`); only `tx_flip` is pinned to CPU 1, before it starts.
  With `CONFIG_IPI_OPTIMIZE`, `ipi_mask_create()` (kernel/smp/ipi.c) leaves out CPUs not in the readied thread's mask
  and where it would not preempt, and the tick alarm stays on CPU 0 (esp32_sys_timer.c), so during a run CPU 1
  gets no scheduler interrupt from a wakeup on CPU 0. The status thread polls the flip counter at its 100 us
  wake (absolute ticks, `BUILD_ASSERT` on the 10 kHz tick) instead of a `k_sem_give()` from the flip thread:
  that takes the scheduler's global spinlock and sends an interrupt. `tx net` shows "flip to status sent" (mean,
  max in us). After the end it sends the ENDED status every 20 ms.
- **Other.** The injector build refuses TXGO (code 7: its pool is two buffers short of the hold). An explicit
  `tx kern pie` does not fall back: a failed golden check ends the run and the report says so; only the
  default (PIE when built) falls back to fast and says it did. `tx tone` and the verify of a net run set the
  tone tables and start under one lock (`tx_tone_lock()`), and the report buffers are cleared at boot.
- **Engine start** (`tx_prestart()`). With source net the engine does not start with the run: the stream
  thread takes the first packets untimed into the first window and starts the engine when that window is
  complete, or 2 ms after the first packet, whichever is first; with no packet for 150 ms the run ends
  (the deadman would at 200 ms). Statuses go out meanwhile (not RUNNING; the credit is the window's end).
- **RAM** (groups of the TCM, `scripts/tcm_groups.py`). Before the PSRAM step: group 0 129,400 of 131,072 B
  (libc arena 1,664 B), group 3 109,248 B (1,264 B free below the ROM boot stack), PSRAM off, LP RAM 36 B.
  After: group 0 130,552 B (arena 512 B), group 3 110,288 B (224 B free), PSRAM 16,256 B of stacks plus the
  1 MiB heap region of `CONFIG_ESP_SPIRAM_HEAP_SIZE` (nothing uses it) and the control thread's stack, LP RAM
  120 B (the control thread's state and counters, 84 B, and the 36 B that were there). See "Memory tiers" below.
  In IRAM: the hook 626 B, `tx_stream_put()` 188 B, the raw status
  read 78 B, the TX fast path of the MAC driver about 400 B. The control and status code, the cookie
  code, the shell and the filter's callers are flash. The injector is `CONFIG_APP_TXS_INJECT` (off by
  default: its 2.4 KB do not fit group 3 next to the network side); `injector.conf` builds it with two
  receive buffers fewer (41 instead of 43).
- **Memory tiers** (the user's rule: make all memory available; hot paths and DMA buffers in fast internal RAM,
  cold things out). PSRAM is the board's 16 MB octal chip at 250 MHz from the MPLL at 500 MHz
  (`CONFIG_SPIRAM_SPEED_250M`; the chip init sets the PLL, `esp_psram_impl_ap_oct.c`). The Ethernet reference clock
  is the APLL at 125 MHz (the driver, `eth_esp32_dwc_ether_1000.c`), the PTP reference PLL_F80M comes from the
  BBPLL: `tx_mem_report()` logs MPLL, APLL, PLL_F80M and CPLL once at boot (and `tx boot` again) and warns
  when the MPLL is not 500 MHz or the APLL not 125 MHz.
  - *In PSRAM*: the stacks of the main thread (the stream's control thread), the system work queue, the log
    thread, the shell, network management (with the ring of its event queue) and the network receive thread (about 16.2 KB,
    `lib/esp_sdr/esp_sdr_txmem_s31.ld`, by section name), and `tx_ctl`'s (2 KB, `place.h`). Not the shell's history heap, nor anything else a kernel init hook sets up
    (k_heap, k_mem_slab backing stores: set up at PRE_KERNEL_1 priority 30, while PSRAM is accessible only from
    priority 40: the first board image died on a store fault there). `scripts/tcm_groups.py --tx` fails the
    build when a `kheap_*`, `_k_mem_slab_buf_*`, idle or interrupt stack symbol lies in PSRAM or LP RAM, and the
    linker asserts it for the interrupt stacks and the system heap.
  - *In LP RAM* (0x2E000000, `.rtc_noinit`, the category region `ESP_SDR_LPRAM` of `esp_sdr_place.h`): the control
    thread's session state and its counters (`nc` in `tx_net.c`). Written a few times a second. It is not
    zeroed at boot (`tx_net_init()` does it), no atomics, no DMA. Its access time from the HP core is not measured.
  - *Stays internal, and why*: the receive buffers and descriptors (DMA, the receive interrupt); the interrupt
    and idle stacks; the flip thread's stack and state and everything CPU 1 reads (`st`, the tone tables, the
    banks); the status thread's stack and the status slots (the MAC reads them; the thread wakes every 100 us and
    its delay budget at 40 MS/s is 0.42 ms, a stack line that misses the cache costs about a microsecond);
    the system heap (`k_malloc`: the interrupt allocator's vector descriptors, `shared_intr_isr()` walks them in
    interrupts, and the PHY's calibration data); the network stack's transmit buffers (see below).
  - *Net transmit buffers*: the MAC driver writes them back before the DMA (`sys_cache_data_flush_range()` in
    `eth_dwc_ether_1000_core.c`), the pool is aligned to 64 B. On the board (2026-11-26, multiapp, same MAC) the
    EMAC's DMA does not reach PSRAM: every send from a PSRAM buffer failed, and receive buffers in PSRAM lost
    frames of 500 bytes and more. They stay internal (4 x 1536 B and their records, 6.9 KB);
    `CONFIG_ESP_SDR_S31_TXMEM_NETTX_PSRAM` is broken, keep it off.
  - *CPU 1 and PSRAM*: nothing CPU 1 runs touches PSRAM. The flip thread's code is in IRAM and its data in groups
    0 and 3 (`st`, `pub_stats`, the tables, the receive buffers it fills from), `txs_*` and the PIE kernels work on
    those and the banks, `tx_stream_put()` (the receive interrupt, CPU 0) touches `st` and the receive buffers.
    The only PSRAM objects are stacks of CPU 0 threads and the event queue ring of network management (CPU 0). The
    one flash call CPU 1 made, `net_buf_unref()` for each packet, now runs from IRAM (`ram_netbuf.ld`). The cold
    state of `tx_ctl` is in LP RAM (`nc`), not PSRAM. `scripts/tcm_groups.py --tx`
    checks after the link (a build step, `txs_layout_check`) that each stack is where it should be, by symbol.
    CPU 1 then uses neither the flash cache nor PSRAM, so the traffic of either does not touch it.
  - *CPU 0*: a thread whose stack is in PSRAM misses the cache on its first touch of each 64 B line (about 0.3
    to 1 us at 250 MHz octal), and an interrupt that arrives while such a thread runs saves its registers on that
    stack (30 words) before it switches to the interrupt stack: one or two misses at the interrupt's entry.
    Those threads run rarely (the shell, the log, management, `tx_ctl`, the work queue), the idle thread and the
    status thread have internal stacks, and the receive interrupt is mostly taken in idle. Not measured on the board.
  - *Freed internal RAM*: 13.9 KB net in group 3 (the stacks, 16.2 KB + 2 KB, less 2 KB of `st`, 0.5 KB of
    `tx_thread` and 1.9 KB of the library's gain table, which moved there from group 0 to make room for the PSRAM
    driver's code and data: 5.9 KB). Group 0 also lost 1.9 KB of network tables (`CONFIG_NET_MAX_ROUTES` and the
    neighbour, connection and context counts) and 512 B of log buffer. The nine buffers it bought (43
    instead of 34, 14,976 B) raised the hold from 16 to 25 and the credit depth from 10 to 19 packets.
  - *Not done*: the code in group 0's IRAM that only runs at boot (about 10 KB of init, clock and PHY setup code
    that the SoC's linker script keeps in IRAM) is the larger prize; it needs the fork's script and is planned
    separately.
- **Shell**: `tx net` (subscriber, run, the hook's counters, control counters, statuses and pings, the
  status cost, stack use), `tx arm <minutes>`, `tx disarm`.

<!-- nav-start -->
Previous: [S31 to host at 80 MS/s through a Colorlight 5A-75B](s31-fpga-bridge.md) · Up: [Reference](index.md) · Next: [Network services on the S31 multiapp host](network-services.md)
<!-- nav-end -->
