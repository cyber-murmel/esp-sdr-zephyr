#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: esdr-update.sh <usb serial> [zephyr.signed.bin]
# DFU-update one running rx_stream board (download retried once), wait for the
# new image and confirm it over the shell so MCUboot keeps it. Needs dfu-util
# and pyserial. Default image: build/rx_stream/zephyr/zephyr.signed.bin.
set -u
[ $# -ge 1 ] || { sed -n 3,6p "$0"; exit 2; }
sn=$1; img=${2:-build/rx_stream/zephyr/zephyr.signed.bin}
[ -f "$img" ] || { echo "no image $img"; exit 2; }
port=$(for d in /sys/bus/usb/devices/*-*; do [ -f $d/serial ] && [ "$(cat $d/serial)" = "$sn" ] && echo ${d##*/}; done | head -1)
[ -n "$port" ] || { echo "$sn: not on USB"; exit 1; }
timeout 15 dfu-util -S $sn -d 2fe3:0005 -e >/dev/null 2>&1
ok=0; for i in $(seq 75); do [ "$(cat /sys/bus/usb/devices/$port/idProduct 2>/dev/null)" = ffff ] && { ok=1; break; }; sleep 0.2; done
[ $ok = 1 ] || { echo "$sn: no DFU mode"; exit 1; }
for try in 1 2; do
  timeout 60 dfu-util -d 2fe3:ffff -p $port -a slot1_image -D "$img" 2>&1 | grep -q 'Download done' || echo "$sn: download $try failed"
  back=0; for i in $(seq 35); do sleep 1; [ "$(cat /sys/bus/usb/devices/$port/idProduct 2>/dev/null)" = 0005 ] && ls /dev/serial/by-id/ 2>/dev/null | grep -q "RX_stream_${sn}-if00" && { back=1; break; }; done
  [ $back = 1 ] && break
done
sleep 1
timeout 20 python3 - "$sn" <<'PY'
import re, serial, sys, time
s = serial.Serial(f"/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_RX_stream_{sys.argv[1]}-if00", 115200, timeout=0.3)
time.sleep(0.3); s.read(65536)
s.write(b"mcuboot confirm\r\n"); time.sleep(0.5); s.write(b"mcuboot\r\n"); time.sleep(1.0)
out = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", s.read(65536).decode(errors="replace"))
conf = re.search(r"^confirmed: *(\d)", out, re.M); size = re.search(r"image size: *(\d+)", out)
print(sys.argv[1], "confirmed", conf.group(1) if conf else "?", "image size", size.group(1) if size else "?")
PY
