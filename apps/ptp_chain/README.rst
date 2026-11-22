PTP chain
#########

What it measures: the offset between the IEEE 1588 hardware clocks (the DWMAC's system time) of two
ESP32-S31-Function-CoreBoard-1 boards cabled directly by gigabit Ethernet (PHY YT8531). A
self-sustaining ping-pong ("chain") of raw Ethernet frames runs between them; every number comes
from MAC hardware timestamps (tx stamp in the transmit descriptor, rx stamp in the receive
descriptor), never from software time. The receive hook of the MAC driver
(``CONFIG_ETH_DWMAC_RX_FAST``) answers each frame from the interrupt, and the transmit stamp
(``dwmac_tx_frame.stamp``, ``CONFIG_ETH_DWMAC_TX_FAST``) comes back with the completion.

Build and flash
===============

::

    .claude/coord/wt.sh s31-ptp 'west build -p -b esp32s31_function_coreboard/esp32s31/hpcore $W/apps/ptp_chain -d build-clanker/esp32s31_function_coreboard/s31-ptp-ptp_chain -o=-j4'

Flash both boards (``west flash -d <build dir>``). The console is the board's UART (CP2102N).
The PTP subsecond mode is digital rollover (ns), ``CONFIG_PTP_CLOCK_DWC_MAC_FINE_SSINC=y`` gives
the 13 ns stamp grid at 80 MHz; ``chain_math.h`` converts binary rollover (2^-31 s) too.

Shell
=====

On one board ``chain start`` (that board is the *seme*, the other answers by itself, the *uke*).
If both are started, the lower MAC stays seme.

``chain peer [aa:bb:cc:dd:ee:ff | clear]``
    The other board's MAC (``chain info`` prints a board's own). Frames go to it as unicast,
    never broadcast, so the boards can share a LAN. The uke learns the peer from the first
    valid chain frame and keeps it; the seme needs ``chain peer <mac>`` before ``chain start``.
    Setting or clearing works only while the chain is stopped. Without an argument: print it.
``chain start [interval_us] [rounds_per_burst]``
    Seme: a burst every ``interval_us`` (default 7812, 128 bursts/s), ``rounds_per_burst`` round
    trips of two frames each (default 1, at most 100).
``chain stop``
    Stop the bursts (the board still answers).
``chain stat on|off``
    Every 1 s: exchanges ``n``, line fit of offset over the window (``off`` at the window centre,
    ``rate`` in ppb, ``rms`` of the residual, all ps and ppb of peer minus local), mean ``delay``,
    counters and the die temperature ``temp`` (``coretemp``, 1 degree C steps plus the fixed
    eFuse calibration offset; ``nan`` without the sensor). The servo lines carry ``temp`` too.
``chain dump [n]``
    The last n (at most 4096) exchanges as ``seq,base_sec,t1_ps,t2_ps,t3_ps,t4_ps`` lines for host
    analysis. Servo steps appear as ``#`` lines.
``chain info``
    MAC, role, subsecond mode, PTP time, link state and speed, MAC rx counters, chain counters.
    The PTP clock's nominal rate is not readable from the app (the driver sets it up).
``chain servo on [period_ms] [bw_hz] [clamp_ns]`` / ``chain servo off``
    On the uke: a rate-only PI loop in the stats thread (defaults 50 ms, 2 Hz, 500 ns). Every
    period the mean offset of the exchanges of that period drives a critically damped PI
    (``chain_servo`` in ``chain_math.c``: wn = TAU bw, kp = 2 wn, ki = wn^2, scaled for ps in and
    ppb out) and ``ptp_clock_rate_adjust`` gets the absolute ratio ``1 + rate_ppb * 1e-9`` (the
    driver scales its default addend, so it is not cumulative; 0.23 ppb per addend step). The
    clock is stepped (``ptp_clock_set`` from 1 ms, else ``ptp_clock_adjust``) only when the offset
    is above the clamp (the first lock included); a step leaves the integrator and the learned
    rate alone, and the exchanges from before it are dropped. Every 1 s the uke prints
    ``servo n= mean= rms= rate= integ= steps=`` (mean and rms of the offsets of the window in ps,
    the applied rate and the integrator in ppb). Default off. After more than 5 periods in a row
    that each stepped, the servo starts over from the nominal rate (``servo: ... restart``).
    With 1 round per burst the uke's exchange spans a whole burst interval and arrives a burst
    late; at the 2 Hz default that loop oscillated and locked out (2026-11-26 soak). Use at least
    2 rounds per burst with the servo (the 0.085 ns run used ``chain start 10000 50``).
