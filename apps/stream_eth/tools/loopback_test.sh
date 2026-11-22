#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# vrt_udp against vrt_fake on 127.0.0.1: a clean stream (exit 0, an unbroken ramp), packets lost on
# the network (exit 1, zeros where they were, the file as long as the time), samples dropped by
# the board (exit 1). Usage: loopback_test.sh <vrt_udp> <vrt_fake>   (nothing leaves the machine)
set -u
UDP=$1
FAKE=$2
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
fail=0

run() { # name fake-args udp-args expected-exit
	local name=$1 fargs=$2 uargs=$3 want=$4 port=$((20000 + RANDOM % 20000))
	"$FAKE" -p "$port" $fargs 2>"$dir/$name.fake.log" &
	local fp=$!
	sleep 0.3
	timeout 40 "$UDP" 127.0.0.1 -p "$port" -o "$( [ "$name" = diskfull ] && echo /dev/full || echo "$dir/$name.cs8")" -b 16 $uargs 2>"$dir/$name.log"
	local got=$?
	kill "$fp" 2>/dev/null; wait "$fp" 2>/dev/null
	if [ "$got" != "$want" ]; then
		echo "FAIL $name: exit $got, wanted $want"; tail -5 "$dir/$name.log"; fail=1
	else
		echo "ok   $name: exit $got"
	fi
}

run clean "-t 8 -r 2000000" "-t 2.5" 0
run netloss "-t 8 -r 2000000 -d 40" "-t 2.5" 1
run boardloss "-t 8 -r 2000000 -l 60" "-t 2.5" 1
# the settings: a command, its acknowledge, a new rate and a new time axis, then the clean capture
run settings "-t 12 -r 1000000" "-r 1000000 -f 2412000000 -g auto -t 2.5" 0
# the first command is lost: the tool sends it again
run lostcmd "-t 12 -r 1000000 -k 1" "-r 1000000 -f 2412000000 -t 2.5" 0
# the VRTK is lost: the first context packet or data says the board has us
run lostk "-t 12 -r 1000000 -K 1" "-r 1000000 -f 2412000000 -t 2.5" 0
# a new subscription right after the stop of the last one, from the same host, with other settings
(
	port=$((20000 + RANDOM % 20000))
	"$FAKE" -p "$port" -t 20 -r 2000000 2>"$dir/resub.fake.log" &
	fp=$!
	sleep 0.3
	timeout 30 "$UDP" 127.0.0.1 -p "$port" -r 2000000 -t 1.5 -o "$dir/resub1.cs8" -b 16 2>"$dir/resub1.log"; r1=$?
	timeout 30 "$UDP" 127.0.0.1 -p "$port" -r 1000000 -t 2 -o "$dir/resub2.cs8" -b 16 2>"$dir/resub2.log"; r2=$?
	kill "$fp" 2>/dev/null; wait "$fp" 2>/dev/null
	if [ "$r1$r2" = 00 ]; then echo "ok   resub: both runs exit 0"; else echo "FAIL resub: $r1 $r2"; tail -3 "$dir/resub2.log"; exit 1; fi
) || fail=1
# a dead board: nothing for 5 s, the tool ends with an error and does not hang
# the board refuses the settings: the tool says so and exits 4, it does not carry on as if executed
run refused "-t 12 -r 1000000 -x" "-r 1000000 -f 2412000000 -t 2" 4
# a dead board:
run dead "-t 20 -r 2000000 -D 2" "-t 30" 1
# a board that reboots: the cookie is gone, the tool subscribes again, the time axis restarts: a loss
run reboot "-t 20 -r 2000000 -R 2" "-t 6" 1
# the file system is full
if [ -w /dev/full ]; then
	run diskfull "-t 8 -r 2000000" "-t 2.5" 1
fi
python3 -I - "$dir" <<'PY' || fail=1
import sys
d = sys.argv[1]
clean = open(d + "/clean.cs8", "rb").read()
n = len(clean)
bad = sum(1 for k in range(1, n) if (clean[k] - clean[k - 1]) & 255 != 1)
# 2.5 s at 2 MS/s of 2 bytes: the file is as long as the time
long_enough = 0.9 * 2e6 * 2.5 * 2 <= n <= 1.2 * 2e6 * 2.6 * 2
print(f"{'ok  ' if long_enough and bad == 0 else 'FAIL'} clean ramp: {n} bytes, {bad} breaks, length {'right' if long_enough else 'WRONG'}")
if not (long_enough and bad == 0):
    sys.exit(1)
# lost packets leave zeros of the packet's length, the file keeps the time: one packet is 1440 bytes
for name, every in (("netloss", 40), ("boardloss", 60)):
    b = open(f"{d}/{name}.cs8", "rb").read()
    zeros = b.count(0)
    runs = 0
    k = 0
    while True:
        k = b.find(bytes(1440), k)
        if k < 0:
            break
        runs += 1
        k += 1440
    # 2.5 s at 2 MS/s in 720 pair packets is about 6900 packets: every 40th is about 173, every 60th about 115
    want = 6944 // every
    ok = len(b) > 1000000 and 0.8 * want <= runs <= 1.2 * want
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {len(b)} bytes, {runs} packets of zeros (every {every}th lost, want about {want})")
    if not ok:
        sys.exit(1)
PY
exit $fail
