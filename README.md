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

The session window has a menu bar: **Session** (quit), **View** (both screens / top screen only,
fullscreen), **Controls** (controls reference, gamepad mapping) and **Help**.

| | |
| --- | --- |
| `F5`…`F10`, pad's left shoulder | Hold to peek at that player's touch screen |
| `Tab`, or View menu | Toggle top-screen-only / both-screens layout |
| `F1` | Controls reference: every keyboard key and pad button |
| `F11` | Fullscreen |
| Mouse | Click a tile to select it; a click on its screen is a touch |
| `Esc`, or Session → Quit Session | Quit (everything is torn down) |

The picker's **Screens** choice picks the layout the session opens in; the View menu and `Tab`
change it live, and the viewer opens the next session the way you last left it.

One pad mapping serves every pad in the session: the left stick is the circle pad, the right
stick walks the touch pointer and clicking it taps, and a held left shoulder is that seat's peek.
**Controls → Gamepad Mapping…** re-binds any of the 3DS buttons by pressing the button you want;
the mapping is saved to `slots/pads.json` and survives the session. The picker lists the pads
the kernel reports; without one, the keyboard works.

## RetroArch core

`src/citra_libretro_split` builds `azahar_splitscreen_libretro`, a libretro core that runs one
ROM on 2-6 consoles inside RetroArch. It is a launcher, not a port of the emulator: Azahar's core
cannot hold several instances in one process, so the core does what the session window does.
The boot menu asks how many players to spawn, then the core starts `azahar-room` plus one headless
`azahar` per seat, joins them, and composites their shared-memory rings into the core's frame.
RetroPad port 1 drives P1, port 2 P2, and so on.

```bash
cmake -S src/citra_libretro_split -B build-split-core -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-split-core
retroarch -L build-split-core/azahar_splitscreen_libretro.so "Test Roms/game.3ds"
```

Or configure the main tree with `-DENABLE_LIBRETRO_SPLIT=ON`, which adds the core target.

- The boot menu starts at the **Players per ROM** core option (2–6); left/right changes it and
  Start spawns that many consoles. The menu also says whether the layout is both screens (2) or
  the action screen with a peek (3+).
- The core looks for the emulator in `AZAHAR_SPLITS_ROOT`, one folder above itself, or the
  working directory, so a beta bundle's `cores/` folder works as-is.
- Per player: A/B/X/Y, D-pad, Start, Select, L3 = L, R = R, L2 = ZL, R2 = ZR, left stick = circle
  pad, right stick = touch pointer, R3 taps, and holding L shows that player's touch screen.
- **One audio stream**: the seats open no audio device. Each writes interleaved stereo into its
  own shared-memory ring (`--shm-audio`, `src/audio_core/shm_audio.h`) at the 3DS's native
  32728 Hz, and the core sums those rings into RetroArch's audio callback at 1/sqrt(players), so
  RetroArch's volume, mute and recorder apply to the whole couch. Session slots, logs and sockets
  land in RetroArch's save directory under `azahar-splitscreen/`.
- **A missing seat says why**: a dump that cannot boot (still encrypted, unreadable) or a seat
  that exits before its first frame is reported in words on that player's tile, read from the
  seat's own logs, instead of leaving a black square.

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

## Beta downloads

Tagged builds are produced by `.github/workflows/release.yml` and attached to a pre-release:

- **Linux x86_64** — `azahar-splitscreen-<tag>-linux-x86_64.tar.gz`
- **macOS Intel (x86_64)** — `azahar-splitscreen-<tag>-macos-x86_64.tar.gz` (the macOS job pins
  `CMAKE_OSX_ARCHITECTURES=x86_64` and the `x86_64-apple-darwin` Rust target, so the artifact is
  Intel regardless of the runner; Apple Silicon runs it under Rosetta 2)

Unpack, put dumps in `Test Roms/`, run `./run.sh`. Qt 6 and SDL2 must be installed, and
`BETA.txt` in the bundle lists the controls and every known limit. **There is no Windows
artifact**: the frame ring is POSIX shared memory and the orchestrator uses Unix process
control, so Windows needs a port first.

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
- **Platforms:** Linux x86_64 and macOS x86_64 only; Windows is not supported yet (POSIX
  shared-memory ring, `setsid`/`kill` process control and Unix sockets would all need ports).

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
