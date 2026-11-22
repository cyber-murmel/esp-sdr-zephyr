.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_stream_eth:

##########
stream_eth
##########

.. nav-start

Previous: `osmosdr_hs <../osmosdr_hs/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__

.. nav-end

The ESP32-S31-Function-CoreBoard's gapless receive (the diag16 ring, 8 bit I and Q) as VITA 49.2
signal data over UDP on its gigabit Ethernet. Target 40 MS/s (80 MB/s of samples, 640 Mb/s, about
55.6k packets/s), 20 MS/s on the way, 10 MS/s at least.

**Status (2026-11-25): it runs on the board.** 40 MS/s and 20 MS/s over IPv6 and IPv4 on the SMP
build (the default): 30 s and 20 short sessions without a lost sample, and a control flood of 3000
datagrams/s beside 40 MS/s without one. The ring consumer must really be pinned to its core: before
that, floods cost 45k to 135k samples and one run in about 70 lost 0.6 s (not seen since, no proof
that it had the same cause). Ten minutes at 40 MS/s over IPv6 (before the pinning): no sample
dropped by the board, 0.014 % lost on the way to a PC under memory pressure. The ramfunc,
coalescing and product options made one core worse. Figures marked *estimate* are arithmetic.
Refused settings, busy, takeover and ``stream stop``/``start`` are tried on the board; a second MAC
on one IP is not. **CONFIG_FPU_SHARING=y is needed** (the command handling uses floating
point; without it gain commands were refused at random when another thread had the FPU).

Why a new app, and not ``stream``
*********************************

``apps/stream`` is an S3 app (Wi-Fi capture bursts, CDC-NCM, the DAC), and the S3 build must stay
byte for byte as it is. Nothing here changes it. ``stream_eth`` shares nothing at build time: its
command handling is ``stream``'s (libvrt, the same acknowledges and error words), its radio setup
is ``osmosdr_hs``'s (frequency with a kHz offset, gain, bandwidth, the ring), written again against
what this app needs.

Build
*****

.. code-block:: sh

   west build -b esp32s31_function_coreboard/esp32s31/hpcore apps/stream_eth

Plain image, no MCUboot. The overlay turns on ``eth``, ``mdio`` and ``phy`` and turns off ``twai0``
(it shares GPIO13 and 14 with RGMII TX_CLK and RX_CLK). The diag16 pads (GPIO 20 to 25, 36 to 40,
42 to 44, 47, 48) are none of the Ethernet pins (GPIO 4 to 19).

Build options, each a configuration file to add with ``-DEXTRA_CONF_FILE=...`` so that each lever
can be measured on its own:

SMP is the default (``prj.conf``): ring consumer on CPU 0, sender on CPU 1 (S31 SMP is
experimental in Zephyr, 20 times fewer losses than one core here).
``onecore.conf``
  One core, both threads on CPU 0 (the old default).
``ramfunc.conf``
  The sender and the driver's transmit path and interrupt run from internal RAM.
``coalesce.conf``
  A transmit completion interrupt on one frame in eight.
``product.conf``
  RAM functions, coalescing, no network statistics and no net shell, ``NET_TC_TX_COUNT=0``.

Other levers are plain Kconfig: ``CONFIG_ETH_ESP32_DWC_TX_THRESHOLD`` (0 is 64 bytes, 1 128, 2 192,
3 256 in the 256 byte FIFO), ``CONFIG_DWMAC_NB_TX_DESCS`` (up to 128), ``CONFIG_APP_VRT_CELLS``,
``CONFIG_APP_VRT_BATCH``, ``CONFIG_APP_VRT_CLIP_STRIDE``, ``CONFIG_APP_VRT_MTU_CAP``.

Memory: the image has about 256 KiB of internal RAM below the diag16 ring, and this build uses 235
KiB of it. The staging cells (32 of them) are 46 KiB, the receive buffers of the stack 18 KiB. More
cells need less of something else.

How a host gets the stream
**************************

**There is no authentication. For a LAN; do not expose the port.** The board streams only to a
host that has shown that it can receive at its address (a cookie, below), takes datagrams only
from unicast hosts on a connected prefix or link-local (``CONFIG_APP_VRT_ALLOW_OFFLINK`` lifts
that), sent to a unicast address of its own, and gives the stream to one host at a time.

Everything is UDP to port 4992 (IPv6 and IPv4; prefer IPv6). The messages, all in ASCII:

========================  ==================================================================
host to board             board to host
========================  ==================================================================
``VRTS``                  ``VRTC`` + 8 byte cookie (12 bytes). No state is kept.
``VRTS`` + cookie         subscribes or renews: ``VRTK`` the first time (a new subscription),
                          nothing on a renewal, ``VRTB`` when another host has the stream
``VRTT`` + cookie         takes the stream over from another live host (``VRTK``)
``VRTX`` + cookie         stops (from the subscriber only), no answer
a VITA 49.2 command       an acknowledge (see below)
========================  ==================================================================

The cookie is SipHash-2-4 of the source address, port, the source MAC, the address the host sent
to, the interface and a 30 s epoch under a key the board makes from the TRNG at boot (the wire
format is as before; the MAC and the address are the datagram's own, not extra bytes). So the
stream goes only to the MAC that answered with the cookie, and a renewal from another MAC (or
another address of the board) gets a new cookie and moves nothing. What the cookie proves is that
the host could receive at that address and MAC; not who it is. A host that can send with the
subscriber's own MAC, IP and port (and has seen a cookie for it) is, by this, the subscriber: that
is the accepted limit. The current and the previous epoch are valid. A host
renews several times a second (``vrt_udp`` every 0.25 s: in a flood of 3000 datagrams/s the board loses
about 70 % of what it receives; it drops a host silent for 5 s) with the cookie it has; when the board
answers with ``VRTC`` it is stale (an epoch more, a reboot): use the new one. A host that is
silent for ``CONFIG_APP_VRT_SILENT_S`` (5 s) is dropped, then anyone may subscribe. A host
that is not the subscriber cannot change the settings.

The settings are a VITA 49.2 command packet to stream ID ``0x5d5d0001`` with EXECUTE and
``rf_reference_frequency`` in Hz, ``gain`` stage 1 (the PHY's gain index, ``-1`` automatic),
``sample_rate`` from the diag16 list (40, 20, 16, 10, 8, 5, 4, 3.2, 2.5, 2, 1.6, 1.25, 1, 0.8, 0.64
MS/s), payload format 8 bit link-efficient complex. Only a command from the subscriber's address, port and MAC is executed (no cookie travels with a
command: a host able to send from that address and port on the LAN could change the settings). As in
``stream`` they are acknowledged (execution and query-state acknowledges, error words per
field). Any other on-link host may ask for the state (query-state, no settings): two answers a
second to a source, ten in all. Datagrams are budgeted per source (20 a second, 300 in all; the
subscriber has 60 a second of its own, outside the 300), cookie answers 5 a second to a source and
100 in all (the subscriber's refresh is not in the 100), so a flood cannot shut the subscriber out. A command is not a subscription. A cancellation command packet does not stop
the stream: ``VRTX`` + cookie does.

``stream stop`` on the UART stops the stream and refuses subscriptions until ``stream start``.
The board streams to the datagram's source address and port **from its own port 4992**, so a
host's firewall passes the packets as replies, from the address the host sent to. If that
address goes away (a new DHCP lease) the stream stops. An ICMP port unreachable does not stop
it; the 5 s do. No multicast, at stream rate or at any other.

The subscriber's MAC is the source MAC of the datagram that asked: nothing is resolved, a router
in between is the next hop. The cookie covers it, so a renewal from another MAC is not a renewal.
The live subscriber (the full match of address, port, MAC and board address) has counters of its own,
the others are counted per source IP, so a host that shares the subscriber's IP cannot use up its share.

Addresses: DHCPv4, IPv6 link-local and autoconfiguration (5 unicast IPv6 addresses, so a ULA
prefix fits beside the link-local and the global one). The log shows each address as it comes
up. For a link-local address use the interface scope: ``fe80::...%eth0``.

Packets
*******

Every packet has the largest payload the MTU allows in whole 32-bit words. The MTU is the
interface's, at most ``CONFIG_APP_VRT_MTU_CAP`` (1492, what the lab router announces; ``stream mtu
1500`` on a direct cable). No IP fragments: don't-fragment is set on IPv4.

======================  ========  =========  ==========  ===========
MTU                     IP        payload    samples     packets/s at 40 MS/s
======================  ========  =========  ==========  ===========
1492                    IPv4      1440 B     720 pairs   55.6k
1492                    IPv6      1420 B     710 pairs   56.3k
1500                    IPv4      1448 B     724 pairs   55.2k
1500                    IPv6      1428 B     714 pairs   56.0k
======================  ========  =========  ==========  ===========

Packets span nodes: the ring's nodes (4080 samples bytes each) are copied into the staging cells,
one cell a packet, and cut at the packet boundary. Layout of a packet (network order)::

   Ethernet 14 | IPv4 20 or IPv6 40 | UDP 8 | VRT header 20 | I0 Q0 I1 Q1 ... | trailer 4
   VRT header: 0x1<<28 (data with stream ID) | trailer | TSI other(3) | TSF real time(2) | count | size
               stream ID 0x5d5d0001, seconds, picoseconds (two words)
   trailer: valid data, AGC/MGC, over-range, sample loss (enable 31..20, indicator 19..8)

- **Items**: 8 bit link-efficient complex signed, I then Q, which is the diag16 ring's byte order
  (signed I[9:2], then signed Q[9:2]) and the order libvrt gives ``iq_pack`` words on the wire. There is
  no conversion. The unit test compares whole packets with libvrt's encoding.
- **Time**: seconds since boot from the sample index, exactly: the index of a run starts from the
  cycle counter, every later packet's time is index / rate (picoseconds exact, past 2^32 samples
  too). A restart (retune, rate change) starts a new index and says so with sample loss.
- **Count**: modulo 16, one per packet sent. A gap in the count is the network's. When the board drops
  samples (the staging is full, the ring skipped nodes) the count goes on, the time jumps, and the
  next packet has the sample loss indicator (enable bit 24, indicator bit 12 of the word).
- **Over-range**: set when a sample byte is 0x7f or 0x80, looking at one word in
  ``CONFIG_APP_VRT_CLIP_STRIDE`` (4): shorter clipped runs can slip by.
- **Context** (IF context, rate, RF reference frequency, bandwidth, gain, settings epoch, payload
  format): before the first data packet of every run of the ring (also when nothing differs from the
  last one), after a subscription, when the settings of the cells differ, and once a second. A
  context never carries settings its run does not have: it is made from the cell it comes before.
  It is sent between the frames of the old run and the first of the new one.

Context packet, in words (network order), as the board writes it with libvrt::

   0   header: type 4, nd0 (bit 25), TSI other, TSF real time, count mod 16, size (18)
   1   stream ID 0x5d5d0001
   2   seconds           3-4  picoseconds (time of the first sample of the next data packet)
   5   CIF0: bit 31 change indicator, 29 bandwidth, 27 RF reference frequency, 23 gain,
       21 sample rate, 16 state and event indicators, 15 payload format, 1 CIF1 follows
   6   CIF1: bit 3 (spec compliance)
   7-8 bandwidth   9-10 RF reference frequency (Q44.20 Hz)   11 gain (stage 1, Q9.7)
       12-13 sample rate
   14  state and event indicators: **bits 7..0 are the settings epoch** of the run whose data follows,
       everything else 0
   15-16 payload format (0xa00001c7, 0)   17 CIF1 field: spec compliance 4 (VITA 49.2)

(Word numbers are those of the golden packet in ``gnuradio/golden_gen.c``: 18 words in all.) The
**settings epoch** is 8 bits that go up by one at every change of the frequency, the gain or the
rate that is accepted (a command that sets what is set changes nothing; A to B to A is two). Each
run of the ring takes the epoch it runs with when it starts. A command that asks for execution and
for the query-state acknowledge (CAM bits X and S) gets the execution acknowledge and then the
query-state acknowledge, both once the settings are taken (the ring restarts after that); the query-state acknowledge
carries the epoch after the command in the same field. A client counts the data from the first
context whose epoch is the acknowledged one. A refused command names its errors in the execution
acknowledge.

The send path
*************

Threads: the ring consumer (``vrt_radio``, priority 0, 4096 bytes of stack, what ``main`` had
before) and the sender (``vrt_send``, priority 1) are created stopped, pinned to CPU 0 and CPU 1 (SMP;
the return value of every pin is checked and logged) and then started; ``main`` (2048 bytes) only starts
them. The control thread (4096 bytes) follows once the radio is up. Check the stacks with ``kernel stacks``
on the board.

::

   diag16 ring --(sink, CPU 0)--> staging cells --(sender, CPU 1)--> frames --(MAC DMA)--> wire
        copy 4080 B a node            1472 B a cell      template header + cell     one descriptor a frame

1. The ring's sink copies a node into the cells (``vrt_cells.c``), checks it was not overwritten
   (``esp_sdr_diag16_node_valid``) and wakes the sender. If the cells are full the whole node is
   dropped, counted, and the next packet carries the sample loss.
2. The sender thread takes finished cells in order and per packet: copies the 62 or 82 byte header
   template, writes the VRT header, the IPv4 header checksum (IPv6: the UDP checksum over the
   cell, the payload checksum being the largest single cost; IPv4 uses 0; the checksum of the payload is summed by the sink while it copies into the cell), the trailer; then
   ``dwmac_send_frames()`` takes a batch of frames, each **one descriptor with two DMA buffers**
   (header buffer, cell in place: no copy, no ``net_pkt``, no IP stack) and one poll demand.
3. The MAC says when a frame is sent; the cell is the ring consumer's again. The control traffic of
   the stack (ARP, ND, DHCP, ping) goes beside it through its own path; the board's own control
   frames (cookies, acknowledges, context packets) are frames of ``dwmac_send_frames()`` too
   (``txpath_send_ctl()``).

Cycle estimate per packet at 320 MHz (*estimate, not measured*): sink copy 0.64 cycles a byte, 900
cycles a packet; header and checksums 300 (IPv4) or 1800 (IPv6, 3 cycles a byte of checksum); over-range
scan at stride 4 about 700; driver send about 300, completion about 200, interrupt about 1500 a frame
(less with ``coalesce.conf``). About 3 to 4k cycles a packet in all, against 5.76k available per core at 40 MS/s on
one core: so 40 MS/s on one core is not out of reach and certainly not sure, two cores
(the default build) have room. 20 MS/s is half of that.

Changes in the Zephyr fork (modules/zephyr, drivers/ethernet/dwc_mac)
*********************************************************************************

All behaviour neutral for the other chips with the default options.

- ``CONFIG_ETH_DWMAC_TX_FAST``: ``dwmac_send_frames()`` (include/zephyr/drivers/ethernet/eth_dwmac_fast.h):
  frames in one or two buffers of the caller, a completion callback each, a batch behind one poll
  demand, ``dwmac_tx_free()``, ``dwmac_tx_underflows()`` (MMC counter).
- ``dwmac_send()`` claimed the head descriptor outside the lock: with two senders (the stack's TX
  thread and a second thread) frames corrupt each other. The claim and the fill are in the lock now.
  Release runs under the lock too; the semaphore is given after it. This fix is unconditional (with
  ``TX_FAST`` off too) and is a patch of its own, ahead of the fast path.
- With ``TX_FAST``: the transmit underflow interrupt is enabled (DMAIER bit 5, ``UNFIE``); on it the
  driver reaps and polls, and ``dwmac_tx_poll()`` lets the sender poll when frames are queued and
  nothing completes for 100 ms, so that a suspended DMA cannot stay dead. Release also polls when
  frames are queued behind the one it reaped.
- ``CONFIG_ETH_DWMAC_TX_IRQ_EVERY`` (default 1): an interrupt on one frame in N; the frames in
  between are reaped by the next interrupt, and a 1 ms timer for the last of a burst.
- ``CONFIG_ETH_DWMAC_TX_RAMFUNC``: ``dwmac_send``, the batch send, the reaping and the interrupt in RAM.
- ``CONFIG_ETH_ESP32_DWC_TX_THRESHOLD`` (default 0, the old 64 bytes): the TTC field.

Shell (UART console)
********************

``stream status``
  subscriber, MTU and packet size, packets, bytes, context packets, descriptor waits, nodes,
  nodes dropped (staging full), ring skips, MAC TX underflows, CPU free per CPU.
``stream start``
  allows subscriptions again after ``stream stop``.
``stream stop``
  stops the stream and drops the subscriber.
``stream mtu [bytes]``
  the IP packet size the stream may use (576 to 1500).
``stream bench <pkt bytes> <seconds> [rate pkt/s]``
  stops the radio and sends synthetic VRT packets of that size (UDP payload, at most what the MTU
  allows) to the current subscriber (subscribe first, ``VRTS``), through the same builder and MAC path,
  as fast as possible or paced: packets/s, Mb/s on the wire and of samples, cycles per packet
  (build and submit), descriptor waits, late packets (paced), MAC TX underflows.

Host tool
*********

``tools/vrt_udp.c`` (gcc and libvrt's sources, no root)::

   vrt_udp <board address or name> [-r samples/s] [-f Hz] [-g gain|auto] [-t seconds] [-o file|-] [-b MiB] [-p port] [-T] [-q]

IPv6 first (AAAA and link-local before A; the family used is printed). Subscribes with the cookie, renews each second, sends the settings until the board acknowledges (five tries), writes interleaved int8 I,Q to the file or ``-o -`` for stdout,
fills gaps with zeros so time stays continuous (up to 5 s of gap), prints per second status on
stderr and exits non-zero on any loss: counter gaps (network), samples the board flagged or a restart of the
time axis, kernel drops (``SO_RXQ_OVFL``, counted from 0 and separate), write errors, writer overflow, bad
packets, a board silent for 5 s, a capture shorter than -t, settings never acknowledged (4), another host
holding the stream (3, ``-T`` takes over), no stream (5). It ends in 5 s of SIGTERM or SIGINT whatever
the disk or the pipe does. One receive thread (``recvmmsg``, the
largest ``SO_RCVBUF`` the system grants) and one writer thread with a ring.

``-z`` asks for IPv6 packets with a zero UDP checksum (it sets ``UDP_NO_CHECK6_RX`` and the flag byte
after the cookie); the board does it only if built with ``CONFIG_APP_VRT_V6_ZERO_CSUM`` and
answers with the packets as before otherwise (the socket then drops them: do not use -z against a
board without it).

``tools/vrt_udp.py`` is the fallback for a few MS/s (no settings). ``tools/vrt_fake.c`` and
``tools/loopback_test.sh`` test the tool on 127.0.0.1 against a fake board.

Build the tools::

   L=modules/lib/libvrt; T=esp-sdr-zephyr/apps/stream_eth/tools
   nix-shell -p gcc --run "gcc -O2 -Wall -pthread -o build-clanker/host/vrt_udp $T/vrt_udp.c -I$L/include -I$L/src $L/src/vrt_*.c $L/src/timestamp_to_calendar.c"

At 80 MB/s raise ``net.core.rmem_max`` (``sysctl``, root) or the kernel drops datagrams; vrt_udp
tells the buffer it got and counts the drops.

Tests
*****

``tests/unit/vrt_eth``: packet bytes against libvrt, timestamps past 2^32 samples, count and loss
flags through the cells, the MTU split, IPv4 and IPv6 headers and checksums. Run with
``scripts/run-unit-tests.sh``.

On the board (2026-11-25): link, DHCP, the net_context datagram path, the cookie handshake, 20 and 40
MS/s over IPv4 and IPv6, SMP. Not verified there: a second host on the same IP (MAC), a lost
tune, the poll-demand and underflow interrupt path (TX_FAST), RAM function placement, a DHCP lease
change while streaming, the one-core build, an hour-long run. Zero-copy
from the ring (diag16 slab detach) is not used.
