# Mission: one window, shared frames (replacing the XEmbed container)

**Status:** proposed — awaiting approval. Supersedes `tools/quadview` (the
XEmbed stopgap) once it passes acceptance.

## Why

Four processes each owning a borderless window works, but the desktop sees four
windows, and the XEmbed container that papered over it only works on
X11/XWayland, is deprecated, and got fragile the moment a seat failed to boot.
Wayland-native is the future default, and `--size/--position` are already X11.

The durable shape is **processes render into shared memory; one viewer owns the
only window**. That is true on Wayland and X11, needs no window-manager tricks,
and the viewer survives a future move to an in-process quad-system — it consumes
frames and knows nothing about who produces them.

## Goal

One window, one taskbar entry, one alt-tab, four live seats at 60 fps, input
routed to the correct seat, on **native Wayland**, with no XEmbed and no
`xdotool`.

**Non-goals:** in-process multi-System (later, separate mission), audio mixing
beyond what the four SDL sinks already do, per-game UI, >4 seats, recording.

## Architecture

```
azahar --user slots/p1/user --shm-frame azsh-<session>-1 --input-socket <path>
   │  (renders 60 fps, hidden 1x1 window, no visible UI)
   ├─ SPMC ring in shared memory: header + 3 RGBA8 slots
   └─ consumes input datagrams (keys/touch) from its socket

azahar-viewer --seat 0:azsh-<session>-0 --seat 1:... --layout 2x2
   └─ one window, one GL context, four quads, presents at display rate
```

### Frame ring (per seat, POSIX shm, unlinked on close)

```
offset 0   magic "AZSH" | version u32 | width u32 | height u32
          stride u32 | format u32 (RGBA8) | slot_count u32 (3) | slot_bytes u32
offset 32  seq u64          // writer increments after finishing a slot
offset 40  request u64      // viewer increments to ask for a fresh frame
offset 64  slots[3]         // width*height*4 each, 64-byte aligned
```

Writer loop (in `azahar`, on the present thread): if `request` moved, call
`system.GPU().Renderer().RequestScreenshot(slot, cb, layout)` into
`slots[(seq+1)%3]`, then `seq++` with a release store. Reader copies the slot
whose `seq` is current and re-checks the sequence after the copy (seqlock
style), so a torn frame is never shown. Pull-based: the producer only reads back
frames the viewer will actually present, so a 60 Hz monitor costs 60 readbacks
per second per seat, not 240.

### Input (per seat, unix datagram socket)

24-byte messages, `{type u8, pad, key u32, x u16, y u16}`:

| type | meaning | lands on |
|------|---------|----------|
| 1/2  | key down / up (`Qt::Key` code) | `InputCommon::GetKeyboard()->PressKey/ReleaseKey` (same call `GRenderWindow::keyPressEvent` makes) |
| 3/4/5| touch down / move / up (viewer-local coords) | `EmuWindow::TouchPressed/TouchMoved/TouchReleased` |

Gamepads stay exactly as they are: each instance reads its own SDL device, so a
pad per seat never passes through the viewer.

## Code touch points

| Area | File | Change |
|------|------|--------|
| Producer | `src/citra_qt/citra_qt.cpp` (`LaunchQtFrontend`, flag parsing) | `--shm-frame <name>`, `--input-socket <path>`; force the window to 1x1 and off-screen |
| Producer | `src/citra_qt/bootmanager.cpp` / `.h` (`GRenderWindow`) | frame ring writer on the present thread (reusing `Renderer::RequestScreenshot`), socket reader on the GUI thread |
| Viewer | `tools/viewer/` (new, standalone Qt6 + OpenGL) | one window, 4 textured quads, click-to-select seat, key/touch forwarding, fps/HUD counters |
| Orchestrator | `src-tauri/src/process.rs` | spawn seats with the two flags, spawn the viewer once, teardown order, drop `spawn_viewer` for quadview |
| Cleanup | `tools/quadview/` | delete once the viewer passes acceptance — one single-window path only |

## Phases

1. **P1 — frame ring (producer only).** `--shm-frame <name>`; verify by reading
   the ring from a throwaway `python3`/`xxd` consumer: 400x480 RGBA, seq
   climbing at 60/s while the game runs. *Done when* a dumped frame is a correct
   PNG of the game.
2. **P2 — viewer window.** One window, 2x2 tiles, four rings at once. *Done when*
   four seats render in one window at ≥58 fps presented, no tearing.
3. **P3 — input.** `--input-socket` + viewer forwarding. *Done when* keyboard and
   touch reach only the selected seat (verified by START opening the menu on that
   seat only, as the XEmbed test did).
4. **P4 — orchestrator integration.** Start/Stop spawn and clean up viewer and
   sockets; remove the quadview spawn. *Done when* `./run.sh` → Start → one
   window, Stop → nothing left running (`azahar`, `viewer`, `azahar-room` all 0,
   shm objects unlinked).
5. **P5 — measurement & docs.** `tools/qa/monitor.sh` also samples the viewer
   (presented fps, CPU), `history.md` gets the numbers, `todo.md` ticked.

## Acceptance (all must pass to delete the stopgap)

- Four seats in one MK7 race through the viewer, `--stats` reporting ≥59 fps on
  every seat, viewer presenting ≥58 fps, one window in `wmctrl -l`.
- Runs on **native Wayland** (`QT_QPA_PLATFORM=wayland`), no XWayland needed for
  the seats or the viewer.
- Input isolation: keys/touch affect the selected seat only; each pad still
  drives its own seat.
- Teardown leaves no processes, no shm files, no sockets.
- Overhead recorded against the four-window baseline already measured
  (805–827 MB RSS, 85–88 % CPU per instance during a race): per-seat CPU and
  viewer CPU documented, with readback cost called out plainly.

## Risks and mitigations

- **Readback stalls the GPU pipeline.** Start with triple-buffered
  `RequestScreenshot` and measure; if per-seat frame time rises more than ~1 ms,
  switch the producer to PBO async readback, and only then consider EGL/dma-buf
  zero-copy.
- **Bandwidth:** 400x480x4 = 750 KB/frame/seat → ~180 MB/s at 4x60. Fine for
  memory bandwidth; the pull model keeps it at display rate.
- **Hidden window on Wayland:** a mapped 1x1 window off-screen-ish is the cheap
  path; if a GL context needs a real surface, fall back to an explicit FBO +
  `QOffscreenSurface`. Decided in P1.
- **Shm naming:** every object is `<session>-<seat>` and unlinked on close; a
  stale object from a crash is detected by the viewer (magic + version) and
  reported, not adopted.
- **Viewer pacing:** present on a timer at the display refresh; never block on a
  seat (skip a tile if its seq did not move).

## Effort

P1 1–2 days, P2 2–3, P3 1–2, P4 1, P5 1 → **~1.5 weeks** of focused work, with
P2 being the first point where the "one window" claim is visually true.

## Open decisions (recommendations in bold)

1. **Viewer toolkit:** Qt6 + OpenGL (same stack as the emulator, no new
   dependency) **vs** SDL2 + GL (smaller, already linked). Recommend Qt6 — we
   already build and debug it.
2. **Frame format:** RGBA8 with CPU readback **vs** NV12/YCbCr to halve the
   bandwidth. Recommend RGBA8 first; it is 1:1 with what the renderer writes.
3. **Where the input socket lives:** one socket per seat **vs** one socket for
   all seats. Recommend one per seat — the instance owns its own socket, and
   the viewer never needs to know seat-to-process mapping beyond the ring name.
