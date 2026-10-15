# H3: events at fixed points (design)

Netplay phase H3 (fork issue #2, upstream issue 126). The design, to agree with the owner before it's
built for real; a prototype checks the core of it (below). Nothing here changes the game when the
harness (and later netplay) isn't used.

**Goal:** the host's speed can't change what the game does. A slow host slows the game, never changes
it: a replay on one busy core matches a normal one read for read.

## What makes a run differ today

The runtime runs one game thread at a time, and switches only inside OS calls (`osSendMesg`,
`osRecvMesg`, `osStartThread`, ...): never in the middle of game code. So between two OS calls the
game is a pure function of game memory. What it does can only differ from run to run through what
comes from outside:

| From outside | Where | Fixed by |
|---|---|---|
| Controller input | `osContGetReadData` | H1 (recorded, replayed) |
| The clock | `osGetCount`, `osGetTime`, timer deadlines | H2 (virtual clock) |
| **When events arrive** | host threads post messages to one shared queue, which game threads empty at *every* OS call | **H3** |
| **The VI phase at startup** | the VI thread counts retraces and swaps VI state as soon as the game is marked started, before the ROM is even loaded | **H3** |
| **Host threads writing game memory** | the SP task thread (the audio microcode's output), the timer thread (a repeating timer's next deadline) | **H3** |
| Which framebuffer the VI shows | `osViGetCurrentFramebuffer`, swapped by the VI thread (without the lock the game's VI calls take); the scheduler (`func_10004DB0`) reads it | **H3** |
| Two host patches that wait on real time | `conker_spin_wait_pass` (1 ms yield), the 500 ms "song just started" mark (`ultra_extras.cpp`) | **H3** |
| The audio device's buffer | `osAiGetLength`, MP3 player pace | H4 |
| The renderer | RT64 writing framebuffers back into game memory, if it does | H5 |
| Settings that change game memory | mouse and stick camera (reads SDL and the time on a game thread), field of view (cull scales), model detail (which models load), widescreen (the window's size) | netplay settings profile (#5) |

The events, and the threads they come from:

| Event | Sent by | When |
|---|---|---|
| VI (retrace) | VI thread | every 1/60 s of real time; a late VI thread *skips* VIs (`new_total_vis` comes from the time) |
| Timers | timer thread | at the deadline, in real time |
| SP done, audio task | SP task thread | when the task finishes |
| SP done, graphics task | graphics thread | when it takes the task from its queue |
| DP done | graphics thread | when the renderer has taken the display list |
| PI (DMA, flash), SI (controller read) | the game thread itself (the work is synchronous) | at once, through the shared queue |

Conker never registers the AI event (only SI, SP, DP, PRE-NMI and VI), and `osAiGetStatus` always says
the AI is free, so the scheduler's audio timer is always 20,000 ticks after the VI.

The game's frame delta, `D_800BE9E4` (VIs per frame), is used all through game logic, so VIs have to
be virtual: fixing only the races in the scheduler wouldn't be enough.

### Measured

A temporary probe over a 2,700-read headless replay (not committed):

- The game goes idle (every game thread waits, and the idle thread runs `pause_self`) about 4 times
  per controller read: about twice per VI (after the VI's work, and after the audio timer's).
- **45%** of host events reached the game while it was busy, at whatever OS call came next. Which call
  that is depends on how far the game got before the host thread posted. Today `pause_self` takes one
  message per wake, too.

## The design: a virtual timeline, events delivered when the game is idle

While the harness is used, the runtime runs the game as a discrete-event simulation:

1. **Game code takes no time.** Virtual time only moves when the game is idle. Then the runtime moves
   it straight to the next event. This is how the game already behaves on a fast host: its work for
   a frame takes a fraction of a VI, then it waits.
2. **No host thread posts to the game.** The VI and timer threads stop once the mode is on, the audio
   task runs on the game thread, and the graphics thread only reports that it's done. The shared queue
   then only holds the game's own messages (PI, SI, SP done), posted from game threads, so they arrive
   in the same order every run, and no new queue is needed.
3. **Delivery points.** When the game is idle (the running queue is empty in `pause_self`) the runtime:
   1. hands over the game's own messages still queued;
   2. finds the next event time `T`: the next VI (VI `k` at `k` × 781,250 ticks, exactly 60 Hz), or the
      earliest timer deadline, whichever is sooner, but never before the last `T`;
   3. paces: waits until real time reaches `T` (skipped with `--fast`). After a stall (a slow host, or
      waiting for a peer) it carries on from there instead of catching up;
   4. sets the clock to at least `T` and delivers every event due at `T`, in a fixed order: DP done
      (after waiting in real time for the renderer to finish the lists), then timers (by deadline,
      then address; a repeating one's next deadline is the last plus its interval), then the VI (the VI
      register swap, the screen update, the retrace count and message);
   5. lets the woken threads run until the game is idle again. If nothing woke, it goes on to the next
      event. Each VI gets a delivery point of its own.

### Each event

- **VI.** The timeline's. Turning the mode on stops the VI thread (it acknowledges, so it's never
  partway through a VI) and resets the VI state: virtual time 0, the first VI state, one retrace to
  go. The game's own `osViSetMode`/`osViSetEvent` then land at the same point every run. No VI is ever
  skipped.
- **Timers.** On the timeline; the timer thread waits for good. H2's real-time deadlines go away.
- **The clock (H2).** Its base is the timeline's `T`, not the controller read count, so the clock and
  the timers are on one timeline. Each read of the clock still moves it a step (47 ticks).
- **The audio task.** It runs on the game thread, inside `osSpTaskStartGo`, and posts its SP done
  from there. Its writes land at the same point every run, and nothing runs alongside them. With SP
  done at once, the scheduler's yield path (`osSpTaskYield`) is never taken.
- **Graphics tasks.** SP done is posted at once by the submitting thread (as today: "the RSP completed
  instantly"). DP done is due at the next VI, delivered just before it. That adds no latency: Conker's
  scheduler already waits for a VI after DP done before it swaps buffers (`func_10004FE0`), so the
  frame delta stays the same. **DP before VI at the same `T` is a rule**, checked by comparing
  `D_800BE9E4` with normal mode. Waiting for the renderer at the VI also means whatever RT64 writes
  into game memory lands at fixed points (H5).
- **The two host patches.** `conker_spin_wait_pass` becomes a delivery point that moves virtual time
  at most 1 ms on: what "yield for up to 1 ms" means. The 500 ms song mark is timed on virtual time.
- **Display-list events** (`osExQueueDisplaylistEvent`, posted from the graphics thread) go to the
  timeline too, or are refused in this mode. Conker doesn't use them.

### A game that doesn't go idle

During a long load the game may not go idle for a while. Then no VIs arrive, the scheduler stops
sending its per-VI messages, and audio starves. The review suggested a limit: after a fixed count of
OS calls without going idle, move virtual time on a fixed amount.

Measured on a 13.5-minute session played in a window (24,449 reads, through several areas and loads;
the timeline logs each new longest burst):

| | OS calls | Real time |
|---|---|---|
| The most OS calls in one burst | 260 | 5.8 ms |
| The longest burst (a load, at 381 s) | 16 | 161 ms |

So a load is long in game code, not in OS calls: a count of OS calls can't see it, and the game always
went idle again by itself. Determinism doesn't need a limit: the load takes no virtual time, the same
in every run. All it costs is up to 161 ms without VIs during a load, which only matters to how the
sound plays (H4). **So no limit for now.** The timeline keeps logging the longest bursts; if a game
thread ever spins without going idle, a limit on virtual time per OS call can be added then.

`pause_self` is also called by three loops that hang the game when an allocation fails
(`func_151AE0E4`, `func_1501878C`, `func_1509B950`). A delivery point checks that nothing else is
ready to run, rather than assuming it's the idle thread.

### What changes for the game

In deterministic mode, events no longer interrupt the game partway through a burst of work. They wait
until the game is idle (or yields, or hits the limit). The N64 interrupts at any instruction, and the
runtime today interrupts at any OS call, so this is a third behaviour. The game already copes with
the runtime's: it can't rely on being interrupted in game code. Normal mode keeps today's.

## Alternatives

- **Record the events' points and replay them:** works for replays, not for lockstep, where peers
  would have to send each other the schedule.
- **Deliver at counted OS calls:** deterministic too, but any change to the runtime, a mod or a hook
  that adds an OS call would change the game, so peers would need identical builds down to that.
- **Make the VI thread wait for idle:** the timer and graphics threads would need the same handshake.
  One timeline is simpler.

## Netplay

Every peer runs the same timeline from the same inputs, so the same events arrive at the same points.
The lockstep wait (H8) happens at a controller read: a real-time wait inside a game thread, which
doesn't move virtual time, so it can't change the game. Deterministic mode is required on every peer.
Recordings and hash logs from normal mode can't be compared with it.

## Save states

In deterministic mode, save states are taken and loaded only at delivery points (not also in
`osRecvMesg`, as in normal mode). A state (layout 4; layout 3's still load) now also holds:

- the timeline: virtual time, the next VI, and the graphics tasks whose DP done is due;
- the RSP's data memory (4KB): each audio task reloads most of it, but not all;
- the host's own state that decides what it writes into game memory (`host_state.cpp`, in the save
  file's spare block, empty in older files): the song marks, where the rings of our display lists
  are (the widescreen iris wipe's, the cutscene bars'), and when each shadow was last drawn (a shadow
  missing for a frame is dropped for one more, `interpolation.cpp`).

The last two were found by the harness's `--state-test <s>:<l>` (save after read `s`, load at `l`,
compare with a run without it). Without them, a load right after the save matched, but a longer
rewind didn't: two reads after the load (the shadows) and 80 reads around a scene's iris wipe (the
ring: a branch in the game's display list to a different slot). With them, every test matches read
for read, on the made-up recording and the played one (saves and loads up to 23,000 reads apart),
with jitter too.

States taken in normal mode still load: the timeline starts again from the state's clock.

## The runtime patch

- A new file, `ultramodern/src/timeline.cpp`, holds the timeline: the delivery point, pacing, the
  limit.
- Small hooks, each behind one check: the VI thread (stops), the timer thread (waits), `pause_self` and
  `yield_self_1ms` (delivery points), `submit_rsp_task` (the audio task inline, SP done posted), and the
  graphics thread (reports DP done to the timeline).
- A few hundred lines, most in the new file. With the harness off, every hook takes today's path.
- The API: `ultramodern::timeline::enable(fast)` before the game starts. It replaces H2's
  `set_virtual_clock`.

## To agree with the owner

1. In deterministic mode only, events are delivered when the game is idle (or yields, or hits the
   limit), not at any OS call. A slow host slows the game, never changes it.
2. The audio RSP task runs on the game thread in deterministic mode.
3. Graphics DP done is due at the next VI. No added latency: the scheduler already waits for that VI,
   and the frame delta is unchanged.
4. VIs come at exactly 60 Hz of virtual time (as today's VI thread), which is a whole 781,250 ticks
   per VI; not 59.94.
5. No limit for a game that doesn't go idle: loads are long in game code, not OS calls (measured: at
   most 260 OS calls, and 161 ms with 16), and the game always goes idle by itself.
6. Save states, in deterministic mode, only at delivery points.
7. Deterministic mode is required on every netplay peer; settings that change game memory are forced
   or synced between peers.
8. All of it is in `recomp/n64modernruntime.patch`, like H1 and H2, rather than going to
   N64ModernRuntime.

## The prototype

Built (step 1 below), behind the harness: `ultramodern/src/timeline.cpp` and hooks in `events.cpp`,
`timer.cpp`, `scheduling.cpp` and `mesgqueue.cpp`, in `recomp/n64modernruntime.patch`. The harness's
`--fast`, `--jitter <us>` and `--real-time` options drive it. Results, headless, on a made-up
2,700-read recording (90 s of game):

| Test | Result |
|---|---|
| 20 `--fast` replays | all identical, every read (about 1.8 s each) |
| `--jitter 500` (up to 0.5 ms at every OS call and graphics task) | identical to them |
| `--jitter 3000`, 300 reads | identical |
| One busy core (`taskset -c 3` next to a busy loop), `--fast` | identical |
| One busy core, paced to real time, 300 reads | identical |
| Paced vs `--fast` | identical |
| Frame delta `D_800BE9E4` | 2 VIs at every read, the same as normal mode |
| Bursts between idle points | at most 260 OS calls, at most 8.6 ms of real time |
| A 13.5-minute played recording (24,449 reads), two replays | identical, about 17 s each |
| `--state-test` (save, then load up to 23,000 reads later), both recordings, and with jitter | identical to runs without it |

Normal mode (no harness) runs as before. With `--real-time`, runs still differ as they did before H3.

A replay of the played recording follows the windowed session for about 380 s (the same scenes at the
same virtual times, with the same OS-call counts), then parts from it after a load. A window still
differs from headless where H4 (the sound device) and H5 (the renderer) come in.

The song mark (`ultra_extras.cpp`) now runs on the game's clock (`osGetCount`, the timeline's in
deterministic mode) instead of the time; `conker_spin_wait_pass` already went through the timeline
(`yield_self_1ms` is a delivery point there).

**Window vs headless.** A replay in a window differs from a headless one from read 1. Dumps show why,
and none of it is the timeline: the field of view and widescreen settings write the cull scales
(`cullScaleX_800D35E0`), and what looks like the audio state and buffers (`D_800314B0`, around
`0x8010B108`) follows the sound device's buffer, which headless reports empty. They're the netplay
settings profile (#5) and H4. A window replay also differs from the window session it was recorded
in, so the window has a source of its own too (the sound device, most likely).

Not done yet: display-list events (Conker doesn't use them), windowed runs (they need #5, H4 and H5).

## The steps

1. **Prototype** (headless; done, above): the VI and timer threads stop, the audio task inline, delivery points
   with VI, timers and DP done, the clock from the timeline, pacing and `--fast`, and a jitter test
   mode (random sleeps at OS calls and in the graphics thread, which finds races better than a busy
   core). Check: 20 or more replays identical; jitter and busy-core replays identical to them;
   `D_800BE9E4` the same as normal mode; OS calls and time between idle points on a level load.
2. ~~The limit, from those numbers~~: measured, not needed (above).
3. Save states: a state loaded at read N gives the same reads from N on. **Done** (above).
4. The two host patches on virtual time (**done**); display-list events (Conker doesn't use them).
5. **Done when:** a replay on one busy core (`taskset -c 3` next to a busy loop) matches a normal one
   read for read, and `--fast` matches paced.

H4 (the audio device) and H5 (the renderer) come after: windowed replays need both.
