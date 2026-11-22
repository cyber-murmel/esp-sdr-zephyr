ESP-SDR receive bench
#####################

What it measures: how fast UDP can reach the ESP32-S31-Function-CoreBoard's gigabit Ethernet when
the network stack is not in the way. The MAC driver's receive hook
(``CONFIG_ETH_DWMAC_RX_FAST``, ``<zephyr/drivers/ethernet/eth_dwmac_fast.h>``) sees each frame in
the receive interrupt, before a ``net_pkt`` exists. It takes UDP/IPv4 and UDP/IPv6 unicast to port
**4993** (and only that) by parsing the headers of the raw frame, puts the buffer on a lock-free
single producer single consumer ring, and wakes a consumer thread pinned to CPU 1. The consumer
checks a 32-bit sequence number and a 4 byte pattern in each payload, counts packets, bytes, gaps,
duplicates and out-of-order packets, and gives the buffer back. Everything else (ARP, neighbour
discovery, ICMP, DHCP, other ports) takes the stack's usual path: the board keeps its normal
IPv4/IPv6 stack beside the hook.

The purpose is feeding I/Q samples from a PC to the S31 for transmission (8+8 bit at 20 MS/s is
320 Mb/s, at 40 MS/s 640 Mb/s). The zperf receive through the whole stack peaks at 244 Mb/s. The transmit
stream built on this hook is ``apps/txstream_eth``.

Payload
=======

``src/rxb_proto.h`` (shared by the board and the host tool): bytes 0 to 3 the sequence number, big
endian; from byte 4 the pattern ``A5 C3 5A 3C`` repeated to the end. At least 8 bytes. The board's
check is sparse by default (the first pattern word and the last 4 bytes, a few cycles whatever the
length); ``rxb start full`` reads every byte (about 4 cycles a byte: not for 600 Mb/s).

Build
=====

On the board (the numbers are in ``prj.conf`` and ``Kconfig``)::

    west build -b esp32s31_function_coreboard/esp32s31/hpcore apps/rxbench

``CONFIG_NET_BUF_RX_COUNT`` (128) must cover the DMA ring (``CONFIG_DWMAC_NB_RX_DESCS`` 64, of which
63 buffers are always with the MAC), the app's ring (``CONFIG_APP_RXB_RING``, 32) and 16 for the
stack: the build checks the sum. RAM: a buffer is 1536 bytes of data plus 128 of ``net_buf``, 1664
bytes; 64 buffers are 104 KB, 96 are 156 KB, 128 are 208 KB (the image without them uses about
144 KB of the 492 KB of internal RAM; the whole image with 128 buffers 352 KB). A pool of 96 needs
``CONFIG_DWMAC_NB_RX_DESCS=48`` or a smaller ring.

The host tool (gcc, Linux)::

    gcc -O2 -Wall -Wextra -pthread -o udp_tx tools/udp_tx.c -lm
    ./udp_tx --selftest              # loopback: IPv6, IPv4, GSO, rate accuracy, refusals

Shell on the board
==================

``rxb start [full]``
    Takes the counters as the baseline, starts taking frames and prints the addresses to send to.
``rxb stat``
    Rate over the time since the last ``stat`` (UDP payload and frame Mb/s, packets/s); totals;
    lost packets (``lost`` counts skipped numbers, ``ooo`` the late arrivals of some of those, so
    ``lost - ooo`` is the net loss), duplicates, bad payloads; the hook's counts (ring full
    drops); the ring's high-water mark and the consumer's time per packet; interrupts and frames
    per interrupt; refills that found the pool empty, frames the MAC dropped (DMA missed frame
    counter, FIFO overflow); CPU free per core; the MMC receive counters since start.
``rxb stop``
    Stops counting. The port stays claimed: its frames are dropped in the interrupt (and counted),
    not passed to the stack, which would answer each one with an ICMP port unreachable.

Run it
======

