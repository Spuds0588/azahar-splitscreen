# The shipped one-window shared-memory flow

**Status: shipped and verified (2026-11-16).** Everything below was observed in real
app-driven runs, not inferred; the numbers and captures are in `docs/evidence/` and
`history.md` (41)–(45).

## What exists at run time

```
azahar-splitscreen                      the picker (Tauri window, label "main")
 │
 ├─ azahar-room                          loopback UDP room, 127.0.0.1:<free port>
 │
 ├─ azahar --user slots/pN/user \
 │         --shm-frame azsh-<pid>-N \
 │         --input-socket slots/Player N.sock \
 │         --join-room 127.0.0.1:<port> --player-name "Player N" \
 │         --stats slots/Player N.fps <rom>          one per seat, headless
 │
 └─ azahar-viewer                        the only mapped window in the session
```

The seat processes never map a window: `--shm-frame` renders into shared memory and sets
`WA_DontShowOnScreen`. Before Start the picker is the only window; after Start the viewer is.
There is no point at which the app owns two windows.

## Lifecycle

1. **Picker.** Lists the dumps in `Test Roms/`, the gamepads the kernel exposes, the players
   count (2–6) and the save mode. It is the entry point, not a session window.
2. **Start** (`start_session` in `src-tauri/src/main.rs`):
   - `slots::prepare` writes `slots/pN/user/config/qt-config.ini` for each seat: standard
     layout, no pause when unfocused, `[Audio] volume=1`, and for seats 2+ `output_type=1`
     (Azahar's null sink → no audio stream).
   - `saves::apply` seeds save sets when the picker asked for them.
   - one `azahar-room` on a free loopback port, `--max_members players+1` (one spare so a
     restarted seat can rejoin).
   - seats spawn headless, two seconds apart, each with its own ring, socket, user dir and
     name; `--stats` writes one fps/speed/frame-time line per second.
   - `check_boots` fails fast (one notice window, nothing left running) if a dump cannot boot.
   - `spawn_viewer` opens the one window; the picker is then **hidden**, not closed.
3. **Session.** The viewer attaches each ring by name, maps the header and its three slots,
   and composites the tiles at display rate. Input (keys, touch, pad buttons) is forwarded over
   each seat's socket; frames arrive through shared memory only.
4. **Quit.** Closing the session window is the only quit: the close handler drops the session,
   which terminates the viewer, every seat and the room (SIGTERM to each process group, then
   SIGKILL), unlinks the rings and sockets, and exits the orchestrator. The `--start` path
   behaves the same: it stops when the viewer window closes, or on `Ctrl-C`.

## Frame path

Per seat, a POSIX shared-memory object `/dev/shm/azsh-<orchestrator pid>-<seat>`:

```
offset 0   magic "AZSH" | version | width | height | stride | format (RGBA8) | slots (3)
offset 32  seq     writer increments after publishing a slot
offset 40  request viewer increments to ask for a fresh frame
offset 64  slots[3]  width*height*4 each, 64-byte aligned
```

The writer publishes into the slot `seq % 3` and bumps `seq`; the viewer reads the newest
complete slot. A replaced ring (a restarted seat) is detected by inode and re-attached, and a
seat that switches from its boot screen to the real one changes the header size, which also
causes a re-attach — both are logged as `viewer: ring … re-attaching`.

Measured on this box: six MK7 seats each at `fps=60.0 speed≈100`, viewer
`presented_fps≈62.5, paint_ms≈8` (`docs/evidence/six-seat-top-only.txt`).

## Layout and identity rules

- 2 seats: `2x1`, both screens stacked per tile. 3–4 seats: `2x2`. 5–6 seats: `3x2`.
- 3+ seats open **top screen only** (the action screen); holding a seat's peek key
  (`F5`–`F10`, or that pad's left shoulder) shows its touch screen. `Tab` toggles the layout.
- Every tile carries the player's color on its edge and on its `P<n>` badge — P1 red, P2 blue,
  P3 green, P4 orange (mgba-splitscreen's palette), P5 purple, P6 cyan.
- Input goes to the seat under the pointer, or to the seat that owns the pad.

## Audio

One device, one stream: seat 1. Seats 2+ use Azahar's null sink, so they render audio for the
game's timing but open no stream. Verified: a six-seat session lists exactly one
`Stream/Output/Audio` node, owned by `slots/p1`.

## Reproduce

```bash
ninja -C build -j 4 && ninja -C tools/viewer/build && (cd src-tauri && cargo build --release)
./src-tauri/target/release/azahar-splitscreen          # pick ROM, players, Start
```

What a healthy run looks like: `slots/session.log` has `room: open`,
`viewer grid: <c>x<r> cells`, one `spawned headless` line per seat and
`viewer: one window compositing all seats`; `slots/Player N.fps` gains a line per second;
`wmctrl -l` shows exactly one `Azahar Splitscreen` window; after closing it, `pgrep` finds no
orchestrator/room/seat/viewer, `ls /dev/shm/azsh-*` is empty and PipeWire has no output streams.

## Deliberately not supported

- Per-seat ROMs: every seat in a session plays the same game (owner's decision).
- Per-seat windows or window tiling: the viewer is the window.
- In-process seats: the spike was a measured no-go (`plans/in-process-seats.md`).
