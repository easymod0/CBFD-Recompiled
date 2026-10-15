# Netplay tools

Work in progress for online multiplayer (issue #126, discussion #137). Lockstep netplay only works if
the same inputs always give the same game on every machine, so the first step is a determinism
harness: record a game's inputs, replay them, and compare a hash of the game's memory read by read.

## The harness

Built only with the CMake option `CONKER_NETPLAY` (off by default):

    cmake -S host -B host/build -DCONKER_NETPLAY=ON
    cmake --build host/build

Its options (see `host/src/netplay/harness.cpp`):

| Option | |
|---|---|
| `--record <file>` | records the pads the game reads, at each controller read |
| `--replay <file>` | gives the game the recorded pads instead (starts the game directly), and quits after the last read |
| `--hash-log <file>` | logs the XXH3-64 of the game's memory at each controller read |
| `--dump-at <n>` | writes the game's memory at read `n` to `<hash-log>.<n>.bin` (can be repeated) |
| `--ignore <addr>:<size>` | leaves a known difference out of the hash (hex, e.g. `800BE6F0:8`; can be repeated) |
| `--stop-after <n>` | quits after read `n` |
| `--ports <n>` | the game sees controllers on ports 1 to `n`, without paks (a replay uses its recording's) |
| `--fast` | runs the timeline as fast as the host can, without waiting for real time (headless) |
| `--jitter <us>` | sleeps at random, up to `us` microseconds, at the game's OS calls and in the graphics thread: a test that the host's speed doesn't change the game |
| `--real-time` | runs the game on the time, as normally, instead of the timeline, to compare |

While any of these are used, the game starts from an empty save of its own (`saves/harness/`, emptied
each run; your own save isn't touched) and sees fixed controllers. Reads are counted from the first,
so read `n` of one run is read `n` of another, and the pads are recorded as the N64 reads them (after
the sticks are converted), which is what netplay will send. The runtime's own host pointers in game
memory (each thread's context in its `OSThread`) are always left out of the hash.

While the harness is used, the game runs on the runtime's virtual timeline (deterministic mode,
`H3-design.md`): game code takes no virtual time, and its VIs (exactly 60 a second), timers and task
completions are delivered only when the game is idle, so the same inputs give the same game however
fast the host is. Its clock (`osGetCount`, `osGetTime`) is the timeline's, plus about a microsecond
each time it's read. A headless replay with `--fast` takes about 1.8 s for 2,700 reads (90 s of game).
`--real-time` turns it off: then two replays differ at the deadlines of two timers the game sets from
the clock (`--ignore 8003A598:8 --ignore 800BE6F0:8`), and at the order events arrive in.

Run one at a time from the same build: runs share the data folder (the stored ROM, which `--rom`
writes, and `saves/harness/`), so two at once break each other.

Record with mouse and gyro aiming and the free camera off: they write the camera straight into the
game rather than through the controller, so a replay doesn't have them.

A typical comparison, of two replays of the same recording:

    host/build/ConkerRecomp --headless --replay run.pads --hash-log a.log
    host/build/ConkerRecomp --headless --replay run.pads --hash-log b.log
    tools/netplay/compare_hashes.py a.log b.log

and, at the read where they first differ, the memory itself:

    ... --hash-log a.log --dump-at 900    (and the same for b)
    tools/netplay/compare_hashes.py a.log b.log --dumps a.log.900.bin b.log.900.bin

## The scripts

| Script | |
|---|---|
| `compare_hashes.py` | the first read two hash logs differ at; with dumps, the ranges that differ, by data symbol |
| `make_inputs.py` | a recording of made-up inputs from a seed, to replay without anyone playing |
| `scan_clock.py` | every read of the game's clock in its code, and whether it's in a loop (needs your ROM, unpacked with `recomp/unpack_rom.py`) |
| `dis_around.py` | the instructions around a function's clock reads |

`H3-design.md` is the design for the timeline (a prototype, still to agree with the owner).
