#!/bin/sh
# Tests the netplay transport from a build of this folder (CMakeLists.txt): the packets' tests, then
# matches between copies of netplay_probe over loopback (2 players; 3 with 20% of packets lost, which
# also tests the host passing inputs on), and a joiner with another ROM, which must be refused.
#   sh host/src/netplay/net/test.sh <build folder>
set -u
bin=$1
probe=$bin/netplay_probe
failed=0

"$bin/packets_test" || failed=1

# match <name> <port> <players> [probe options...]: a host and players - 1 joiners; all must succeed.
match() {
    name=$1; port=$2; players=$3; shift 3
    "$probe" host "$port" --players "$players" "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    sleep 1
    i=1
    while [ $i -lt "$players" ]; do
        "$probe" join "127.0.0.1:$port" "$@" > "$name.join$i.txt" 2>&1 &
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

match two 27200 2 --reads 150
match three-lossy 27201 3 --reads 150 --loss 20

# Another ROM: refused, with the reason.
"$probe" host 27202 --reads 30 > refused.host.txt 2>&1 &
host=$!
sleep 1
"$probe" join 127.0.0.1:27202 --reads 30 --rom 2 > refused.join.txt 2>&1
if grep -q "ended: a different ROM" refused.join.txt; then
    echo "refused: passed"
else
    echo "refused: FAILED"
    cat refused.join.txt
    failed=1
fi
kill $host 2>/dev/null

[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
