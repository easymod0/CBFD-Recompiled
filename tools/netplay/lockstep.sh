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
#   mouse      a 3-player Deathmatch where everyone moves their mouse, gyro and scroll wheel and aims
#              (make_match.py --mouse), alone and as a match of three at the automatic input delay, each
#              copy with the others' pads and movement scrambled: all three must match the solo replay
#   couch      the same 3-player Deathmatch with two players on one copy (--local-players 2): the host
#              with ports 1 and 2 and a joiner with port 3, then the host with port 1 and a joiner with 2
#              and 3; each copy's file has the other copy's ports scrambled. Two players' views are shown two
#              to a window, which widens their culls: each match must match a solo replay with those view pairs
#   tall       a 2-player Deathmatch (make_match.py --mouse): the split screen's two views are drawn taller in
#              "my view only", the match's rule, so every copy widens each camera's culls up and down: both copies
#              of a match of two must match its solo replay
#   jittery    the same with input delay 0 and random sleeps (--jitter 500) on the joiner, so the two
#              are out of step and wait for each other all the time
#   desync     the host changes a byte of its memory at read 300: both must stop for a desync
# It exits 0 if all is as it should be, 1 if not. The logs are kept in --out; without it they're in a
# temporary folder, deleted if all passed. The data folders it makes for the copies (about 1 GB in all) are
# always deleted. As in check.sh, a run is judged by what it prints and then stopped. It uses UDP ports
# 27310 to 27316 on 127.0.0.1.
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
        -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-lockstep.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
