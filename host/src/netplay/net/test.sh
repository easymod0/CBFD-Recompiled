#!/bin/sh
# Tests the netplay transport from a build of this folder (CMakeLists.txt): the packets' and join codes'
# tests, then matches between copies of netplay_probe over loopback (2 players, joining by code; 3 with
# 20% of packets lost, which also tests the host passing inputs on), joiners with another ROM or other
# mods, which must be refused, joiners sending Inputs early or far ahead, which must be dropped, and one
# that never says Hello, which must be dropped too.
#   sh host/src/netplay/net/test.sh <build folder>
set -u
bin=$1
probe=$bin/netplay_probe
failed=0

"$bin/packets_test" || failed=1
"$bin/join_code_test" || failed=1

# match <name> <port> <players> [probe options...]: a host and players - 1 joiners; all must succeed.
# The joiners join by the host's code if $by_code is set, else by address.
match() {
    name=$1; port=$2; players=$3; shift 3
    to=127.0.0.1:$port
    [ -n "${by_code:-}" ] && to=$("$probe" code "$to")
    "$probe" host "$port" --players "$players" "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    sleep 1
    i=1
    while [ $i -lt "$players" ]; do
        "$probe" join "$to" "$@" > "$name.join$i.txt" 2>&1 &
        pids="$pids $!"
        sleep 0.5
        i=$((i + 1))
    done
    ok=1
    for pid in $pids; do
        wait "$pid" || ok=0
    done
    for f in "$name".*.txt; do
        echo "-- $f"
        grep -v '^\[netplay\]' "$f"
    done
    if [ $ok -eq 1 ]; then echo "$name: passed"; else echo "$name: FAILED"; failed=1; fi
}

by_code=1 match two 27200 2 --reads 150
match three-lossy 27201 3 --reads 150 --loss 20

# Another ROM, or other mods: refused, with the reason.
refused() {
    name=$1; port=$2; option=$3; reason=$4
    "$probe" host "$port" --reads 30 > "$name.host.txt" 2>&1 &
    host=$!
    sleep 1
    "$probe" join "127.0.0.1:$port" --reads 30 $option 2 > "$name.join.txt" 2>&1
    if grep -q "ended: $reason" "$name.join.txt"; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
refused refused-rom 27202 --rom "a different ROM"
refused refused-mods 27204 --mods "different mods"

# A hostile joiner: Inputs before the start (it joins a lobby of 3 alone, so the match hasn't started), or
# far beyond what the game could have reached (in a started match of 2). The host must drop it as broken
# (the joiner hears why) and keep running.
hostile() {
    name=$1; port=$2; players=$3; mode=$4
    "$probe" host "$port" --players "$players" --reads 600 > "$name.host.txt" 2>&1 &
    host=$!
    sleep 1
    "$probe" join "127.0.0.1:$port" --reads 600 --hostile "$mode" > "$name.join.txt" 2>&1
    if grep -q "ended: an unexpected or broken packet" "$name.join.txt" && kill -0 $host 2>/dev/null; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
hostile hostile-early 27205 3 early
hostile hostile-far 27206 2 far

# A joiner that never says Hello: dropped after the handshake time (10 s).
"$probe" host 27203 --reads 30 > silent.host.txt 2>&1 &
host=$!
sleep 1
if "$probe" join 127.0.0.1:27203 --silent > silent.join.txt 2>&1; then
    echo "silent: passed"
else
    echo "silent: FAILED"
    failed=1
fi
grep -v '^\[netplay\]' silent.join.txt
kill $host 2>/dev/null

[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
