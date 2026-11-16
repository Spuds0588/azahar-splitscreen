# AGENTS.md — Orientation for agent sessions

> **Read this first, then `todo.md` and `history.md`.** The intent document is
> `chat-3ds-couch-multiplayer-architec-google-ai-studio-2026-11-13-1012.md`
> (the PRD + implementation-guide export). Read it once for the reasoning;
> the two working files carry the current state.

## Hard rules for this repo

1. **Follow YAGNI.** Build only what the current milestone needs. Before adding
   a flag, crate, setting, abstraction or file, ask whether v1 is blocked
   without it. If not, it goes in `todo.md` under "Not doing (YAGNI)" or
   "Backlog" — not in the tree. The sisters both grew large by accreting
   speculative paths; we are not repeating that.
2. **Prefer the one-line solution.** A CLI flag beats a subsystem, a config
   value beats a new command, deleting code beats adding it, and an existing
   Azahar behaviour beats a patch. If the change needs a paragraph to justify,
   it is probably the wrong shape. (One line, not one-liner golf: keep it
   readable.)
3. **Clean the dev environment after every task.** The machine is a 15 GiB box
   with ~3.4 GiB free while idle, and Azahar's build trees are GB-scale. At the
   end of every task: stop every process/session you started, delete the build
   output you created, and say in your wrap-up what you freed. See
   [Dev-env hygiene](#dev-env-hygiene).

## What this project is

**azahar-splitscreen** is a couch split-screen front end for Nintendo 3DS
multiplayer: 2–6 **pre-linked Azahar instances**, one per player, each rendering
headless into its own shared-memory frame ring, composited by a single viewer
window and joined to a loopback `azahar-room` session, with a Tauri v2
orchestrator doing ROM selection, controller assignment, slot isolation,
spawning and teardown. Upstream Azahar's value is one console in one window;
this fork's is N consoles in **one** window, one audio stream, one quit path and
per-player identity colors.

- Repo: `Spuds0588/azahar-splitscreen`; `upstream` is
  `azahar-emu/azahar` (track it, rebase often, keep the fork diff surgical).
- Everything outside `src-tauri/` is upstream Azahar source. The split-screen
  work is a **downstream fork with a minimal patch surface**, plus a Tauri
  wrapper that owns orchestration.
- Not affiliated with or endorsed by Nintendo. No ROMs, no keys, no NAND in
  the tree. Azahar's `AI-POLICY.md` still applies to anything intended for
  upstream: AI-written patches are not submittable there, so keep fork-only
  changes small, human-reviewed and disclosed if they are ever proposed.

## North star for engineering decisions

**Playability and stability at the lowest hardware floor possible.** Accuracy
is not the goal (inherited from both sister projects). Apply it whenever a
trade-off appears:

- Never raise the floor: no new required files, no feature that only fits
  machines with extra RAM, threads or cores. "Stable with 4 instances on a
  mid-range desktop" is the working bar; add a weaker target only if the owner
  names one.
- 1× native resolution by default for 3–4 players is acceptable and expected;
  2× only for 2 players.
- Prefer not rendering what nobody can see (hidden bottom screen, audio for
  non-focused players) over optimising it.
- Accuracy work is welcome only where it fixes a game that is broken today.

## Architecture (decided — do not relitigate without the owner)

**Path 1: several full Azahar processes, one window.** Not an in-process
multi-core fork: Azahar's core is singleton-heavy (memory hooks, JIT code cache,
global state), so one process per player is the only sane shape — and the
in-process spike measured as a no-go (`plans/in-process-seats.md`). Each seat
renders into a POSIX shared-memory ring (`--shm-frame`) and takes input from its
socket (`--input-socket`); `tools/viewer` owns the only window.

```
azahar-splitscreen ── azahar-room (loopback UDP)
                   ├─ azahar --shm-frame azsh-<pid>-1 …   (headless seat)
                   └─ azahar-viewer                       (the one window)
```

- **Orchestrator:** Tauri v2 (Rust backend + web UI). Owns ROM picker,
  controller mapping, slot provisioning, process lifecycle, layout math.
- **Link layer:** Azahar's own network room (`azahar-room`) bound to
  `127.0.0.1` on a free port, with each instance auto-joined at launch. Do not
  invent a sync layer — the sisters' first lesson was to use the emulator's own
  link support.
- **Identity/saves:** one user directory per slot, so NAND, `SecureInfo_A`,
  save data, config and shader cache cannot collide. Cloning a save set into
  each slot is a supported mode; independent saves is the other.
- **Input:** keys, touch and pad buttons are forwarded by the viewer to the
  seat under the pointer or owning the pad; every seat has a hold-to-peek key
  (F5–F10) for its own touch screen, which is what keeps a top-screen-only 3+
  seat layout playable; the right stick is a virtual touch cursor.
- **Window:** exactly one, owned by `tools/viewer`. A seat must never map a
  window (`--shm-frame` implies `WA_DontShowOnScreen`); a per-seat window is a
  regression. The picker hides on Start, and closing the session window ends the
  session and the app.
