# Mission: four seats in one process (go/no-go spike)

**Verdict, 2026-11-14: no-go for now.** The *core* is no longer the blocker — four
`Core::System` instances boot and run side by side in one process (that took a 5-line
change), and three of them render distinct, advancing frames simultaneously in one
window. What does not hold up is the *presentation and lifecycle* layer: one seat of
the four never produces a frame, shutdown hangs, and one run died with a SIGSEGV.
The four-process + shared-memory-ring design stays; the conditions below are what an
in-process rework would have to satisfy first.

## What was built

`--inproc-seats N <rom>` (new `src/citra_qt/inproc_seats.{h,cpp}`) boots N seats in
the one process: N `Core::System`, N `EmuThread`, N `GRenderWindow` (one per seat,
tiled 2x2 in one window), each seat keeping the production producer path (its own
shared-memory ring and input socket) so the comparison is like for like. A 1 s timer
logs `seatN fps/seq/hash` plus process CPU and RSS.

Core changes the spike needed:

- `Core::System::GetInstance()` now returns a **thread-local current** System, falling
  back to the process-wide instance (`Core::System::SetCurrent`). Set in
  `EmuThread::run` and the present thread; the singleton path (normal frontend) is
  unchanged.
- `GRenderWindow::main_context` was a process-wide static, i.e. **one GL context for
  every window in the process**. It is now one context per window, which is the thing
  that lets seats render on their own threads at all.
- Frame limiter: the frontend normally releases frame-advance mode after the loading
  screen; a seat that is never released waits forever before its first frame. The
  spike releases it per seat.
- `Service::AttemptLLE` no longer throws when no config was ever loaded (an empty
  `lle_modules` map means HLE, which is what `false` meant).

## What was measured (one 60 s run, xcb, `--inproc-seats 4`)

| seat | ring fps | frame seq | hash advances |
|---|---|---|---|
| 0 | 54–57 | 3105 | yes |
| 1 | **0** | **0** | **never published a frame** |
| 2 | 53–57 | 3110 | yes |
| 3 | 53–58 | 3088 | yes |

- Distinct frames: all three live seats hash differently and keep advancing
  (`...474fb9f7` / `...2fb159b4` / `...e9458266` over time), so they are independent
  renderers, not one seat drawn three times.
- Process totals: **1 900 MB RSS, 34 % CPU**, one process, four "EmuThread" and four
  "Present" threads.
- Input: a tap at frame (440,195) sent to seat 0's socket changed **no** seat's frame
  (`0.00` on all three live seats), so input routing was **not** proven in-process.
- Shutdown: SIGTERM leaves the process alive (>8 s) with rings and sockets still
  present; the hang is in `EmuThread::~EmuThread` destroying a condition variable of
  a thread that never joined.

Four-process reference (same machine, `history.md` (12)/(13)/(15)): 4 seats 59–60 fps
each, rings 49–53 fps, ~700–880 MB RSS **per** seat, ~35–40 % CPU **per** seat
(≈150 % total), teardown to 0 processes / 0 rings / 0 sockets every time.

## Why no-go

1. **Presentation is the blocker, not core state.** The stuck seat has a live
   emulation thread and a live present thread but its screenshot request never
   completes, so its ring stays empty — a per-seat readback/present race in the Qt
   seat path (`GRenderWindow` was written for one window per process).
2. **Shutdown is not reliable** with several seats (join/teardown ordering).
3. **One run crashed** (SIGSEGV) with four live contexts.
4. Memory is *better* in-process (1.9 GB total vs ~3.4 GB), but CPU per seat is not:
   ~34 % for three seats is in the same range as 35–40 % per seat across four
   processes, so the expected win is memory and window/taskbar tidiness, not speed.

## Conditions for a go

- Give each seat its own `EmuWindow`/presentation loop instead of `GRenderWindow`'s
  single-window present thread, so screenshots and presentation never contend.
- Prove 4/4 seats at 59–60 fps with advancing per-seat frame ids for 60 s.
- Make stop deterministic: request shutdown, wake the frame limiter, join every
  emulation and present thread, then destroy contexts.
- Re-check input routing per seat (the tap test has to change exactly one seat).

## Re-run

```bash
ninja -C build -j4 bin/Release/azahar
DISPLAY=:1 QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR=1 QT_ENABLE_HIGHDPI_SCALING=0 \
  ./build/bin/Release/azahar --inproc-seats 4 "<rom>" 2>&1 | grep multiseat
# rings at /dev/shm/azsh-inproc-N, input sockets at /tmp/azsh-inproc-pN.sock
```
