# Mission: four seats in one process (go/no-go spike)

**Verdict, 2026-11-14 (second pass): no-go.** The presentation model was not the
blocker. With each seat publishing its own ring from its own timer and one
compositor window, three of four seats still render at 54–57 fps while one seat's
**guest application exits on its own** (`svcExitProcess` → status 16,
`ShutdownRequested`). The same four seats as four *processes*, sharing one user
profile, all publish. So the wall is remaining process-global emulator state, plus a
process-global input stack — both architectural, not harness bugs.

## What the spike is

`--inproc-seats N <rom>` (`src/citra_qt/inproc_seats.{h,cpp}`): N `Core::System`
instances, N `EmuThread`s and N hidden `GRenderWindow`s in one process, each seat with
its own render context, shared-memory ring and input socket. This widget is the single
window: it draws every seat's ring with QPainter, 2x2, and forwards clicks to the
clicked seat's socket. A 1 s timer logs `seatN fps/seq/hash` plus process CPU and RSS.

## What the second pass changed

- **Presentation:** no seat starts a present thread any more; there is no shared
  present path at all. Each seat's own `GRenderWindow` publishes its ring from its own
  timer, and the single window composites the rings in software. This is the
  "own presentation loop per seat" lever.
- **Teardown:** stop wakes the frame limiter (`NotifyPendingWork`), waits on the thread
  in bounded steps and terminates only as a last resort, so `EmuThread::~EmuThread`
  never destroys a condition variable a live thread is waiting on.
- **Boot staggering:** seats boot 8 s apart (the orchestrator staggers its spawns for
  the same reason).

## Measured, one clean run (`--inproc-seats 4`, xcb, 50 s)

```
multiseat: seat1 core stopped, status=16 ()
multiseat: t=47s seat0 fps=57 seq=2061 hash=474fb9f7 seat1 fps=0 seq=0 hash=00000000
                seat2 fps=54 seq=1192 hash=474fb9f7 seat3 fps=57 seq=762 hash=474fb9f7
                | live=3/4 cpu=74% rss=1871MB
after SIGTERM: procs=0 rings=0 socks=0        (process gone within 1 s in a second run)
```

- **Rendering works and is per-seat:** three seats publish at 54–57 fps; their hashes
  differ whenever their screens differ (e.g. `474fb9f7 / 9610e141 / 8ffc9bd7` in one
  sample), so they are independent renderers, not one image drawn three times.
- **One process:** 46 threads, 1 871 MB RSS, ~74 % CPU for three live seats.
- **The casualty is a dead system, not a starved frame:** the lost seat reports
  `IsPoweredOn() == false` and `status=16` (`ShutdownRequested`, returned when the guest
  calls `svcExitProcess` or asks APT for a restart), and its emulation thread has exited.
- **Teardown is fixed:** SIGTERM → 0 processes, 0 rings, 0 sockets, no leaked threads.
- **Input is not proven:** taps at the bottom screen (both `(440,195)` on a 480x240
  layout and `(200,360)` on the stacked 400x480 one) changed no seat's frame.

## The discriminating experiment

The seat casualty looked like profile contention, so the same ROM and the same user
directory were run as **four separate processes** (`--shm-frame`, no `--user`):
all four published rings (seq 2528 / 2320 / 2205 / 2051 over 40 s). A single
in-process seat also runs normally (59.0 fps emulation, 55 fps ring). So sharing the
profile is fine and being in one process is what ends the seat.

## Why no-go

1. **Remaining process-global state in the emulator ends a guest.** One (sometimes two)
   of N Systems has its application exit right after boot; the same configuration in
   its own process does not. That is state shared between Systems, and it is the same
   class of problem as the singleton but not yet enumerated: the `InputCommon` statics,
   `Common::Hacks::hack_manager`, `Core::PerfStats::game_frames_updated`, the service
   module registry and the shader/disk-resource caches are all candidates.
2. **Input is process-global for keys.** `ApplyForwardedInput` calls
   `InputCommon::GetKeyboard()`, a process-wide keyboard: a key sent to one seat's
   socket is visible to every seat. Touch goes through the seat's own `EmuWindow`, so
   touch is per-seat in principle, but it was not demonstrated here.
3. Memory is the only clear win (1.9 GB total vs ~3.4 GB for four processes); CPU is a
   wash (~74 % for three seats vs ~150 % for four processes).

## Conditions for a go

- Find and remove the shared state that ends a guest seat: start the two-seat case and
  break on `Core::System::SendSignal` / `svcExitProcess` to catch the caller, then
  bisect the globals above. Four seats must survive 60 s.
- Make input per-seat: input device state must belong to a `System`, not to
  `input_common`'s file-scope statics, and a seat's socket must not move another
  seat's pad. A tap must change exactly one seat's frames.
- Keep the loop that is already proven: thread-local `Core::System::GetInstance`,
  one GL context per window, one ring and one socket per seat, one compositing window.

## Re-run

```bash
ninja -C build -j4 bin/Release/azahar
DISPLAY=:1 QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR=1 QT_ENABLE_HIGHDPI_SCALING=0 \
  ./build/bin/Release/azahar --inproc-seats 4 "<rom>" 2>&1 | grep -E "multiseat"
# rings: /dev/shm/azsh-inproc-N   input sockets: /tmp/azsh-inproc-pN.sock
# tap a seat: python3 - <<'PY'
# import socket, struct
# s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
# s.sendto(struct.pack('<B3xIHH', 3, 0, 200, 360), '/tmp/azsh-inproc-p0.sock')
# s.sendto(struct.pack('<B3xIHH', 5, 0, 0, 0), '/tmp/azsh-inproc-p0.sock')
# PY
```
