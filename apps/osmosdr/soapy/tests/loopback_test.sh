#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The esdr module's VRT link against vrt_sim on 127.0.0.1 / ::1 (nothing leaves the machine).
# Usage: loopback_test.sh <vrt_sim> <soapy_capture> <dir with libEsdrSupport.so>
# Needs SoapySDR's libraries on the path (nix-shell -p soapysdr). Prints ok/FAIL per check.
# ONLY=<case> runs one case; VRTHOST=<host> is the address in the device string (default 127.0.0.1).
set -u
SIM=$1
CAP=$2
export SOAPY_SDR_PLUGIN_PATH=$3
fail=0
skip=0
simlog=$(mktemp)
trap 'rm -f "$simlog"' EXIT

# run <name> <sim args> -- <capture args>: module output in $out, what the simulator did in $simlog
run() {
	local name=$1 port=$((${PORTBASE:-20000} + RANDOM % 5000)) sim=()
	shift
	while [ "$1" != "--" ]; do sim+=("$1"); shift; done
	shift
	cur=$name
	skip=0
	if [ -n "${ONLY:-}" ] && [ "$ONLY" != "$name" ]; then skip=1; return; fi
	"$SIM" -p "$port" "${sim[@]}" 2>"$simlog" &
	local sp=$!
	sleep 0.3
	out=$("$CAP" "driver=esdr,vrt=${VRTHOST:-127.0.0.1},vrt_port=$port${DEVX:-}" "$@" 2>&1)
	kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
}
# expect <regex>: the module's output of the last run must match
expect() {
	[ "$skip" = 1 ] && return
	if echo "$out" | grep -Eq "$1"; then echo "ok   $cur: $1"; else echo "FAIL $cur: $1"; echo "$out"; fail=1; fi
}
# expect_not <regex>: it must not
expect_not() {
	[ "$skip" = 1 ] && return
	if echo "$out" | grep -Eq "$1"; then echo "FAIL $cur: not $1"; echo "$out"; fail=1; else echo "ok   $cur: not $1"; fi
}
# sim <regex>: the simulator's log (what it was tuned to, what it received) must match
sim() {
	[ "$skip" = 1 ] && return
	if grep -Eq "$1" "$simlog"; then echo "ok   $cur: sim: $1"; else echo "FAIL $cur: sim: $1"; cat "$simlog"; echo "$out"; fail=1; fi
}
# sim_count <regex> <n>: the simulator's log has exactly n lines matching
sim_count() {
	[ "$skip" = 1 ] && return
	local n
	n=$(grep -Ec "$1" "$simlog")
	if [ "$n" = "$2" ]; then echo "ok   $cur: sim: $2 x $1"; else echo "FAIL $cur: sim: $n x $1, wanted $2"; cat "$simlog"; fail=1; fi
}
# sim_le <regex> <n>: at most n lines
sim_le() {
	[ "$skip" = 1 ] && return
	local n
	n=$(grep -Ec "$1" "$simlog")
	if [ "$n" -le "$2" ]; then echo "ok   $cur: sim: $n x $1 (at most $2)"; else echo "FAIL $cur: sim: $n x $1, at most $2"; cat "$simlog"; fail=1; fi
}
# sim_last <regex> <regex>: the last line matching the first also matches the second
sim_last() {
	[ "$skip" = 1 ] && return
	if grep -E "$1" "$simlog" | tail -n 1 | grep -Eq "$2"; then echo "ok   $cur: sim: last $1 is $2"; else echo "FAIL $cur: sim: last $1 is not $2"; cat "$simlog"; echo "$out"; fail=1; fi
}
# sim_not <regex>
sim_not() {
	[ "$skip" = 1 ] && return
	if grep -Eq "$1" "$simlog"; then echo "FAIL $cur: sim: not $1"; cat "$simlog"; fail=1; else echo "ok   $cur: sim: not $1"; fi
}
CLEAN='timeouts 0, END_BURST 0, zero samples 0, breaks 0'