- **Display:** 3+ seats show the top screen only (peek reveals the touch
  screen); 2 seats show both; tiles carry the player's color and `P<n>` badge
  (palette copied from mgba-splitscreen).
- **Audio:** seat 1 owns the output; seats 2+ run Azahar's null sink
  (`[Audio] output_type=1`), so a session opens exactly one audio stream.

Planned shape (create these only when the milestone needs them):

```
src-tauri/
  src/main.rs        # Tauri entry
  src/lib.rs         # commands + app state
  src/controllers.rs # gamepad enumeration/mapping (via the Azahar binary, not a new SDL stack)
  src/slots.rs       # per-slot user dir, unique identity, save cloning
  src/layout.rs      # monitor work area -> 2x1 / 2x2 / 3x2 rect math
  src/process.rs     # azahar-room + staggered Azahar spawn + cleanup
  binaries/          # sidecar azahar / azahar-room builds
patches/             # tracked git patches for the Azahar fork
```

## Current state (verified 2026-11-16)

The shipped shape is the one above: picker → room + 2–6 headless seats → one
viewer window → close it to quit. Six MK7 seats held 60 fps each on this box
(load 8.7 on 8 threads, 8 of 15 GiB); a 2-seat session is the everyday case. Only
two dumps boot here (Mario Kart 7, Monster Hunter Stories), and every seat in a
session plays the same ROM by design. `history.md` (43)–(45) and
`docs/one-window-flow.md` carry the numbers. Facts confirmed by running it:

- **Qt-only frontend.** `src/citra_qt/` is the only desktop frontend (no SDL
  frontend dir); SDL2 is used for gamepad input and audio
  (`src/input_common/sdl/`, `src/audio_core/sdl2_sink.cpp`). So a "lighter SDL
  frontend" is not an option — N Qt instances, kept headless, is what we have.
- **Binary:** target `citra_meta` with `OUTPUT_NAME azahar`
  (`src/citra_meta/CMakeLists.txt`), i.e. `build/bin/Release/azahar`.
- **CLI flags** (`src/citra_qt/citra_qt.cpp`): upstream's `--fullscreen/-f`,
  `--windowed/-w`, `--dump-video/-d`, `--gdbport/-g`, `--install/-i`,
  `--movie-play/-p`, `--movie-record/-r`, `--version/-v`, `--help/-h`, plus the
  fork's `--user <dir>`, `--position X,Y`, `--size WxH`, `--borderless`,
  `--join-room host:port`, `--player-name <name>`, and the headless seat pair
  `--shm-frame <name>` + `--input-socket <path>` (with `--stats <path>` for the
  per-second fps line). `--multiplayer/-m` is still a parsed-and-ignored stub, and
  there is no `--list-controllers`.
- **`--join-room`, not `--room`**: `citra_meta` already uses `--room` to launch
  the room-server binary, so `--room` never reaches the emulator.
- **User dir** (`src/common/file_util.cpp` `SetUserPath`): `--user <dir>` calls
  it before `QApplication`; the derived dirs (`nand/`, `sdmc/`, `log/`, …) are
  built by concatenation, so the flag appends the trailing separator. Its own
  paths are assigned, not emplaced, so the call is re-settable.
- **Room server:** builds from `src/citra_room/` + `src/citra_room_standalone/`;
  `--room-name`, `--room-description`, `--port`, `--max_members` (2–16),
  `--password`, `--preferred-app`, `--log-file`; default port
  `Network::DefaultRoomPort` = **24872** (`src/network/room.h:18`).
- **No single-instance guard** (no `QSharedMemory`/`QLockFile` in `src/`), so
  multiple copies run side by side today.
- Console identity is NAND-derived (`src/core/hw/unique_data.cpp` reads
  `SecureInfo_A` / `LocalFriendCodeSeed_B`), so separate user dirs are expected
  to give separate identities — confirmed: each seat joins under its own name and
  keeps its own Mii, and its own save data lives in its slot.

## Build & run

```bash
# from the repo root; Qt6 + Ninja + CMake >= 3.25 + SDL2 required
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_LTO=OFF -DENABLE_QT_UPDATE_CHECKER=OFF
ninja -C build -j 4          # -j4 keeps the link step inside this box's RAM
./build/bin/Release/azahar --windowed /abs/path/game.3ds
./build/bin/Release/azahar --fullscreen /abs/path/game.3ds

# the app: emulator + room (above), the viewer, then the orchestrator
ninja -C tools/viewer/build
(cd src-tauri && cargo build --release) && ./src-tauri/target/release/azahar-splitscreen
# pick a ROM, players 2-6, Start. Slots/logs land in slots/; close the window to quit.
```

One headless seat by hand, joined to a room and shown in the viewer:

