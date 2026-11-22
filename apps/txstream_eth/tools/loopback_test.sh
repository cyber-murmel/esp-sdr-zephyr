#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# vrt_tx against tx_fake on 127.0.0.1 (nothing leaves the machine): a clean run at 20 and 40 MS/s
# in both formats (exit 0, every payload right), a file and stdin source, GSO, the amplitude
# refusal, RF refused by the board, RF refused three times and then accepted, packets lost (exit 1), a stall of the board (exit 1, the stream
# recovers and the payloads stay right), a dead board, a second host.
# Usage: loopback_test.sh <vrt_tx> <tx_fake>
set -u
TX=$1
FAKE=$2
dir=$(mktemp -d)
trap 'rm -rf "$dir"; kill $(jobs -p) 2>/dev/null' EXIT
fail=0

# run name fake-args tx-args want-tx-exit want-fake-exit(or - for any) [grep pattern of the fake log]
run() {
	local name=$1 fargs=$2 targs=$3 want=$4 wantf=$5 pat=${6:-} sin=${7:-/dev/null} port=$((20000 + RANDOM % 20000))
	local attempt=0 got gotf fp
	while :; do
		attempt=$((attempt + 1)); port=$((20000 + RANDOM % 20000))
		"$FAKE" -p "$port" -d $((port + 1)) -t 30 $fargs 2>"$dir/$name.fake.log" &
		fp=$!
		sleep 0.3
		timeout 40 "$TX" --to 127.0.0.1 --port "$port" --dport $((port + 1)) $targs <"$sin" 2>"$dir/$name.log"
		got=$?
		wait "$fp"; gotf=$?
		# a plain (non real time) scheduler may stall the sender for a few ms now and then: a run that
		# should be clean gets up to 3 tries; the protocol failures are not retried
		if [ "$want" = 0 ] && [ "$got" = 1 ] && [ "$attempt" -lt 3 ]; then
			echo "     $name: attempt $attempt not clean ($(grep -h 'NOT CLEAN' "$dir/$name.log")), again"
			continue
		fi
		break
	done
	if [ "$got" != "$want" ]; then
		echo "FAIL $name: vrt_tx exit $got, wanted $want"; tail -4 "$dir/$name.log"; fail=1
	elif [ "$wantf" != "-" ] && [ "$gotf" != "$wantf" ]; then
		echo "FAIL $name: tx_fake exit $gotf, wanted $wantf"; tail -3 "$dir/$name.fake.log"; fail=1
	elif [ -n "$pat" ] && ! grep -Eq "$pat" "$dir/$name.fake.log"; then
		echo "FAIL $name: no '$pat' in the fake's log"; tail -3 "$dir/$name.fake.log"; fail=1
	else
		echo "ok   $name (try $attempt): vrt_tx $got, tx_fake $gotf; $(tail -1 "$dir/$name.fake.log" | cut -c1-110)"
	fi
}