run clean20 -t 15 -- -r 20e6 -t 3 -s
expect "cycle 0: [0-9]+ samples in [0-9.]+ s = (19\\.[5-9]|20\\.[0-4])[0-9]* MS/s, $CLEAN"
expect 'sensor vrt_link_up = 1'
run clean40 -t 15 -- -r 40e6 -t 3
expect "cycle 0: .* = (39\\.[5-9]|40\\.[0-4])[0-9]* MS/s, $CLEAN"
expect 'kernel dropped 0'
run gap -t 15 -g 1:100 -- -r 20e6 -t 3
expect 'zero samples (7199[0-9]|7200[0-9]), breaks 0'
expect 'lost [0-9]+ packets / 72000 samples'
run biggap -t 15 -g 1:2000 -- -r 40e6 -t 3
expect 'END_BURST 1, zero samples 0, breaks 1'
run restarts -t 15 -- -r 10e6 -t 1 -c 3
expect "cycle 2: [0-9]+ samples .* $CLEAN"
run switch -t 15 -- -r 20e6 -t 4 -R 8e6 -T 2450e6
expect 'switched at .* rate 8000000, freq 2450000000'
expect 'END_BURST 1'
sim 'run: freq 2450000000 rate 8000000'
run dies -t 2 -- -r 20e6 -t 8
expect 'link lost \(renewals go on\)'
expect 'timeouts [1-9]'
run refuse -t 6 -- -r 3e6 -f 5000e6 -t 1
expect '3200000 S/s used'
expect 'outside the board'
sim_not 'freq 5000000000'
run reboot -t 30 -z 1.5:6 -- -r 8e6 -f 2450e6 -t 11
expect 'link lost \(renewals go on\)'
expect 'END_BURST [1-9]'
# the board came back with its defaults: the settings were sent again, and the stream returned at them
sim_count 'run: freq 2450000000 rate 8000000' 2
sim_count 'subscriber on port' 2
run busy -t 8 -b -- -r 20e6 -t 1
expect 'another host has the stream \(VRTB\); add vrt_takeover=1'
DEVX=,vrt_takeover=1 run takeover -r 8000000 -t 10 -b -- -r 8e6 -t 2
DEVX=
expect "cycle 0: [0-9]+ samples in .* = 8\\.0[0-9]* MS/s, $CLEAN"
sim 'takeover on port'
run stolen -r 8000000 -t 15 -o 1.5 -- -r 8e6 -t 8 -s
expect 'another host has the board.s stream \(VRTB\)'
expect 'link lost \(renewals go on\)'
expect 'sensor vrt_link_up = 0'
run noop -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2412e6 -R 8e6 -c 2
expect "cycle 0: [0-9]+ samples .* $CLEAN"
expect "cycle 1: [0-9]+ samples .* $CLEAN"
sim_count 'run: ' 2
run refused -r 8000000 -t 20 -m 2500e6 -- -r 8e6 -f 2412e6 -t 3 -T 2800e6 -c 2
expect 'refused, staying at 2412000000 Hz'
expect 'cycle 1: [0-9]+ samples .* timeouts 0, END_BURST 0'
sim_not 'freq 2800000000'
run refusedfirst -r 8000000 -t 12 -m 2500e6 -- -r 8e6 -f 2800e6 -t 1 -K -c 2
expect 'frequency: out of range'
expect 'sent: 2800000000 Hz gain -1 8000000 S/s'
# the module's own settings are what the board has again, and the second activation works
expect 'cycle 0: activation failed; now rate 8000000, freq 2412000000'
expect "cycle 1: [0-9]+ samples .* $CLEAN"
sim 'run: freq 2412000000'
sim_not 'freq 2800000000'
# contexts that carry another epoch than the board acknowledged: never in force
run wrongepoch -r 8000000 -t 12 -E 7 -- -r 8e6 -t 1
expect 'context does not show the settings epoch acknowledged'
run retunef -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2462e6
expect 'switched at .* freq 2462000000'
expect 'timeouts 0, END_BURST 0'
sim 'run: freq 2462000000'
# small retunes are sent (the context's tolerance never swallows them)
for d in 1000 50000 100000; do
	run "retune$d" -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T $((2412000000 + d))
	expect 'timeouts 0, END_BURST 0, zero samples ([0-9]{1,5}|[0-3][0-9]{5}), breaks 0'
	sim "run: freq $((2412000000 + d)) "
