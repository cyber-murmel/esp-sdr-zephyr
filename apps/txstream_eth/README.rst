ESP-SDR transmit stream over Ethernet
#####################################

The S31 DAC streamed by whole-group ownership flips on CPU 1, fed over UDP by ``vrt_tx``
(``tools/vrt_tx.c``; control on port 4992, samples on port 4994, ``src/tx_net.c``, protocol in
``doc/dev/txs.md``) or, with ``CONFIG_APP_TXS_INJECT`` (``-DEXTRA_CONF_FILE=injector.conf``), by a tone
from an injector thread that stands in for the network, in the transmit memory layout
(``CONFIG_ESP_SDR_S31_TXMEM``: TCM groups 1 and 2 the DAC banks, image in group 0, buffers, heaps
and stacks in group 3), with the gigabit Ethernet up (DHCP, IPv6). The flip thread is in
``src/tx_stream.c``, the injector in ``src/tx_inject.c``, the signal in ``src/tx_tone.c``; the
position model, the flip schedule, the jitter cursor and the credits are the txs module
(``lib/esp_sdr/src/txs``, ``doc/dev/txs.md``). The main thread is the stream's control thread: it
turns the chain on and off (on one thread, the library's lock needs it) and prints the report.

Build (from the workspace root)::

    .claude/coord/wt.sh s31-tx 'west build -p -b esp32s31_function_coreboard/esp32s31/hpcore \
        $W/apps/txstream_eth -d build-clanker/esp32s31_function_coreboard/s31-tx-txstream -o=-j4'
    python3 scripts/tcm_groups.py <build dir> --tx

Memory: the image and the hot data are in the TCM (group 0 the image, group 3 the buffers; groups 1 and 2 are the
DAC banks). The 16 MB PSRAM is on, 250 MHz from the MPLL at 500 MHz, and holds the stacks of the threads with slack
(main, the work queue, the log, the shell, network management and the receive thread of the stack, ``tx_ctl``); the
LP RAM holds the control thread's state and counters. That freed group 3 for receive buffers: 43 of them, the stream
holds 25 (``src/tx_hold.h``), the credit depth is 19 packets (13338 words 8+8 bit). Receive buffers and descriptors,
interrupt stacks, the flip and status threads and everything CPU 1 touches stay internal; CPU 1 never touches PSRAM.
``main()`` logs once the PSRAM size, speed and init time and the PLL-derived clocks (MPLL, the EMAC reference,
PLL_F80M, CPLL); ``tx boot`` prints them again. ``doc/dev/txs.md``, "Memory tiers".

Shell on the UART
=================

``tx tone`` and ``tx inject`` need the injector build (``-DEXTRA_CONF_FILE=injector.conf``); the default image says
so and does nothing (it used to print ``start failed: -134``).

``tx boot``
    Ownership register, ``dump_ready``, addresses and groups of the image, heaps, receive buffer,
    interrupt and thread stacks; the layout checks of ``scripts/tcm_groups.py --tx`` at run time;
    the canary of groups 1 and 2 and the free tail of group 3 over 200 ms.
``tx init [MHz]``
    ``esp_sdr_init()`` and the first tune (2437): the ownership register before and after each,
    the MAC's counters around it, the canary, the time. A failing tune leaves the board counted as
    not tuned (``tx tone`` refuses until a tune succeeds).
``tx tone <20|40> <seconds> [rf] [amp 1..40] [k 1..175] [fmt iq8|w32] [verify 0|1]``
    A tone of k cycles per 351 samples (default 18, 1026 kHz at 20 MS/s), amplitude amp of 511
    counts (default 4), sent as packets of 351 (w32) or 702 (iq8) samples by the injector through the
    txs path, both groups prepared, refilled one whole group behind the play position, flipped on
    the model clock. ``fmt`` is the payload (8+8 bit or ready words), ``verify 1`` reads the written
    window back against the tone. Without ``rf`` the transmit chain stays off. With ``rf`` the
    library's replay start turns the chain on, then CPU 1 restarts the index; allowed only when the
    carrier (the library's tuned frequency and LO offset) plus and minus the tone lies inside 2400 to
    2483.5 MHz with a 1 MHz guard at each edge. Returns at once; the summary prints when the run
    ends. The amp is the level: the S31's chain-on ignores the library's power step.
``tx kern [ref|fast|pie]``
    The expansion kernels: reference, plain C, PIE (CPU 1 vector unit). Each is checked against the
    reference and timed at the start of a run.
``tx inject [off|bench [iq8|w32]] [loss <permille>] [reorder <n>] [stall <ms>]``
    The injector's faults, also on the running stream: packets dropped (0 to 1000 permille), sent in
    reverse batches of n (up to 8), a pause of ms (0 to 50, in 10 ms slices) once a second. A stall beyond about 100 ms (64 zero
    passes at 20 MS/s) ends the run by design, as the flip scheduler's hang rule. ``bench``
    measures how fast it can make packets alone (not while a stream runs).
``tx cfg [lat <0|20..200>] [tol <0..1024>] [ovh <0..2000>]``
    The flip's ownership write latency in cycles (0: measured before each start, the default), the
    words a flip may land late before it counts, the cycles a step costs besides its words. Applies
    from the next start.
``tx stop [now]``
    Drain: one pass of zeros, then engine off and chain off. ``now``: at the next poll.
``tx hang [lock]``
    Test: the flip thread spins, the deadman (timer 1 interrupt on CPU 0, a tick per half pass,
    5 ticks without a flip) stops the engine and the chain goes off. With ``lock`` it spins with
    CPU 1's interrupts off (``arch_irq_lock()``, not with ``rf``): the abort of the thread may then never return, the report
    comes first and a new start is refused until a reset.
``tx stat``
    The statistics, and the stack bytes the flip and control threads never used.
``tx net``
    The network side: subscriber, run, kernel of the last run (a run from the network takes the PIE
    kernel when built, the fast one if PIE fails its golden check, ``tx kern`` overrides), the verify
    counts, the hook's counts by reason, control messages, statuses by part in cycles, the stacks.
``tx arm <minutes>``, ``tx disarm``
    A run from the network may ask for RF (``vrt_tx --rf``) only inside the arming, after ``tx init``,
    with the carrier inside the band for the whole sample rate. ``tx arm`` is refused without a
    subscriber and prints the subscriber's address, port and MAC it arms: the arming is for that
    subscriber only. A takeover (VRTT), a new subscriber, VRTX or the 5 s silence ends the run and
    disarms (``tx arm`` again). A run on air drains when it is disarmed or runs out. ``vrt_tx --rf``
    keeps its subscription when the board refuses RF (code 5), says it waits for ``tx arm`` on the
    board's UART with its own address, asks again every second and gives up (exit 4) after
    ``--arm-wait`` seconds (60).

First test without RF, on the board and then from the host (``tools/vrt_tx``)::

    tx boot ; tx init ; tx net
    vrt_tx --to <board address> --rate 20 --fmt iq8 --source board --verify --seconds 180