OKRUN='0 bad headers, 0 wrong payloads'
run iq8-20 "-T 100:1" "--rate 20 --fmt iq8 --source tone:100:1 --seconds 3" 0 0 "$OKRUN"
run w32-20 "-T -250:2" "--rate 20 --fmt w32 --source tone:-250:2 --seconds 3 --allow-amp" 0 0 "$OKRUN"
run iq8-40 "-T 1000:1" "--rate 40 --fmt iq8 --source tone:1000:1 --seconds 3" 0 0 "$OKRUN"
# w32 at 40 MS/s is 1.3 Gb/s on the wire: refused, unless the cap is lifted (the loopback has no wire)
"$TX" --to 127.0.0.1 --rate 40 --fmt w32 --source tone:3000 --seconds 1 2>/dev/null; r=$?
if [ "$r" = 2 ]; then echo "ok   w32 at 40 MS/s over the line cap refused: exit 2"; else echo "FAIL w32-40 cap: exit $r"; fail=1; fi
run w32-40 "-T 3000:1" "--rate 40 --fmt w32 --source tone:3000 --seconds 3 --max-mbps 3000" 0 0 "$OKRUN"
run gso "-T 100:1" "--rate 20 --fmt iq8 --source tone:100 --seconds 2 --gso" 0 0 "$OKRUN"
run verify-flag "-T 100:1" "--rate 20 --fmt iq8 --source tone:100 --seconds 2 --verify" 0 0 "$OKRUN"
# a file of 1 s of ramp, looped for 2 s (the fake does not check the payload: exit codes only)
python3 -I -c "import sys; sys.stdout.buffer.write(bytes((k % 5) - 2 & 255 for k in range(2 * 20000000)))" > "$dir/ramp.iq8"
run file-loop "" "--rate 20 --fmt iq8 --source file:$dir/ramp.iq8 --seconds 2 --loop" 0 0
run file-once "" "--rate 20 --fmt iq8 --source file:$dir/ramp.iq8" 0 0
run stdin "" "--rate 20 --fmt iq8 --source stdin --allow-amp" 0 0 "" "$dir/ramp.iq8"
# the amplitude guard: refused before any packet leaves
"$TX" --to 127.0.0.1 --rate 20 --fmt iq8 --source tone:100:5 --seconds 1 2>/dev/null; r=$?
if [ "$r" = 2 ]; then echo "ok   amp 5 refused: exit 2"; else echo "FAIL amp 5: exit $r"; fail=1; fi
python3 -I -c "import sys; sys.stdout.buffer.write(bytes([10, 0] * 1000))" > "$dir/loud.iq8"
"$TX" --to 127.0.0.1 --rate 20 --fmt iq8 --source file:"$dir/loud.iq8" 2>/dev/null; r=$?
if [ "$r" = 2 ]; then echo "ok   loud file refused: exit 2"; else echo "FAIL loud file: exit $r"; fail=1; fi
run amp-allowed "-T 100:5" "--rate 20 --fmt iq8 --source tone:100:5 --seconds 2 --allow-amp" 0 0 "$OKRUN"
run amp-maxamp "-T 100:8" "--rate 20 --fmt iq8 --source tone:100:8 --seconds 2 --max-amp 8" 0 0 "$OKRUN"
# the board refuses RF
run rf-refused "" "--rate 20 --fmt iq8 --source tone:100 --seconds 2 --rf --arm-wait 2" 4 -
# RF asked, the board refuses TXGO three times (code 5: not armed), then accepts (`tx arm`): the run that starts
# after that wait is clean like one without it (no resync, no lost word), and the wait leaves no report lines
# ("sent 0 words") behind
run rf-wait "-T 100:1 -R 3" "--rate 20 --fmt iq8 --source tone:100 --seconds 3 --rf --arm-wait 20" 0 0 "$OKRUN"
if grep -q 'waiting for `tx arm' "$dir/rf-wait.log" && ! grep -q 'sent 0 words' "$dir/rf-wait.log"; then
	echo "ok   rf-wait: the wait was announced, no report line for it"
else
	echo "FAIL rf-wait: log"; head -5 "$dir/rf-wait.log"; fail=1
fi
# packets lost on the way: the board counts lost words, vrt_tx says not clean
run loss "-T 100:1 -L 50" "--rate 20 --fmt iq8 --source tone:100 --seconds 3" 1 -
# the board's CPU stalls for 5 ms: underruns, a resync, then clean again; the payloads stay right
run board-stall "-T 100:1 -S 1:5" "--rate 20 --fmt iq8 --source tone:100 --seconds 3" 1 - '0 bad headers, 0 wrong payloads'
# a dead board
"$TX" --to 127.0.0.1 --port 29999 --rate 20 --fmt iq8 --source tone:100 --seconds 1 2>"$dir/dead.log"; r=$?
if [ "$r" = 1 ]; then echo "ok   dead board: exit 1"; else echo "FAIL dead board: exit $r"; fail=1; fi
# a second host while the first runs: VRTB, exit 3 (the first run gets 3 tries, as above)
second_host() {
	local port=$((20000 + RANDOM % 20000)) fp t1 r1 r2
	"$FAKE" -p "$port" -d $((port + 1)) -t 30 -T 100:1 2>"$dir/busy.fake.log" &
	fp=$!
	sleep 0.3
	"$TX" --to 127.0.0.1 --port "$port" --dport $((port + 1)) --rate 20 --fmt iq8 --source tone:100 --seconds 4 -q 2>/dev/null &
	t1=$!
	sleep 1.5
	timeout 20 "$TX" --to 127.0.0.1 --port "$port" --dport $((port + 1)) --rate 20 --fmt iq8 --source tone:100 --seconds 1 2>/dev/null
	r2=$?
	wait "$t1"; r1=$?
	wait "$fp"
	R1=$r1; R2=$r2
}
for try in 1 2 3; do
	second_host
	[ "$R1" = 0 ] && [ "$R2" = 3 ] && break
done
if [ "$R1" = 0 ] && [ "$R2" = 3 ]; then echo "ok   second host (try $try): first $R1, second $R2"; else echo "FAIL second host: $R1 $R2"; fail=1; fi
exit $fail
