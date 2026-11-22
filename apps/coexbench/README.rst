.. SPDX-License-Identifier: GPL-3.0-or-later

#########
coexbench
#########

A bench for the ESP32-S3: the Zephyr Wi-Fi driver (station, DHCP, net and wifi shell), ESP-NOW
and lib/esp_sdr on one radio. It measures what burst captures cost a Wi-Fi link, when the blob
powers the PHY down and up, and how ESP-NOW wake windows behave.

The results and what they mean for the radio arbiter are on the page
`Coexistence <../../doc/radio/coexistence.md>`__.

Build (sysbuild with MCUboot, shell on the OTG port's CDC-ACM, ``west dfu``)::

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/coexbench

``CONFIG_ESP_SDR_ARB`` puts the radio arbiter between the blob and the PHY
(`Coexistence <../../doc/radio/coexistence.md>`__): captures need a claim (``coex sdr``), except the
passive ones of ``coex cap passive``, which need Wi-Fi awake. This standalone bench has no ring: its three dump banks need the
DRAM image below 0x3fcb0000, which Wi-Fi, USB and the net shell do not leave (cutting stacks to
fit double-faulted CPU 1 on association). ``coex sdr`` runs bursts on CPU 1 (SMP, Wi-Fi heap in
PSRAM). Ring windows are for multiapp, where Wi-Fi and the radio are modules.

The library runs with ``CONFIG_ESP_SDR_WIFI_SHARED=y`` (no power save, promiscuous mode or
channel change at init) and starts passive (``esp_sdr_rx_set_passive()``): captures sample what
the MAC and AGC have set up.

Shell
*****

``coex ps none|min|max``
   ``esp_wifi_set_ps()`` (the driver's own op only does MAX).
``coex listen <n>``
   Listen interval in beacons, for the next association with ``max``.
``coex chan``
   The Wi-Fi primary channel.
``coex cap <n> [passive|forced] [interval_ms]``
   n bursts (40 MS/s, 4096 samples) on the Wi-Fi channel. Per capture: power in dBFS, peak, the
   AGC register's index field (S3, unverified as a live value), burst and wall time. Forced
   tunes to the channel and takes over the front end, gain and filter for the run.
``coex sdr <seconds> [ring|burst] [freq_mhz]``
   The SDR in the radio arbiter's windows while Wi-Fi sleeps: claim, run to the deadline (ring
   on CPU 1, or bursts of 4096 samples at 40 MS/s), release, repeat. Prints windows, claims that
   failed (``busy``: Wi-Fi never sleeps, so power save must be ``min`` or ``max``), SDR time and
   duty, samples, ring window ends against failures, index gaps seen by the sink, preemptions and
   deadline misses. The frequency defaults to the Wi-Fi channel.
``coex arb [clear]``
   The arbiter's counters: Wi-Fi enables and disables, the learned wake period and how far the
   enable-to-enable interval came off it, windows, SDR time, preemptions and their waits.
``coex log [clear]``
   The PHY event ring (phy enable and disable, and the channel calls ``phy_change_channel``,
   ``chip_v7_set_chan``, ``set_chanfreq``, with time, arguments, context, CPU, thread, Wi-Fi
   channel), the counts, and the min, mean and max of the off windows and on times.
``espnow init|peer <mac>|send <count> <interval_ms> [bytes]|stats [clear]|window <ms> <interval_ms>``
   ESP-NOW unicast with send callback counts, the receive counters and the wake window.

``wifi connect -s <ssid> -k 1 -p <psk>``, ``net ping <ip>`` and the rest of the net shell work as
in the Zephyr Wi-Fi shell sample.