1. Connect the PC and the board on the gigabit LAN, flash, open the console. The log says
   ``rxbench ...``; ``net iface`` shows the addresses. Ping the board from the PC first (this also
   teaches the switch the board's MAC, see Safety).
2. ``rxb start`` on the board prints its addresses. IPv6 is the preferred way, a link-local
   address needs the interface on the PC::

       ./udp_tx -r 300 -t 10 fe80::...%enp0s20f0u1u3       # 300 Mb/s of payload, 10 s
       ./udp_tx -4 -r 300 -t 10 192.168.178.151             # IPv4, 1440 byte payload
       ./udp_tx -r 600 -t 10 -G 8 fe80::...%enp0s20f0u1u3   # 600 Mb/s with UDP GSO

3. ``rxb stat`` on the board during and after the run. The tool prints what it really sent
   (packets, rate over the true elapsed time, lateness of its batches); the board's packet total
   must match. ``udp_tx`` takes the first IPv6 address of a name before any IPv4 one (``-4``
   forces IPv4), and refuses multicast, broadcast, unspecified and IPv4-mapped IPv6 addresses.
4. Climb: 100, 200, 300, 450, 600, 800 Mb/s (the table above is what the board did). Look for the first step where ``lost`` rises, ``ring
   full drops`` rises (the consumer is behind: bigger ``CONFIG_APP_RXB_RING`` with more buffers),
   ``refills found the pool empty`` rises (more buffers), or ``MAC missed frames`` / ``FIFO
   overflows`` rise (the interrupt cannot keep up with the DMA).

Measured on the board
=====================

IPv6 link-local, 8 s per step, 1420 byte payloads, ``udp_tx`` from the PC, SMP on (the receive
interrupt and the consumer on separate cores, ``CONFIG_DWMAC_NB_RX_DESCS=64``, 128 buffers, ring
32):

=========================  ============================================================
Offered (UDP payload)      100, 200, 300, 400, 500, 600, 700, 800 Mb/s (800 is 70423 packets/s)
Sent / received            2,535,228 / 2,535,228 over all steps
Lost, bad, duplicates      0, 0, 0
MAC missed frames, FIFO    0, 0
Ring full drops            0 (at most 24 of 32 waiting)
Consumer                   2.5 us a packet
Frames per interrupt       1.0 (the interrupt takes each frame as it comes)
CPU free during 800 Mb/s   about 48 % and 26 % (the two cores)
=========================  ============================================================

IPv4 at 300 Mb/s: 208,335 of 208,335.

Read the CPU figures with care: ``rxb stat`` prints the mean of the time since the last ``stat``,
and a 74 % / 63 % free reading is that mean over an interval that was half idle (the stream ran
for only part of it). The 48 % / 26 % are what the cores had while the 800 Mb/s stream ran.
"CPU free" is also relative to a CPU that was idle at boot (the load threads are timed once, in the
first second): a start under load does not change that baseline.

The sequence accounting does not forget an outage: a pause in the stream is counted as the packets
that did not come (``lost``), and a sender that starts again from a lower number (more than 65536
behind) counts as a ``restart``, not as loss or reordering. ``rxb start`` begins a new run.

SMP and one core
================

The app runs with SMP (the default): the receive interrupt, the network threads and the consumer
share two cores, the consumer pinned to CPU 1. ``onecore.conf`` (``EXTRA_CONF_FILE``) builds it
for one core: ``CONFIG_SMP=n``, the pin is guarded by ``CONFIG_SMP && CONFIG_SCHED_CPU_MASK``.
That build compiles; it has not been run on the board.

Transmit memory layout
======================

``txmem.conf`` (``-DEXTRA_CONF_FILE=txmem.conf``) builds the bench in the layout of an S31 transmit
image (``CONFIG_ESP_SDR_S31_TXMEM``, lib/esp_sdr/esp_sdr_txmem_s31.ld): TCM groups 1 and 2
(0x2F020000 to 0x2F060000) are left to the DAC banks, text, data, bss and the libc heap are in
group 0 (below 0x2F020000), everything ``.noinit`` (the 34 receive buffers of 1664 bytes, stacks,
kernel heaps, the MAC's descriptors) in group 3 from 0x2F060000 up to the ROM boot stack at
0x2F07AFB0. The linker asserts all three; ``scripts/tcm_groups.py <build dir> --tx`` lists the
sections per group and fails if one lies in groups 1 or 2. Nothing is dropped from ``prj.conf``.

The 34 buffers are the MAC's 15 (16 descriptors), the app's ring (16) and 3 for the stack
(``CONFIG_APP_RXB_STACK_HEADROOM=2`` is what the build's conservative sum needs). With a shallower
ring: ``CONFIG_APP_RXB_RING=8``, ``CONFIG_APP_RXB_STACK_HEADROOM=10``.

``canary fill [seed]`` writes a pattern over groups 1 and 2 (256 KiB), ``canary check`` counts the
words that differ: run ``canary fill``, ``rxb start``, the stream, ``rxb stop``, ``canary check``;
it must print ``PASS`` (0 words changed). The command exists only in this layout, because in the
normal one the image lives in those groups.

Safety on the LAN
=================

* The board claims one thing: unicast UDP to its own MAC address and port 4993 (IPv4 or IPv6, no
  fragments, no IPv6 extension headers, no VLAN tag). Frames for any other port, multicast,
  broadcast, ARP, neighbour discovery, ICMP and DHCP are not claimed and take the stack's path.
* ``udp_tx`` sends only where told: one host on the command line, which must be unicast (multicast,
  broadcast, the unspecified address and IPv4-mapped IPv6 are refused; the socket cannot broadcast
  either). It needs
  a rate (there is none by default), stops by itself after ``-t`` seconds (10 by default) and
  refuses more than 1000 Mb/s on the wire and packets that would fragment on a 1500 byte MTU.
* A claimed flood cannot starve ARP, neighbour discovery or ICMP: (1) they are not claimed, they
  reach the stack in the same interrupt; (2) the claimed frames never exceed the ring (32
  buffers), a full ring drops in the interrupt instead of taking more of the pool, and the build
  keeps 16 buffers beyond the DMA ring and the app's ring for the stack; (3) the consumer runs at
  priority 10 on CPU 1 under the network threads (8) and the system work queue (7): they preempt
  it. What is not protected: the MAC's DMA ring is one FIFO, so when the interrupt cannot keep up
  and the descriptors run out, the MAC drops whatever arrives, ARP included. Stay below the rate
  where ``MAC missed frames`` rises.
* A stream at 600 Mb/s only goes from this PC to this board: unicast to the board's MAC, so a
  switch forwards it to the board's port only, once it knows the MAC (the board has sent something:
  ping it first; an unknown unicast address is flooded to all ports of a switch until learned).
  Do not point it at a router, a Wi-Fi bridge or a link shared with others, and do not run it
  through a link slower than 1 Gb/s.
