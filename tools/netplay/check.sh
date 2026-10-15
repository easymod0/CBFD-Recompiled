#!/bin/sh
# The determinism check (netplay, issue #126): replays one made-up recording headless four ways and
# checks that each gives the game the same memory, read by read. The CI runs it (netplay.yml); run it
# yourself after building with CONKER_NETPLAY=ON:
#
#   tools/netplay/check.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                          [--reads 2700] [--seed 1] [--timeout 300] [--out <folder>]
#
# The runs, all on the timeline as fast as the host can (--fast):
#   a, b     the same replay twice
#   jitter   with random sleeps in the game's OS calls and graphics thread (a slower, uneven host)
#   state    a save state taken at a third of the way and loaded at two thirds (it goes back and on again)
# b, jitter and state must each match a. The logs are kept in --out (a temporary folder otherwise,
# printed at the end). It exits 0 if everything matches, 1 if not.
#
# A run is judged by what it prints, not by its exit code: once it says its last read was made (its hash
# log is written out by then) it's stopped, rather than waiting for it to quit. The runs share the game's data folder, so they're made one at a time, and nothing else may
# use the same build at the time.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=2700
SEED=1
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
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
[ -n "$OUT" ] || OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-check.XXXXXX")
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" || exit 2

# run <name> [harness options...]: one replay, its hash log in $OUT/<name>.log and its output in
# $OUT/<name>.out. Fails if it doesn't reach its last read within $TIMEOUT seconds, or its log is short.
run() {
    name=$1; shift
    out=$OUT/$name.out
    rm -f "$OUT/$name.log"
    start=$(date +%s)
    "$EXE" --headless --rom "$ROM" --fast --replay "$OUT/inputs.pads" --hash-log "$OUT/$name.log" "$@" > "$out" 2>&1 &
    pid=$!
    result=
    while [ -z "$result" ]; do
        if grep -q "was the last: quitting" "$out"; then
            result=done
        elif ! kill -0 $pid 2>/dev/null; then
            result="quit before its last read"
        elif [ $(( $(date +%s) - start )) -ge "$TIMEOUT" ]; then
            result="didn't reach its last read in $TIMEOUT s"
        else
            sleep 0.1
        fi
    done
    sleep 0.3
    kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null
    if [ "$result" != done ]; then
        echo "$name: $result (its output: $out)"
        tail -n 20 "$out" | sed 's/^/    /'
        return 1
    fi
    lines=$(wc -l < "$OUT/$name.log")
    if [ "$lines" -ne "$READS" ]; then
        echo "$name: its hash log has $lines reads, not $READS"
        return 1
    fi
    echo "$name: $READS reads in $(( $(date +%s) - start )) s"
}

# same <name>: whether <name>'s hash log matches a's.
same() {
    if result=$(python3 tools/netplay/compare_hashes.py "$OUT/a.log" "$OUT/$1.log"); then
        echo "  same as a: $result"
    else
        echo "  NOT the same as a: $result"
        return 1
    fi
}

failed=0
run a || exit 1
run b && same b || failed=1
run jitter --jitter 500 && same jitter || failed=1
run state --state-test $((READS / 3)):$((READS * 2 / 3)) && same state || failed=1

echo "logs: $OUT"
if [ $failed -ne 0 ]; then
    echo "FAILED: the same inputs didn't give the same game"
    exit 1
fi
echo "passed: the same inputs gave the same game every time"
