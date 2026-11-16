# Azahar Splitscreen

**Couch split-screen for Nintendo 3DS local multiplayer, in one window.**

Azahar Splitscreen is a fork of [Azahar](https://github.com/azahar-emu/azahar) (the Citra
successor) plus a small Tauri orchestrator. It runs 2–6 isolated Azahar instances — one
"console" per player — links them through Azahar's own network room, and composites all of
their screens into a **single window** driven by shared memory. No per-seat windows, no
window tiling, no window-manager tricks.

*Not affiliated with or endorsed by Nintendo. No ROMs, keys or NAND dumps live in this repo.*

---

## Why this exists (and what upstream Azahar does not do)

Upstream Azahar emulates **one** 3DS, in **one** window. That is the right shape for playing a
single-player game, and it is exactly the wrong shape for a couch session: 3DS local
multiplayer means one console per player, so four people on a couch need four consoles, four
identities, four sets of saves, four windows to arrange, and one audio device that suddenly
receives four copies of the same soundtrack.

Azahar Splitscreen owns that whole problem:

| Upstream Azahar | Azahar Splitscreen |
| --- | --- |
| One console, one window | 2–6 consoles, **one** window (one taskbar entry, one alt-tab) |
| You open each instance yourself | A picker starts the room and every seat in one click |
| Windows you position and tile | Seats render **headless** into shared-memory rings; a viewer composites them |
| Same user data for every instance | One isolated user dir per seat (NAND, `SecureInfo_A`, saves, config, shader cache) |
| Every instance plays its own audio | Seat 1 owns the output; the other seats open **no** audio stream |
| No way to tell the screens apart | Per-player colored outline + `P<n>` badge (palette from [mgba-splitscreen](https://github.com/)'s player tags) |
| Nothing to quit in one action | Closing the one window tears down the room, every seat and the viewer |

The emulator itself is upstream: the fork's patch surface is deliberately tiny (a handful of
frontend flags, the shared-memory frame ring, the input socket, and the headless seat path).
Everything couch-specific lives in `src-tauri/` and `tools/viewer/`.

## What a session looks like

```
azahar-splitscreen  (Tauri picker)
   │  pick ROM + 2–6 players, press Start; the picker hides
   ├── azahar-room                loopback UDP, one room for the session
   ├── azahar --shm-frame azsh-<pid>-1 --input-socket "slots/Player 1.sock" --user slots/p1/user
   ├── azahar --shm-frame azsh-<pid>-2 …                                  (one per seat, headless)
   └── azahar-viewer              the session window: composites every ring, routes input
        └─ closing this window drops the session, kills the seats and exits
```

- Each seat publishes RGBA frames into its own POSIX shared-memory ring (3 slots, sequence
  counter) and receives keyboard/touch datagrams on its socket.
- The viewer draws the rings into a grid — 2 seats `2x1`, 3–4 `2x2`, 5–6 `3x2` — at the
  display rate, with each player's color on the tile edge and on the badge.
- With 3+ seats the tiles show the **top screen only** (the action screen); each player holds
  their own peek key (`F5`–`F10`, or a pad's left shoulder) to see their touch screen.
- Two seats still show both screens, as before.

## Controls

| | |
| --- | --- |
| `F5`…`F10` | Hold to peek at that player's touch screen |
| `Tab` | Toggle top-screen-only / both-screens layout |
| Mouse | Taps go to the seat under the pointer |
| `Esc` / close the window | Quit (everything is torn down) |

Gamepads are enumerated by the picker and their buttons are forwarded to the seat they were
assigned to; without one, the keyboard works.

## Build and run

```bash
# Qt6 + Ninja + CMake >= 3.25 + SDL2 + the Rust toolchain
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_LTO=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
ninja -C build -j 4                                    # emulator + azahar-room
ninja -C tools/viewer/build                             # the one-window viewer
(cd src-tauri && cargo build --release)                 # the picker/orchestrator

./src-tauri/target/release/azahar-splitscreen           # pick a ROM, players, Start
```

QA/terminal path (same session, no picker, foreground, `Ctrl-C` or close the window to stop):

```bash
./src-tauri/target/release/azahar-splitscreen --start "Test Roms/<game>.3ds" --players 2
```

Slots, save sets, per-seat fps and logs land in `slots/`.

## Status

Verified on this dev box (Linux, Wayland/XWayland, 15 GiB RAM, 8 threads), all from real
app-driven runs — details and captures in `docs/evidence/`:

- **One window.** After Start there is exactly one session window; the picker hides. Closing it
  leaves no orchestrator, room, seat, emulator or viewer process, no `/dev/shm/azsh-*` rings
  and no audio streams.
- **Six seats** ran Mario Kart 7 in one window with every seat at `60.0 fps`, speed ≈100
  (load 8.7 on 8 threads, 8 of 15 GiB) — working, but at this machine's edge.
- **Top-screen-only** at 3+ seats with working per-seat peek; two seats keep both screens.
- **One audio stream:** seat 1's; every other seat uses Azahar's null sink.
- **Save data:** each player's own set, one shared set for the group, or a per-seat import.
- **Link layer:** Azahar's own room (UDS relay) with real counters between two instances.

Known limits, stated plainly:

- **Two titles boot on this box**: Mario Kart 7 and Monster Hunter Stories. The other four test
  dumps failed with "Failed to determine system mode" and were removed; every QA claim above
  rests on those two titles.
- **Save states are not implemented** (save *data* sets are).
- **Gamepads are optional and per-seat**; nothing is invented when none is attached.
- **Per-seat ROMs are deliberately unsupported**: every seat in a session plays the same game.
- The 4–6 seat audio headroom question is open: the mix can reach full scale at the default
  system volume.
- In-process seats (one process, N cores) were investigated and rejected — see
  `plans/in-process-seats.md`.

## Documentation map

| File | What it holds |
| --- | --- |
| `docs/one-window-flow.md` | How the shipped one-window shared-memory flow works, end to end |
| `docs/evidence/` | Per-feature measurements, logs and screenshots behind the status above |
| `history.md` | Dated engineering log with verdicts and numbers |
| `todo.md` | What is done, what is open, what is deliberately not happening |
| `AGENTS.md` | Orientation for agent sessions, rules and gotchas |
| `plans/` | The two architecture spikes and their verdicts |

## Relationship to upstream

This is a downstream fork of `azahar-emu/azahar`; the upstream README, installation
instructions, release channels and compatibility reporting all still apply to the emulator
itself. Fork changes are kept small and reviewed by hand, and Azahar's `AI-POLICY.md` applies
to anything proposed upstream.
