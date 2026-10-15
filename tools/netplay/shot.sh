#!/bin/sh
# Replays a recording in a window, as fast as it goes, and saves a screenshot of its last moment: for
# looking at what's drawn at a point a recording reaches (make_match.py's matches, with --my-view-only...).
# X11 only (under Wayland the game is run with SDL_VIDEODRIVER=x11); needs python-xlib and ImageMagick.
#
#   tools/netplay/shot.sh <recording.pads> <shot.png> [game options...]
#     e.g. tools/netplay/shot.sh heist.pads heist.png --my-view-only
#
# Each run starts from a data folder of its own (a temporary one), so it doesn't touch the game's.
# Options from the environment: EXE (host/build/ConkerRecomp), ROM (conker/baserom.us.z64),
# WINDOW (960x540), SCALE (the desktop's scaling, 1).
set -u
cd "$(dirname "$0")/../.."
[ $# -ge 2 ] || { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
pads=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shot=$2
shift 2
EXE=${EXE:-host/build/ConkerRecomp}
ROM=$(cd "$(dirname "${ROM:-conker/baserom.us.z64}")" && pwd)/$(basename "${ROM:-conker/baserom.us.z64}")
data=$(mktemp -d "${TMPDIR:-/tmp}/netplay-shot.XXXXXX")
out=$data/out.txt

SDL_VIDEODRIVER=x11 "$EXE" --rom "$ROM" --data "$data/data" --window "${WINDOW:-960x540}" --replay "$pads" --fast "$@" > "$out" 2>&1 &
pid=$!
result=1
for _ in $(seq 1 150); do
    if grep -q "was the last" "$out"; then
        sleep 2
        python3 tools/netplay/xwin.py --pid $pid --scale "${SCALE:-1}" shot "$shot" && result=0
        break
    fi
    if ! kill -0 $pid 2>/dev/null; then
        echo "the game quit before the recording's end:"
        grep -v '^\[rt64\]' "$out" | tail -n 5
        break
    fi
    sleep 2
done
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
rm -rf "$data"
[ $result -eq 0 ] && echo "$shot"
exit $result