done
# the board rounds to a kHz plan: +300 Hz is a change of the request and a new run (as radio.c now)
run retune300 -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2412000300
expect 'timeouts 0, END_BURST 0, zero samples ([0-9]{1,5}|[0-3][0-9]{5}), breaks 0'
sim 'run: freq 2412000300 '
# or no new run (-k): accepted, the epoch does not move, nothing is lost
run retune300k -k -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2412000300
expect "$CLEAN"
sim_count 'run: ' 1
# a set during the activation is not lost, before and after the command
run actset1 -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 3 -X 2437e6 -Y 20
expect "cycle 0: .*timeouts 0"
sim_count 'run: freq 2437000000' 1
run actset2 -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 3 -X 2437e6 -Y 150
expect "cycle 0: .*timeouts 0"
sim 'run: freq 2437000000'
# a no-op set loses nothing, a real retune the ring restart (30 ms: 240000 samples at 8 MS/s) and no more
run noop2 -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2412e6 -R 8e6
expect "$CLEAN"
sim_count 'run: ' 1
run retune30 -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2462e6
expect 'zero samples ((2[0-9]|3[0-9])[0-9]{4}), breaks 0'
# a drag: 200 sets in a second reach the board as at most 20 commands a second
run drag -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -Z
expect 'storm: [0-9]+ sets'
sim_le 'change accepted' 26
# the stream is taken while sets come in: no call waits longer than about half a second
run stormsteal -r 8000000 -o 1.8 -t 20 -- -r 8e6 -f 2412e6 -t 5 -Z
expect 'max set call ([0-9]|[1-9][0-9]|[1-5][0-9][0-9]) ms'
run aba -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 6 -T 2412050000 -B 2412e6
expect 'timeouts 0, END_BURST 0'
expect '= (7\.[5-9]|8\.0)[0-9]* MS/s'
sim_count 'run: freq 2412000000' 2
sim_count 'run: freq 2412050000' 1
run retuner -t 20 -- -r 20e6 -f 2412e6 -t 4 -R 40e6
expect 'END_BURST 1'
sim 'run: freq 2412000000 rate 40000000'
run lostcmd -r 8000000 -t 20 -C 1 -- -r 8e6 -f 2412e6 -t 4 -T 2462e6
expect 'timeouts 0, END_BURST 0'
sim_count 'command lost' 1
sim 'run: freq 2462000000'
run lostack -r 8000000 -t 20 -A 1 -- -r 8e6 -f 2412e6 -t 4 -T 2462e6
expect 'timeouts 0, END_BURST 0'
sim_count 'ack lost' 1
sim_count 'run: freq 2462000000' 1
run lostrefusal -r 8000000 -t 20 -m 2500e6 -N 1 -- -r 8e6 -f 2412e6 -t 4 -T 2800e6
expect 'refused the settings'
# the settings are not known for the moment of the retry (good unknown: samples do not count): milliseconds, not seconds
expect "timeouts 0, END_BURST 0, zero samples [0-9]{1,5}, breaks 0"
expect 'freq 2412000000'
sim_count 'refusal lost' 1
sim_not 'freq 2800000000'
# every try of the refusal lost: the resend's refusal rolls the settings back, the stream is not left dark
run lostrefusal3 -r 8000000 -t 20 -m 2500e6 -N 3 -- -r 8e6 -f 2412e6 -t 6 -T 2800e6 -s
expect 'refused the settings'
expect '= [5-8]\.[0-9]+ MS/s'
sim_count 'refusal lost' 3
sim_not 'freq 2800000000'
expect 'sensor vrt_link_up = 1'
VRTHOST='[::1]' run ipv6 -r 8000000 -6 -t 15 -- -r 8e6 -t 3
VRTHOST=
expect 'vrt using IPv6'
expect "cycle 0: .* = (7\\.[5-9]|8\\.0)[0-9]* MS/s, $CLEAN"
# name with ::1 first and a board on IPv4 only: the next address is tried
VRTHOST=localhost run fallback -r 8000000 -t 15 -- -r 8e6 -t 2
VRTHOST=
expect 'vrt using IPv4'
expect "cycle 0: .* = (7\\.[5-9]|8\\.0)[0-9]* MS/s, $CLEAN"
run junk -r 8000000 -j -t 15 -- -r 8e6 -t 3 -s
expect "cycle 0: .* = (7\\.[5-9]|8\\.0)[0-9]* MS/s, $CLEAN"
expect 'sensor vrt_bad_packets = [1-9]'
# a frequency the board accepts but cannot tune: it streams nothing; 5 s on (the link is gone) the
# next setting still goes out
run silentfreq -r 8000000 -S 2437000000 -t 30 -- -r 8e6 -f 2412e6 -t 3 -T 2437e6 -W 7 -B 2462e6
sim 'accepted, no stream'
sim 'run: freq 2462000000'
# an executed command whose acknowledge is lost, then back to the old value: the board must not stay at the new one
run lostackback -r 8000000 -U 1 -A 2 -t 20 -- -r 8e6 -f 2412e6 -t 4 -T 2462e6 -W 0.25 -B 2412e6
sim_last 'run: freq' 'freq 2412000000 '
# manual gain 40, automatic, activate, manual again: the 40 is sent
run gainmode -r 8000000 -t 20 -- -r 8e6 -f 2412e6 -t 4 -M
sim_last 'run: freq' 'gain 40'
run nobody -t 0.1 -- -r 8e6 -t 1
expect 'no answer from'
exit $fail