``chain pps <n>``
    Fixed mode: write the PPS0 control field (0 to 15, PPSEN0 off). The MAC's PPS output goes to
    GPIO2 (J2 pin 7, ``CONFIG_APP_CHAIN_PPS_PIN``) through the GPIO matrix (signal
    ``EMAC_PTP_PPS_PAD_OUT_IDX``). Measured on the S31 (oscilloscope, digital rollover): 0 gives
    one pulse per second, one 13 ns increment wide; n gives a rising edge every 2^(31 - n) ns of
    PTP time (8: 8.389 ms, 119 edges/s). Every edge lands on the first increment past its
    nominal time, with a different increment phase, so many edges average far better than 1 PPS.
``chain pps flex [width_us] [clk]``
    Flexible mode: a pulse train, interval 1 s, width ``width_us`` (default 1000), the first rising
    edge on the PTP second boundary two seconds after the command. PPSEN0 = 1, TRGTMODSEL0 = 3
    (target time starts the output, no interrupt), interval and width registers = ticks - 1,
    target = (second + 2, 0), then PPSCMD0 = 2 (start pulse train). A tick is the sub-second
    increment (SSIR, 13 ns with ``FINE_SSINC``): 1 s is 76923077 ticks, a 1000 us width 76923
    ticks, so the period is 1.000000001 s in PTP time. ``clk`` counts in reference clock cycles
    (12.5 ns at 80 MHz) instead. ``chain pps stop`` stops the train.
    Absent on the S31 (measured 2026-11-26): with PPSEN0 written first, the PPS control register
    reads back 0x5 (only the fixed-mode field sticks, PPSEN0 and TRGTMODSEL0 do not), and the
    interval register reads 0. Then ``pps flex`` and ``pps stop`` write the old control value back
    (a fixed-mode output runs on) and return -19. Use fixed mode.

The registers are the ones of the DWMAC 3.7 PTP block at EMAC base + 0x700 (HAL
``emac_ptp_struct.h``: the struct places timestamp control at 0x700, addend 0x718, target seconds
0x71c and nanoseconds 0x720, PPS control 0x72c, PPS0 interval 0x760 and width 0x764, checked with
``offsetof`` against the offsets in the driver's ``eth_dwmac_priv.h``). The S31's MAC has
the fixed-mode PPS field only: ``pps flex`` selects flexible mode, writes a pattern to the interval
register and fails with -19 because neither sticks.
``devmem`` is in the shell for register peeks.

Frame
=====

EtherType 0x88B5, destination the peer's MAC (``chain peer``; frames to us or to broadcast are
accepted, others dropped), source own MAC, 60 bytes. After the 14 byte
Ethernet header, little endian (``src/chain.c``):

======  ====  ==========================================================
offset  size  field
======  ====  ==========================================================
0       2     magic 0x7c1a
2       1     version 1
3       1     flags: bit0 reply, bit1 sender is seme, bit2 rx stamp valid, bit3 tx stamp valid
4       4     seq (each node numbers its own frames)
8       4     reply_to_seq (valid with the reply flag)
12      2     hops left (the receiver sends the next frame with hops-1 while hops > 0)
14      2     0
16      12    rx stamp: rx_seq, sec, subsec: the sender's rx stamp of the last frame it received
28      12    tx stamp: tx_seq, sec, subsec: the sender's tx stamp of its previous own frame
======  ====  ==========================================================

Chain and exchanges
===================

The seme's timer sends the first frame of a burst with ``2 * rounds - 1`` hops; every received
chain frame triggers the next frame from the receive hook on both ends until the hops run out. A
lost frame ends that burst, the next timer tick restarts the chain. The carried stamps flow across
bursts, so no stamp is lost at a burst end.

An exchange is a pair of frames m (X to Y) and m+1 (Y to X) with m+1 a *direct reply* to m
(reply flag and reply_to_seq): a pair spanning a burst gap would be biased by the rate offset times
the gap. At node X, with t1 = X tx(m), t2 = Y rx(m), t3 = Y tx(m+1), t4 = X rx(m+1)::

    offset = ((t2 - t1) - (t4 - t3)) / 2      (Y minus X)
    delay  = ((t2 - t1) + (t4 - t3)) / 2

Both pairings complete at each node, about one exchange per received frame:

* X started the pair: t2 comes in the reply, t3 in the next frame of Y; completes one frame later.
* Y started the pair (Y tx p, X rx p, X tx s, Y rx s): completes when the next frame of Y arrives,
  a reply to s or the first frame of the next burst (it carries Y's tx of p and Y's rx of s). The
  carrier is matched by those carried sequence numbers only, so with one round per burst both
  nodes get one exchange per burst. Stored as t1 = X tx s, t2 = Y rx s, t3 = Y tx p,
  t4 = X rx p: the same formulas give Y minus X, so a log line is the same whichever side began.

The stats thread drops exchanges with a mean delay below 0 or above 100 us (``rejected``: stamps
of two clock eras, e.g. after a peer reboot), and after a servo step the exchanges whose t1 is
from before the step. A burst tick is skipped (``skipped``) while a chain frame went in or out
less than 1 ms ago. A servo offset of 1 ms or more sets the clock (``ptp_clock_set``), a smaller one
adjusts it.

Lost frames, non-reply frames, repeats and reordered frames (u32 sequence compare with wrap) give
no exchange. Stamps are int64 picoseconds from the start of the second of t1 (no rounding to ns).

Real time
=========

The EMAC interrupt, the hook, the burst timer and the stats thread are on CPU 0. The hook does no
logging, allocation, blocking or ``irq_lock``; the hook, the sender and the pairing are in IRAM
(``place.h``, ``chain_ram.ld``). Exchanges go through a lock-free single producer ring to the
thread. Counters (tx sent, slot busy, driver refused, tx stamp missing, rx stale or lost, ring
overflow, re-entry) are in ``chain info``. The stats thread reads the die temperature once a
second through the ESP HAL, which locks interrupts on CPU 0 for a register read; the first read
(range selection, a 300 us wait) is done when the thread starts, before a chain can run. A range
change (outside -10 to 80 C) would wait 300 us again.

Measured and not verified
=========================

* Unit tested on the host (``tests/unit/ptp_chain``): stamp conversion, pairing, signs, fit.
* Run on two CoreBoards over a direct gigabit cable (2026-11-26, chain stats and ``chain dump``
  analysed on the host, in-loop only, no external reference): no frame lost in 3 million
  exchanges, 5.4 to 5.7 ns rms per exchange (white: means of 98 exchanges 0.62 ns), mean one-way
  delay about 519 ns, TDEV 0.6 ns at 1 s at 128 bursts/s; with the servo on 0.085 ns rms of the
  1 s offset means. Free-running crystal pair: ADEV about 4e-9 to 5e-9 at 1 s, rate drifting by
  about 120 ppb in 9 minutes after power-up.
* PPS against an oscilloscope (2026-11-26, Siglent SDS2504X Plus, 10x probes on both GPIO2,
  scope FRR statistics C1 to C2 at 15 measurements/s, both boards ``chain pps 8``, servo on at
  ``chain start 1000 2``): per edge 7.6 ns rms between the boards (both probes on one pin:
  15 ps rms, skew -0.05 ns, subtracted below). Over 12 link-ups (both boards reset, 40 s of
  edges each, about 0.3 ns standard error) the mean one-way delay took the levels 516, 520,
  524, 528 and 532 ns. At 516, 524 and 532 ns the PPS offset was -0.18 ns on average (6
  link-ups, -0.48 to +0.42 ns); at 520 and 528 ns it was -3.6 or +3.9 ns (6 link-ups). Each
  direction's delay is set at link-up in 8 ns steps (one 125 MHz period): the mean delay moves
  in 4 ns steps and the offset error is half the difference, 0 or 4 ns depending on the
  parity, with a sign the two-way exchange cannot see. Renegotiating the link until the delay
  is on an "even" level leaves about 0.2 ns between the boards' PPS. The levels belong to this
  cable and pair of boards; that an even level never carries 8 ns rests on 6 link-ups.
* The same over a cheap unmanaged gigabit switch (2026-11-26, only the scope and the two boards
  on it, 12 link-ups, same method): mean one-way delay 2824 to 2838 ns with no levels (3.5 ns
  rms), 36 to 40 ns rms per exchange, 13 to 17 ns rms per PPS edge. The PPS offset was -3.2 to
  +3.6 ns, 2.3 ns rms over the link-ups (about 0.6 ns standard error each), mean +0.4 ns. With
  four links on the path, each set at its own link-up, the even/odd rule of the direct cable
  does not carry over (correlation with the delay 0.5).
* Offset is exact only for symmetric one-way delays; a cable and PHY asymmetry shows up as a
  constant bias of half the difference (above: 0 or 4 ns per link-up on the direct cable).
  Stamp resolution is the 13 ns increment grid.
