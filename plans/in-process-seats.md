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

## Input, measured end to end

The gap this closes: earlier passes could not show that a tap reaches a seat's guest.
It now does, it is consumed by a guest that reads the touch panel, and the title
responds on a screen that is touch-operable.

**Delivery, four seats in one process.** A tap is a datagram to that seat's socket
(`type 3` = touch press at framebuffer x,y, `type 5` = release); `Shutdown` of the
path is: socket -> `GRenderWindow::ApplyForwardedInput` -> `EmuWindow::TouchPressed`
-> per-System HID service. One clean run (`--inproc-seats 4`, xcb, frame 400x480,
bottom screen `(40,240)-(360,480)`), tap `(200,400)` sent to `/tmp/azsh-inproc-p0.sock`:

- receipt and acceptance on **seat 0's window only**: `touch win=seat0 x=200 y=400
  accepted=1 frame=400x480 bottom=(40,240)-(360,480)`;
- emulation-side receipt for **seat 0's System only**: HID reported
  `pressed=1 x=160 y=160` (`x`/`y` are bottom-screen pixels: 0.5 x 320, 0.667 x 240),
  then `pressed=0`; the other three seats' HID services logged nothing;
- the run itself was **4/4 live at 53-57 fps**, one process, 2 291 MB RSS, ~96 % CPU;
- SIGTERM: `stopped 4 seats`, **0 processes, 0 rings, 0 sockets**.

Repeated with the instrumentation removed (the numbers above are from the receipt run;
this one is the stripped build): 4/4 live at 54-57 fps, 2 326 MB RSS, ~96 % CPU, the
tapped seat's hash differs for one sample with the animated top screen and returns to
the same static frame as the other three, and SIGTERM again leaves 0 processes, 0 rings,
0 sockets with no crash.

**Two things were dropping the event.** Both are fixed in the tree:

1. The spike loaded no input profile, so `Settings::values.current_input_profile`
   was empty and HID's `LoadInputDevices` created **no devices at all**
   (`CreateDevice("")`); touch and keys were silently discarded. The spike now sets
   `engine:emu_window` plus `engine:keyboard,code:77` (START) and `code:65` (A).
2. The touch factory was process-global (`global_touch_state` in `emu_window.cpp`):
   every window shared the first window's `TouchState`, so one seat's tap would move
   every seat's cursor. Each `EmuWindow` now registers `emu_window_<ptr>` and
   `Core::System` carries a touch-device override (`SetTouchDeviceOverride`) that HID
   prefers over the global profile, so each seat's HID binds its own window.
   `engine:emu_window` still points at the first window, so single-window configs and
   the four-process design are unchanged.

**A homebrew responder proves the path.** `tools/touchprobe` (`tools/touchprobe/out/`,
built with `tools/touchprobe/build.sh`) draws exactly what libctru reads from the
emulated panel, so it is a title that responds by construction. Run in a seat, before
a tap it shows `x=0 y=0 held=0 presses=0`; after one datagram `(200,360)` it shows
`x=160 y=120 held=0 presses=1` (bottom screen `(40,240)-(360,480)`, so panel
`x=(200-40)=160`, `y=(360-240)=120`), and the ring goes `2a30b3b2 -> 2f08c5e4` and
stays. A second tap at `(120,340)` reads back `x=80 y=100 presses=1`. Re-run on the
stripped build gives the same result, so no instrumentation is needed to reproduce it.

**Entry timing is identical for the games.** HID logs 192 pressed entries for a 0.8 s
hold in both cases - one every ~1.1 M ticks, ~4.1 ms, 240 Hz - with the coordinates
above and no gaps. So timing is not what separated MK7 from a responding title.

**Mario Kart 7 takes the touch where the target is touch-operable.** On the kart-parts
screen, a tap at `(330,275)` (HID `(290,35)`) moved the selection from the wheels
column to the glider column and the top screen changed from `Normal` to `Super Glider`;
hashes `bb95a2d9 -> cdf42ef0 -> b379ada1` with no key sent between the dumps.

**The earlier "guest ignores touch" result is retracted.** Those four taps were
mis-targeted or aimed at screens that only take buttons: the 150cc tap at panel `y=150`
is below the 150cc row (rows sit at `y≈25/75/125`); the Mii dialog's OK answers to
A/START; the mode-menu tap only highlights a row; and the run recorded as "character
grid" was actually on the engine-class screen, so the grid was never tapped. Nothing
about the input path was wrong: touch is per-seat and consumed.

Two limits remain, measured the same way:

- **Keys cannot be per-seat.** `ApplyForwardedInput` presses `InputCommon::GetKeyboard()`,
  a process-global, so a forwarded key moves every seat at once; only touch is
  addressable per seat today.
- **A per-seat picture only exists at one seat.** With 2 seats both rings publish a
  blank frame (uniform, 2 distinct byte values); with 1 seat the ring carries the real
  screen (88 distinct values). Sequence numbers keep advancing at 53-57 fps either way,
  so the four-seat visual comparison in the done-bar cannot be made yet.

## Conditions for a go

- Find and remove the shared state that ends a guest seat: start the two-seat case and
  break on `Core::System::SendSignal` / `svcExitProcess` to catch the caller, then
  bisect the globals above. Four seats must survive 60 s.
- Make keys per-seat: touch is per-System now (see above), keys are not -
  `InputCommon::GetKeyboard()` is file-scope state in `input_common`, so a forwarded
  key still moves every seat.
- Find out why a ring carries a real picture at one seat but a blank one at two; that
  is the last thing between this spike and a four-seat visual check.
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
# s.sendto(struct.pack('<B3xIHH', 3, 0, 200, 400), '/tmp/azsh-inproc-p0.sock')   # touch at x,y
# s.sendto(struct.pack('<B3xIHH', 5, 0, 0, 0), '/tmp/azsh-inproc-p0.sock')         # release
# frames live in ring slots: header 64 B, w@8 h@12 stride@16 slots@24 seq@32,
# newest frame at 64 + ((seq-1) % slots) * h * stride (RGBA8888, bottom-up)
# the HID entry counts quoted above came from temporary fprintf()s in HID's touch
# block; they are removed from the tree - re-add them to re-measure timing. The
# responder itself needs no instrumentation:
#   tools/touchprobe/build.sh
#   ./build/bin/Release/azahar --inproc-seats 1 tools/touchprobe/out/touchprobe.3dsx
# PY
```
