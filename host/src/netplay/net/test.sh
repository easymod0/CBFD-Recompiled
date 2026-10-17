#!/bin/sh
# Tests the netplay transport from a build of this folder (CMakeLists.txt): the packets' and join codes'
# tests, then matches between copies of netplay_probe over loopback (2 players; 3 with 20% of packets
# lost, which also tests the host passing inputs on), joiners with another match's key, another ROM or
# other mods, which must be refused, joiners sending Inputs early or far ahead or a Hash twice, which
# must be dropped, one that never says Hello, which must be dropped too, and one sending a message bigger
# than any packet, which the library must close.
#   sh host/src/netplay/net/test.sh <build folder>
set -u
bin=$1
probe=$bin/netplay_probe
failed=0

"$bin/packets_test" || failed=1
"$bin/join_code_test" || failed=1

# The join code a host printed in <file> (its key is random), once it has.
code_of() {
    i=0
    while [ $i -lt 50 ]; do
        code=$(sed -n 's/^join code //p' "$1")
        [ -n "$code" ] && { echo "$code"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}

# match <name> <port> <players> [probe options...]: a host and players - 1 joiners, by its code; all
# must succeed.
match() {
    name=$1; port=$2; players=$3; shift 3
    "$probe" host "$port" --players "$players" "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    to=$(code_of "$name.host.txt")
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

match two 27200 2 --reads 150 --own-view 1
# The host's own-view rule reaches the joiner with Start.
if grep -q "started (own view)" two.join1.txt; then echo "own view: passed"; else echo "own view: FAILED"; failed=1; fi
match three-lossy 27201 3 --reads 150 --loss 20

# Another match's key (a code for this address with another key), another ROM, or other mods: refused,
# with the reason (a wrong key's alone, as nothing else is checked).
refused() {
    name=$1; port=$2; option=$3; reason=$4
    "$probe" host "$port" --reads 30 > "$name.host.txt" 2>&1 &
    host=$!
    to=$(code_of "$name.host.txt")
    [ "$option" = "--key" ] && to=$("$probe" code "127.0.0.1:$port" 1234567890ABCDEF) && option=
    "$probe" join "$to" --reads 30 ${option:+$option 2} > "$name.join.txt" 2>&1
    if grep -q "ended: $reason" "$name.join.txt"; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
refused refused-key 27207 --key "not this match's join code"
refused refused-rom 27202 --rom "a different ROM"
refused refused-mods 27204 --mods "mods on"

# A hostile joiner: Inputs before the start (it joins a lobby of 3 alone, so the match hasn't started),
# far beyond what the game could have reached, or the same Hash twice (in a started match of 2). The host
# must drop it as broken (the joiner hears why) and keep running.
hostile() {
    name=$1; port=$2; players=$3; mode=$4
    "$probe" host "$port" --players "$players" --reads 600 > "$name.host.txt" 2>&1 &
    host=$!
    to=$(code_of "$name.host.txt")
    "$probe" join "$to" --reads 600 --hostile "$mode" > "$name.join.txt" 2>&1
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
hostile hostile-hash 27208 2 hash

# A joiner that never says Hello: dropped after the handshake time (3 s). One that sends a message bigger
# than any packet: closed by the library at once.
raw() {
    name=$1; port=$2; option=$3
    "$probe" host "$port" --reads 30 > "$name.host.txt" 2>&1 &
    host=$!
    code_of "$name.host.txt" > /dev/null
    if "$probe" join "127.0.0.1:$port" "$option" > "$name.join.txt" 2>&1; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        failed=1
    fi
    grep -v '^\[netplay\]' "$name.join.txt"
    kill $host 2>/dev/null
}
raw silent 27203 --silent
raw oversize 27209 --oversize

[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