# However the check ends (Ctrl-C too): the copies still running are stopped, and the data folders this
# made deleted (only those: one already in an --out folder is left alone).
made=
pids=
cleanup() {
    for pid in $pids; do kill -9 "$pid" 2>/dev/null; done
    for name in $made; do rm -rf "$OUT/data-$name"; done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# scramble <recording> <other> <ports> <prefix>: <prefix><n>.pads for each port n (from 1), the recording
# with every other port's pads and movement taken from <other> (the same length, from another seed).
scramble() {
    python3 - "$@" <<'PY'
import sys
real_path, other_path, ports, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
real = open(real_path, "rb").read()
other = open(other_path, "rb").read()
assert len(real) == len(other)
header, read_size, pad_size, motion_size = 40, 73, 5, 13   # version 4 (harness.cpp)
for own in range(ports):
    scrambled = bytearray(real)
    for port in range(ports):
        if port == own:
            continue
        for start in range(header, len(real), read_size):
            pad = start + 1 + port * pad_size
            motion = start + 1 + 4 * pad_size + port * motion_size
            scrambled[pad:pad + pad_size] = other[pad:pad + pad_size]
            scrambled[motion:motion + motion_size] = other[motion:motion + motion_size]
    assert scrambled != real
    open(f"{prefix}{own + 1}.pads", "wb").write(scrambled)
PY
}

# scramble_own <recording> <other> <ports> <out> <own>: <out>, the recording with every port but those in
# <own> (port numbers from 1, e.g. 12) scrambled from <other>, for a copy playing several ports.
scramble_own() {
    python3 - "$@" <<'PY'
import sys
real_path, other_path, ports, out, own = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
real = open(real_path, "rb").read()
other = open(other_path, "rb").read()
assert len(real) == len(other)
header, read_size, pad_size, motion_size = 40, 73, 5, 13   # version 4 (harness.cpp)
scrambled = bytearray(real)
for port in range(ports):
    if str(port + 1) in own:
        continue
    for start in range(header, len(real), read_size):
        pad = start + 1 + port * pad_size
        motion = start + 1 + 4 * pad_size + port * motion_size
        scrambled[pad:pad + pad_size] = other[pad:pad + pad_size]
        scrambled[motion:motion + motion_size] = other[motion:motion + motion_size]
assert scrambled != real
open(out, "wb").write(scrambled)
PY
}

# The recordings say everyone sees only their own view, as the matches below are (the host's rule by default),
# so the solo replays have the same culls.
python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" --ports 2 --mouse --own-view || exit 2
python3 tools/netplay/make_inputs.py "$OUT/other.pads" --reads "$READS" --seed "$((SEED + 1))" --ports 2 --mouse --own-view > /dev/null || exit 2
scramble "$OUT/inputs.pads" "$OUT/other.pads" 2 "$OUT/" || exit 2
python3 tools/netplay/make_match.py "$OUT/mouse.pads" --mode deathmatch --players 3 --mouse --seed "$SEED" --own-view || exit 2
python3 tools/netplay/make_match.py "$OUT/mouse-other.pads" --mode deathmatch --players 3 --mouse --seed "$((SEED + 1))" --own-view > /dev/null || exit 2
scramble "$OUT/mouse.pads" "$OUT/mouse-other.pads" 3 "$OUT/mouse-" || exit 2
# The couch matches show two players' views two to a window (ports 1 and 2, then 2 and 3), which changes their
# culls: their solo replays are the mouse recording marked so (the header's view pairs).
for pairs in 12 23; do
    python3 -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[36:40]=sum(1<<(int(p)-1) for p in sys.argv[2]).to_bytes(4,'little'); open(sys.argv[3],'wb').write(d)" \
        "$OUT/mouse.pads" $pairs "$OUT/couch-solo$pairs.pads" || exit 2
done
scramble_own "$OUT/couch-solo12.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-12.pads" 12 || exit 2
scramble_own "$OUT/couch-solo12.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-3.pads" 3 || exit 2
scramble_own "$OUT/couch-solo23.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-23.pads" 23 || exit 2
scramble_own "$OUT/couch-solo23.pads" "$OUT/mouse-other.pads" 3 "$OUT/couch-1.pads" 1 || exit 2
MOUSE_READS=$(( ($(wc -c < "$OUT/mouse.pads") - 40) / 73 ))
python3 tools/netplay/make_match.py "$OUT/tall.pads" --mode deathmatch --players 2 --mouse --seed "$SEED" --own-view || exit 2
python3 tools/netplay/make_match.py "$OUT/tall-other.pads" --mode deathmatch --players 2 --mouse --seed "$((SEED + 1))" --own-view > /dev/null || exit 2
scramble "$OUT/tall.pads" "$OUT/tall-other.pads" 2 "$OUT/tall-" || exit 2
TALL_READS=$(( ($(wc -c < "$OUT/tall.pads") - 40) / 73 ))

# start <name> [options...]: one copy in the background, with its own data folder, its hash log in
# $OUT/<name>.log and its output in $OUT/<name>.out; its PID is added to $pids.
# The recording is $OUT/inputs.pads, or $OUT/$pads if that's set.
start() {
    name=$1; shift
    rm -f "$OUT/$name.log" "$OUT/$name.out"
    [ -e "$OUT/data-$name" ] || made="$made $name"
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

# same <name> [<solo> <reads>]: whether <name>'s hash log is solo's (or <solo>'s, of <reads> reads).
same() {
    ref=${2:-solo}
    reads=${3:-$READS}
    if result=$(python3 tools/netplay/compare_hashes.py "$OUT/$ref.log" "$OUT/$1.log"); then
        lines=$(wc -l < "$OUT/$1.log")
        if [ "$lines" -eq "$reads" ]; then
            echo "  $1 same as $ref: $result"
            return 0
        fi
        echo "  $1 has $lines reads, not $reads"
        return 1
    fi
    echo "  $1 NOT the same as $ref: $result"
    return 1
}

failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

echo "match"
pads=1.pads start match-host --netplay-host 27310 --netplay-address 127.0.0.1 --players 2 --input-delay 2
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

echo "mouse"
pads=mouse.pads start mouse-solo
finish "was the last: quitting" mouse-solo || exit 1
pads=mouse-1.pads start mouse-host --netplay-host 27313 --netplay-address 127.0.0.1 --players 3
code=$(code_of mouse-host)
pads=mouse-2.pads start mouse-join1 --netplay-join "$code"
# Ports go to joiners in the order they're let in, and each copy plays its own file's port: the second waits
# for the first to be in (port 2).
i=0
while ! grep -q "in, on port 2" "$OUT/mouse-join1.out" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
pads=mouse-3.pads start mouse-join2 --netplay-join "$code"
if finish "was the last: quitting" mouse-host mouse-join1 mouse-join2; then
    for name in mouse-host mouse-join1 mouse-join2; do
        same $name mouse-solo "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi

echo "couch"
# One at a time: finish counts a copy that quits after its last read, before the other's, as quitting early.
pads=couch-solo12.pads start couch-solo12
finish "was the last: quitting" couch-solo12 || exit 1
pads=couch-solo23.pads start couch-solo23
finish "was the last: quitting" couch-solo23 || exit 1
pads=couch-12.pads start couch-host2 --netplay-host 27314 --netplay-address 127.0.0.1 --players 3 --local-players 2
pads=couch-3.pads start couch-join1 --netplay-join "$(code_of couch-host2)"
if finish "was the last: quitting" couch-host2 couch-join1; then
    grep -q "this is ports 1 and 2" "$OUT/couch-host2.out" || { echo "  couch-host2 isn't on ports 1 and 2"; failed=1; }
    # The automatic delay counts the joining machine's round trip (one, on loopback: 0), not its players.
    grep -q "this is ports 1 and 2, input delay 0" "$OUT/couch-host2.out" || { echo "  couch-host2's automatic input delay isn't 0"; failed=1; }
    for name in couch-host2 couch-join1; do
        same $name couch-solo12 "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi
pads=couch-1.pads start couch-host1 --netplay-host 27315 --netplay-address 127.0.0.1 --players 3 --input-delay 2
pads=couch-23.pads start couch-join2 --netplay-join "$(code_of couch-host1)" --local-players 2
if finish "was the last: quitting" couch-host1 couch-join2; then
    grep -q "this is ports 2 and 3" "$OUT/couch-join2.out" || { echo "  couch-join2 isn't on ports 2 and 3"; failed=1; }
    for name in couch-host1 couch-join2; do
        same $name couch-solo23 "$MOUSE_READS" || failed=1
    done
else
    failed=1
fi

echo "tall"
pads=tall.pads start tall-solo
finish "was the last: quitting" tall-solo || exit 1
pads=tall-1.pads start tall-host --netplay-host 27316 --netplay-address 127.0.0.1 --players 2
pads=tall-2.pads start tall-join --netplay-join "$(code_of tall-host)"
if finish "was the last: quitting" tall-host tall-join; then
    for name in tall-host tall-join; do
        same $name tall-solo "$TALL_READS" || failed=1
    done
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

if [ $failed -ne 0 ]; then
    echo "logs: $OUT"
    echo "FAILED"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: the match gave each copy the same game as the replay alone, and a desync was caught"