```bash
./build/bin/Release/azahar-room --room-name splitscreen --preferred-app "Azahar Splitscreen" \
    --port 24872 --max_members 3 &
QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR=1 QT_ENABLE_HIGHDPI_SCALING=0 \
  ./build/bin/Release/azahar --user "$PWD/slots/p1/user" --shm-frame azsh-manual-1 \
    --input-socket "$PWD/slots/Player 1.sock" --join-room 127.0.0.1:24872 \
    --player-name "Player 1" --stats /tmp/manual.fps "/abs/path/game.3ds" &
./tools/viewer/build/azahar-viewer --seat "azsh-manual-1:$PWD/slots/Player 1.sock"
```

`--preferred-app` is mandatory for `azahar-room`; without it the room exits with
"preferred application is empty!".

- `ENABLE_LTO=OFF` is for dev-loop builds; release/packaging can turn it back
  on. Prefer `ccache` (`-DENABLE_CCACHE=ON`) over repeated cold builds.
- Options live in `CMakeLists.txt` (`ENABLE_QT`, `ENABLE_ROOM`,
  `ENABLE_ROOM_STANDALONE`, `ENABLE_SDL2`, `ENABLE_OPENAL`, `ENABLE_LTO`, …).
- A first cold configure downloads submodules; if `externals/` is incomplete,
  `git submodule update --init --recursive` is the fix, not `rm -rf externals`.
- Booting seats is fine on this box (display `:1`): six seats ran at 60 fps
  each. When a seat hangs, SIGKILL it - a standalone instance ignores SIGTERM.
- Link testing needs no game ROM: `tools/diagrom/build.sh` builds
  `azahar-linktest.3dsx` (libctru, docker devkitARM), which Azahar loads
  directly. See `tools/diagrom/README.md` for the host/join procedure and what
  a healthy screen looks like. **Verified 2026-11-13:** two instances in one
  loopback room reach `LINK ACTIVE - 2 PS` with real counters (see
  `history.md`), so the room relay and UDS path are known-good.

## Dev-env hygiene

Cleaning up is part of finishing a task, not a separate chore: this repo's
build output is bigger than the source tree, and the box is a 15 GiB machine
with 7-12 GiB free while idle.

```bash
# at the end of every session that built or ran something
pgrep -af "azahar|citra-room"                 # anything you spawned
du -sh build src-tauri/target 2>/dev/null
rm -rf build src-tauri/target .freebuff/preview-*.log
```

- Disposable: `build/`, `build-*/`, `src-tauri/target/`, `src-tauri/gen/*/app/build`,
  `node_modules/`, `__pycache__/`, stray `*.log` and runtime save dirs you
  created. All regenerable by the commands above.
- **Never delete:** `externals/` submodules, `dist/`, `src/`, `.git/`,
  `Test Roms/` (if the owner adds one), or anything with uncommitted work
  (`git status` first).
- Out-of-repo caches (`~/.cargo`, `~/.cache/ccache`) only matter for disk
  pressure — trim only if the owner asks.
- Report what you freed (or that there was nothing to free) in the wrap-up.

## Git & commits

- Branch from `master` for features (`feat/…`); never rebase or push over the
  owner's uncommitted work.
- Keep commits focused and descriptive; update `todo.md` and append to
  `history.md` in the same change as the work they describe.
- No vendored ROMs, keys, NAND dumps or screenshots containing save data.

## Key gotchas

- **Global/singleton core state:** assume nothing in `src/core` is re-entrant
  across instances. One process per player is the rule that keeps this true.
- **Config stomping:** two instances sharing a user dir overwrite each other's
  `config/` (resolution, audio device, controller mapping) on exit. Separate
  user dirs per slot, always.
- **Seats must stay headless.** `--shm-frame` sets `WA_DontShowOnScreen`; a seat
  that maps its own window is a regression, because one window is the point. The
  viewer is the only mapped window and needs X11/XWayland
  (`QT_QPA_PLATFORM=xcb`) for keyboard focus and for evidence capture.
- **Wayland cannot be driven from outside:** no external window moves, and
  per-window capture fails (`xwd` -> `BadColor` on this compositor). Use
  `spectacle` for full-screen evidence, or the viewer's own dump path.
- **Audio has one owner.** Seat 1 keeps the default output; seats 2+ are
  configured with `[Audio] output_type=1` (Azahar's null sink), so a session has
  exactly one stream. The old `--effects-tilt` experiment was deleted; do not
  reintroduce it.
- **ROM path parsing:** the Qt arg parser only accepts the game path as the
  last argument, or as the *only* argument. Keep new flags before it.
- **`--multiplayer` is a stub.** Do not read it as proof that room auto-join
  exists.
- **Saves are per user dir.** Copying a save set is a filesystem operation
  inside each slot's `user/`, not an Azahar feature.
- **Shader compilation cost multiplies by instance count.** Warm the shader
  cache once, then copy it into each slot's cache dir.

## Task wrap-up format (end of every task)

Use exactly these three sections, each with bullets:

```
## What I did
- <files changed, commands run, measurements, what was verified and how>

## What you need to do
- <manual steps, ROMs/displays needed, decisions only the human can make; "Nothing" is valid>

## What I recommend we work on next
- <ranked next steps, one bullet each, a sentence long>
```

Also append a dated entry to `history.md` (verdict + evidence) and tick or add
the matching boxes in `todo.md`.
