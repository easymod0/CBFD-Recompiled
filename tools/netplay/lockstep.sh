#!/bin/sh
# The lockstep check (netplay): two copies of the game on this machine play a match over
# loopback, each giving its own port's pads from the same made-up 2-port recording, and must give the
# game exactly what a replay of that recording alone does, read by read. Run it after building with
# CONKER_NETPLAY=ON:
#
#   tools/netplay/lockstep.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                             [--reads 1800] [--seed 2] [--timeout 300] [--out <folder>]
#
# The runs, all headless and --fast, each copy with a data folder of its own (--data):
#   solo       the recording replayed alone
#   match      host and joiner, input delay 2: both hash logs must match solo's. Each is given a copy
#              of the recording with the other's port scrambled, so it only gets the right game if the
#              other's pads come over the network
#   jittery    the same with input delay 0 and random sleeps (--jitter 500) on the joiner, so the two
#              are out of step and wait for each other all the time
#   desync     the host changes a byte of its memory at read 300: both must stop for a desync
# It exits 0 if all is as it should be, 1 if not. The logs are kept in --out (a temporary folder
# otherwise). As in check.sh, a run is judged by what it prints and then stopped. It uses UDP ports 27310 to 27312 on 127.0.0.1.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=1800
SEED=2
TIMEOUT=300
OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --reads) READS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
[ -n "$OUT" ] || OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-lockstep.XXXXXX")
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" --ports 2 || exit 2
# <port>.pads: the recording with the other port's pads (of 2) from another seed.
python3 tools/netplay/make_inputs.py "$OUT/other.pads" --reads "$READS" --seed "$((SEED + 1))" --ports 2 > /dev/null || exit 2
python3 - "$OUT" <<'PY' || exit 2
import sys
out = sys.argv[1]
real = bytearray(open(f"{out}/inputs.pads", "rb").read())
other = open(f"{out}/other.pads", "rb").read()
header, read_size, pad_size = 32, 21, 5
for own in (1, 2):
    scrambled = bytearray(real)
    port = 2 - own  # the other one, counted from 0
    for at in range(header + 1 + port * pad_size, len(real), read_size):
        scrambled[at:at + pad_size] = other[at:at + pad_size]
    assert scrambled != real
    open(f"{out}/{own}.pads", "wb").write(scrambled)
PY

# start <name> [options...]: one copy in the background, with its own data folder, its hash log in
# $OUT/<name>.log and its output in $OUT/<name>.out; its PID is added to $pids.
# The recording is $OUT/inputs.pads, or $OUT/$pads if that's set.
pids=
start() {
    name=$1; shift
    rm -f "$OUT/$name.log" "$OUT/$name.out"
    "$EXE" --headless --rom "$ROM" --data "$OUT/data-$name" --fast --replay "$OUT/${pads:-inputs.pads}" \
        --hash-log "$OUT/$name.log" "$@" > "$OUT/$name.out" 2>&1 &
    pids="$pids $!"
}

# code_of <name>: the join code the hosting copy <name> printed (its key is random each time), once it has.
code_of() {
    i=0
    while [ $i -lt 600 ]; do
        code=$(sed -n 's/^\[netplay\] join code \([^ ]*\).*/\1/p' "$OUT/$1.out" 2>/dev/null)
        [ -n "$code" ] && { echo "$code"; return; }
        sleep 0.1
        i=$((i + 1))
    done
    echo "no-code-from-$1"
}

# finish <pattern> <name...>: waits until each named copy prints <pattern> (true), or one quits or the
# time runs out first (false); then kills the copies started.
finish() {
    pattern=$1; shift
    began=$(date +%s)
    result=
    while [ -z "$result" ]; do
        all=1
        for name in "$@"; do
            grep -q "$pattern" "$OUT/$name.out" 2>/dev/null || all=0
        done
        if [ $all -eq 1 ]; then
            result=done
        elif [ $(( $(date +%s) - began )) -ge "$TIMEOUT" ]; then
            result="not done in $TIMEOUT s"
        else
            for pid in $pids; do
                kill -0 "$pid" 2>/dev/null || result="a copy quit early"
            done
            sleep 0.2
        fi
    done
    sleep 0.3
    for pid in $pids; do
        kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    pids=
    if [ "$result" != done ]; then
        echo "  $result"
        for name in "$@"; do
            echo "  -- $name.out (last lines)"
            grep -v '^\[rt64\]' "$OUT/$name.out" | tail -n 8 | sed 's/^/    /'
        done
        return 1
    fi
    echo "  in $(( $(date +%s) - began )) s"
}

# same <name>: whether <name>'s hash log is solo's.
same() {
    if result=$(python3 tools/netplay/compare_hashes.py "$OUT/solo.log" "$OUT/$1.log"); then
        lines=$(wc -l < "$OUT/$1.log")
        if [ "$lines" -eq "$READS" ]; then
            echo "  $1 same as solo: $result"
            return 0
        fi
        echo "  $1 has $lines reads, not $READS"
        return 1
    fi
    echo "  $1 NOT the same as solo: $result"
    return 1
}

failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

echo "match"
pads=1.pads start match-host --netplay-host 27310 --netplay-address 127.0.0.1 --players 2
pads=2.pads start match-join --netplay-join "$(code_of match-host)"
if finish "was the last: quitting" match-host match-join; then
    same match-host || failed=1
    same match-join || failed=1
else
    failed=1
fi

echo "jittery"
start jittery-host --netplay-host 27311 --netplay-address 127.0.0.1 --players 2 --input-delay 0
start jittery-join --netplay-join "$(code_of jittery-host)" --input-delay 0 --jitter 500
if finish "was the last: quitting" jittery-host jittery-join; then
    same jittery-host || failed=1
    same jittery-join || failed=1
else
    failed=1
fi

echo "desync"
start desync-host --netplay-host 27312 --netplay-address 127.0.0.1 --players 2 --netplay-desync-at 300
start desync-join --netplay-join "$(code_of desync-host)"
if finish "the netplay match is over" desync-host desync-join; then
    for name in desync-host desync-join; do
        # The desync itself: this peer's comparison found it ("desync at read"), or the other's did and
        # said so (a desync); not merely the host's note that it changed its memory.
        if grep -q '\[netplay\] desync at read\|(a desync)' "$OUT/$name.out"; then
            echo "  $name: $(grep -m1 '\[netplay\] desync at\|the match is over' "$OUT/$name.out")"
        else
            echo "  $name stopped, but not for a desync"
            failed=1
        fi
    done
else
    failed=1
fi

echo "logs: $OUT"
if [ $failed -ne 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "passed: the match gave each copy the same game as the replay alone, and a desync was caught"
