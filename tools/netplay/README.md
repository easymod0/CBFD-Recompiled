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
| `--state-test <s>:<l>` | takes a save state after read `s` and loads it at read `l`, so the game goes back and on again: the hash log then holds the reads after the load, which should match a run without it |
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

The sound plays on the timeline too (H4, `host/src/audio_clock.cpp`). The game's audio thread sizes
each buffer by how much is left of the one playing (`osAiGetLength`), and the MP3 player that cutscenes
and lip-sync wait on is paced by it. Normally that comes from the sound device's queue, whose pace
differs from run to run, and headless has none. While the harness is used, each buffer the game hands
over (`osAiSetNextBuffer`) instead ends at a fixed time on the timeline after the one before, as the
N64's audio interface plays them, and the length is what's left of the buffer playing at the
timeline's time, headless and in a window alike. The sound device only plays what it's given: it
waits for about 70 ms of sound before it starts (and again if it runs dry), and drops buffers while it's
over about 270 ms behind. A save state holds the buffers still playing.

A window replay now matches a headless one. Of the session recorded on 2026-10-14, the first 12,000
reads (6.7 minutes: the opening cutscenes and the first chapter) give the same hashes in a window
and headless; in the window the device ran dry once, before the first buffer started, and dropped
nothing. Before H4, two windows split from each other at about read 360, and before H5 (the renderer
writing into game memory) a window split from headless at read 2.

Run one at a time from the same build: runs share the data folder (the stored ROM, which `--rom`
writes, and `saves/harness/`), so two at once break each other.

### The netplay settings profile

While the harness is used, the settings that change the game or what's in its memory are fixed
(`host/src/netplay/profile.cpp`), whatever the menus say, so a recording made in a window replays the
same headless or on another machine:

- **Off:** the mouse and gyro (R-Look, the aiming modes, the free camera, the bank's leaps), Mouse:
  Turn the Camera, Right Stick: Free Camera and Aiming: Swap Sticks. They move the camera in game
  memory from input the recording doesn't hold, or by the time.
- **At their defaults:** the Camera tab (Field of View, Invert Turning, Turning Speed, the stick's
  aiming, Lock-On), Conker's Detail, Cutscene Aspect Ratio (Expand) and the sound volumes (100%).
- **Widescreen:** the culls and sprites are widened for 16:9 whatever the window (a wider window can
  lose a little at its sides).
- **Save states** (F5, F7) are off, and **mods** are refused but for texture packs: turn them off in
  the Mods menu.
- **The renderer** writes nothing into game memory (H5). RT64's Render to RAM copies each frame's
  colour and depth images back into the game's memory, as the N64's RDP did, from the graphics thread
  whenever it gets to the display list. Headless, with no renderer, nothing is written. So a window
  run differed from a headless one from read 2 (the depth image at `0x800E9D80`), and the copies raced
  the game. RT64 still draws everything on the GPU; only the copy back is off
  (`RT64::setRenderToRAMOff`, `rt64.patch`), so what the game's own code reads back from those images
  is whatever was there before (as headless).

What only changes the picture or the sound outside game memory (the renderer's settings, Overscan
Borders, Show FPS, the reticle, texture packs) and what only changes the pads (the controls) is left to
the player: the pads are what's recorded, and what netplay will send.

### Recordings

A recording (version 2) holds the ROM's XXH3-64 and the profile's hash: a replay with another ROM or
another profile is refused before the game starts. `make_inputs.py` writes 0 for both, which isn't
checked; version 1 recordings (before the profile) are replayed unchecked.

### The determinism check

After a change, run `tools/netplay/check.sh` (with the game built with `CONKER_NETPLAY=ON`). It makes a
recording of 2,700 made-up reads (`make_inputs.py --seed 1`), replays it headless four ways (twice, with
random sleeps in the host, and through a save state taken and loaded on the way) and compares each
hash log with the first. A run is judged by what it prints, not its exit code, as quitting can hang
or crash on Linux (upstream issue 127). The CI runs it on every push (`.github/workflows/netplay.yml`)
where the ROM secrets are set, as for the other builds.

A typical comparison, of two replays of the same recording:

    host/build/ConkerRecomp --headless --replay run.pads --hash-log a.log
    host/build/ConkerRecomp --headless --replay run.pads --hash-log b.log
    tools/netplay/compare_hashes.py a.log b.log

and, at the read where they first differ, the memory itself:

    ... --hash-log a.log --dump-at 900    (and the same for b)
    tools/netplay/compare_hashes.py a.log b.log --dumps a.log.900.bin b.log.900.bin

## The transport (in progress, fork issue #7)

`host/src/netplay/net/` holds what goes over the network, and builds without the game or the ROM
(its `CMakeLists.txt`). The game links it with `CONKER_NETPLAY`, but doesn't use it yet (lockstep, #8).

- **GameNetworkingSockets** (v1.6.1, downloaded while configuring): encrypted UDP connections, direct by
  address. The host listens on a port; the others connect to it, and it passes their inputs on. Its
  NAT traversal and relays (ICE) are off for now, so the host may have to forward the port.
- **`packets.cpp`**: every packet (Hello, Welcome, Refuse, Start, Inputs, Hash, Bye), fixed-size and
  versioned, each field checked; the only code that reads from the network. `packets_test.cpp` tests
  it and `fuzz_packets.cpp` fuzzes it.
- **`session.cpp`**: the handshake refuses another build, ROM or settings profile, a full or started
  match; then each peer sends only its own port's pads (8 reads per packet, so lost packets don't
  matter) and memory hashes.
- **`netplay_probe`**: two or more copies play a made-up match (`host <port>`, `join <address:port>`,
  `--loss <percent>`) and check every pad arrived right. `test.sh` runs the tests and a few matches;
  `.github/workflows/netplay-transport.yml` runs them on Windows, Linux and macOS (no ROM needed).

## The scripts

| Script | |
|---|---|
| `check.sh` | the determinism check: replays a made-up recording headless twice, with `--jitter` and with `--state-test`, and fails unless all four give the same hashes (about 30 s; the CI runs it, `.github/workflows/netplay.yml`) |
| `compare_hashes.py` | the first read two hash logs differ at (it exits 1 if they differ); with dumps, the ranges that differ, by data symbol |
| `make_inputs.py` | a recording of made-up inputs from a seed, to replay without anyone playing |
| `scan_clock.py` | every read of the game's clock in its code, and whether it's in a loop (needs your ROM, unpacked with `recomp/unpack_rom.py`) |
| `dis_around.py` | the instructions around a function's clock reads |

`H3-design.md` is the design for the timeline (a prototype, still to agree with the owner).
