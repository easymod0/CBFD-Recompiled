# H3: events at fixed points (design)

Netplay phase H3 (fork issue #2, upstream issue 126). This is the design, to agree before any code
is written. Nothing here changes the game when the harness (and later netplay) isn't used.

**Goal:** the host's speed can't change what the game does. A replay on one busy core matches a
normal one read for read.

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
| What the RSP audio task writes | the SP task thread, in parallel with game threads | **H3** |
| Which framebuffer the VI shows | `osViGetCurrentFramebuffer`, swapped by the VI thread; the scheduler (`func_10004DB0`) reads it | **H3** |
| Two host patches that wait on real time | `conker_spin_wait_pass` (1 ms yield), the 500 ms "song just started" mark (`ultra_extras.cpp`) | **H3** |
| The audio device's buffer | `osAiGetLength`, MP3 player pace | H4 |
| The renderer reading back | framebuffer reads, if any | H5 |

The events, and the threads they come from:

| Event | Sent by | When |
|---|---|---|
| VI (retrace) and AI | VI thread | every 1/60 s of real time; a late VI thread *skips* VIs (`new_total_vis` comes from the time) |
| Timers | timer thread | at the deadline, in real time |
| SP done, audio task | SP task thread | when the task finishes |
| SP done, graphics task | graphics thread | when it takes the task from its queue |
| DP done | graphics thread | when the renderer has taken the display list |
| PI (DMA, flash) | the game thread itself (the copy is synchronous) | at once, but through the shared queue |
| SI (controller read done) | the game thread itself | at once, but through the shared queue |

### Measured

A temporary probe over a 2,700-read headless replay (not committed):

- The game goes idle (every game thread waits, and the idle thread runs `pause_self`) about **4
  times per controller read**, so about once per VI.
- **45%** of host events reached the game while it was busy, at whatever OS call came next. Which call
  that is depends on how far the game got before the host thread posted, which is the difference
  H1 saw (`D_8002AC54`, the scheduler's task message, and the queues at `D_8003B200`/`D_8003B218`).
- No game thread polled for long: at most 3 non-blocking receives failed between two idle points.
  `conker_spin_wait_pass` (the 1 ms yield) didn't run at all.

## The design: a virtual timeline, events delivered when the game is idle

While the harness is used, the runtime runs the game as a discrete-event simulation:

1. **Game code takes no time.** Virtual time only moves when the game is idle. Then the runtime moves
   it straight to the next event. This is how the game already behaves on a fast host: its work for
   a frame takes a fraction of a VI, then it waits.
2. **Host events are delivered only at delivery points.** The idle thread (`pause_self`) is the main
   one: by then every game thread waits, so game memory is the same in every run. There the runtime:
   1. finds the next event time `T`: the next VI (`k` × 1/60 s), the earliest timer deadline, or a
      task completion that is due;
   2. waits in real time for anything those events need (the renderer finishing a display list);
   3. paces: waits until real time reaches `T` (skipped with `--fast`, for headless replays);
   4. sets virtual time to `T` and delivers every event due at `T`, in a fixed order: SP, DP, timers
      (by deadline, then address), VI (with the VI register swap), AI;
   5. lets the woken threads run (`check_running_queue`) until the game is idle again.
3. **Messages the game sends itself** (PI, SI, flash) keep arriving at the next OS call, as today, but
   in a queue of their own, so host events can't get in between them.

### Each event

- **VI and AI.** They come from the timeline, not the VI thread. The VI register swap (`update_vi`)
  and the screen update happen at the virtual VI too, so `osViGetCurrentFramebuffer` gives the same
  answer in every run. No VI is ever skipped: a slow host makes the game run slower, not different.
  (The VI thread keeps doing the dummy VIs before the game starts.)
- **Timers.** They run out on the timeline. The timer thread isn't used, and H2's table of
  real-time deadlines goes away.
- **The clock (H2).** A frame's base becomes the timeline's time, not the controller read count, so
  the clock and the timers are on one timeline. Each read of the clock still moves it a step.
- **The audio task.** It runs on the game thread, inside `osSpTaskStartGo`, and its SP done is due at
  once (delivered at the next delivery point). Its writes to game memory then land at the same point
  in every run, and nothing runs alongside them. The N64 runs it in parallel, so a game that read the
  results before SP done would already go wrong there.
- **Graphics tasks.** SP done is due at once, as today ("the RSP completed instantly"). DP done is due
  at the next VI, and the delivery point waits for the renderer to finish the list first. That keeps
  a frame of overlap between the game and the renderer. Headless, it never waits.
- **The two host patches.** `conker_spin_wait_pass` becomes a delivery point that moves virtual time
  1 ms on, or to the next event if that's sooner: what "yield for up to 1 ms" means. The 500 ms song
  mark is timed on virtual time.

### A game that never goes idle

If game threads run for a long time without the game going idle (a long load, or a loop polling a
queue without blocking), events would wait too long. So a deterministic limit applies: after a fixed
number of OS calls without going idle (say 100,000), the next non-blocking `osRecvMesg` is a delivery
point too. The probe never came near this, so a run that hits it logs it.

### What changes for the game

In deterministic mode, events no longer interrupt the game partway through a frame. They wait until
the game is idle (or until a yield, or the limit). The N64 interrupts at any instruction, and the
runtime today interrupts at any OS call, so this is a third behaviour. The game already copes with
the runtime's: it can't rely on being interrupted in game code. The probe shows the game reaches
idle every VI, so a frame's events reach it at most one burst of work late. Normal mode keeps
today's behaviour.

## Netplay

Every peer runs the same timeline from the same inputs, so the same events arrive at the same
points. The lockstep wait (H8) happens at a controller read: a real-time wait inside a game thread.
That doesn't move virtual time, so it can't change the game either. A peer that falls behind runs
slower, with the same game.

## Save states

Save states are already taken and loaded at the idle point (`pause_self`), which is now a delivery
point. The timeline (virtual time, the next VI, events due) is saved with the timers, so a state
taken in deterministic mode goes on the same way after loading. States taken in normal mode still
load. In deterministic mode the timeline starts again from the state's clock.

## The runtime patch

- A new file, `ultramodern/src/timeline.cpp`, holds the timeline: the virtual time, events due, the
  delivery point, pacing and the OS-call limit.
- Small hooks elsewhere, each behind one `if (deterministic)`: the VI thread (no VI or AI messages),
  the timer thread (idle), `pause_self` and `yield_self_1ms` (delivery points), `enqueue_external_message_src`
  (host events go to the timeline, the game's own messages to their own queue), `submit_rsp_task`
  (the audio task runs inline), and the graphics thread (DP done to the timeline).
- About 300 lines in all, most in the new file. With the harness off, every hook takes today's path.
- The API: `ultramodern::set_deterministic(bool fast)` before the game starts, like
  `set_virtual_clock` (which it replaces).

## To agree with the owner

1. In deterministic mode only, events are delivered when the game is idle (or yields, or hits the
   limit), not at any OS call.
2. The audio RSP task runs on the game thread in deterministic mode.
3. Graphics DP done is due at the next VI (a frame of overlap with the renderer), not at once.
4. VIs come at exactly 60 Hz of virtual time (as today's VI thread), not 59.94.
5. All of it is in `recomp/n64modernruntime.patch`, like H1 and H2, rather than going to
   N64ModernRuntime.

## The steps, once agreed

1. The timeline, with VI, AI and timers delivered at idle points, and the clock from the timeline.
   Two normal replays should match with no one-off reads.
2. The game's own messages (PI, SI) in their own queue.
3. The audio task inline; graphics SP and DP done on the timeline.
4. The two host patches on virtual time; the OS-call limit; save states; `--fast`.
5. **Done when:** a replay on one busy core (`taskset -c 3` next to a busy loop) matches a normal
   one read for read. A `--fast` replay should match a paced one too.

H4 (the audio device) and H5 (the renderer) come after: windowed replays need both.
