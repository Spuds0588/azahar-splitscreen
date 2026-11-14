# todo.md — What remains

> Ordered by priority within each section. **Append results to `history.md` as
> you work** so the next session can pick up mid-thought. Tick boxes in the
> same change as the work. Rules live in `AGENTS.md` (YAGNI, one-line
> solutions, clean the dev env).

## ✅ 2026-11-13 — Diagnostic link-test ROM (built and run; link verified)

No game ROM is needed to test the link: `tools/diagrom` builds
`azahar-linktest.3dsx`, a libctru homebrew that hosts/joins an emulated UDS
network and reports what actually arrives.

- [x] **`azahar-linktest.3dsx` built** (134 KB, valid 3DSX header: magic, 44-byte
      header, code/rodata/data segments). Host (A) creates a UDS network and
      broadcasts a heartbeat every 100 ms; joiners (B) beacon-scan, connect,
      echo heartbeats back; the screen shows node id, node mask, per-peer
      counts, RTT, `LINK ACTIVE - N PS`, `STALL`, and the last result code.
- [x] **Emulator-side UDS coverage checked in source**: every command libctru
      uses is implemented in `nwm_uds.cpp`; `SetProbeResponseParam` (0x21) is
      stubbed but the HLE framework returns success for unimplemented
      functions, so hosting works and logs one harmless error line.
- [x] **Run two instances against the ROM** — done 2026-11-13: host
      `state=6 node=1`, joiner `state=9 node=2`, mask `0x0003`, tx/rx climbing
      at ≈18/s each way, RTT 32 ms typical (16–69 ms min/max), `LINK ACTIVE`
      on both. Counters and the automation issues are in `history.md`
      (2026-11-13 (3)).

## 🟢 Phase 0 — verify the ground before writing code (do this first)

Nothing below is worth patching until these are true/false, written down in
`history.md`. Each item is a command or a 5-minute observation, not a study.

- [x] **Build upstream Azahar as-is** (`cmake … -DENABLE_LTO=OFF`,
      `ninja -C build -j 4`). 2420 targets, ~30 min cold, `build/` 1.5 GB, no
      OOM at `-j4`. Booted a homebrew, not a game to a title screen (no ROM to
      test with).
- [ ] **Two instances, one ROM, one machine** — launch twice with the same
      default user dir: do both windows render, does either write the other's
      `config/`? (Expected: yes, they stomp. This is the isolation argument.)
- [x] **Per-slot user dir, no patch** — two instances with `cwd=slotN` and a
      pre-created `user/` produced separate `nand/`, `sysdata/mac.txt`, config
      and `sdmc/`, and both joined the same room at once. No patch needed.
- [x] **Room baseline** — `azahar-room` on `127.0.0.1:24872`, both instances
      joined by hand (Multiplayer → Direct Connect), `LINK ACTIVE - 2 PS` with
      real counters. Menus used and every automation trap are in `history.md`;
      the flow is what the orchestrator must replace with auto-join.
- [ ] **Controller visibility** — enumerate SDL gamepads from the OS while two
      Azahar instances run (`SDL_VIDEODRIVER`-free path, `evdev`/`hidraw` in
      mind). Confirms whether `--list-controllers` needs to be a patch or can
      be a small probe binary.
- [x] **Window tiling reality on this machine** — X11 (`DISPLAY=:1`) +
      `QT_QPA_PLATFORM=xcb`; `--position/--size/--borderless` place windows
      exactly, `QT_SCALE_FACTOR=1` is required or Qt rounds 960x540 → 1000x563.
      Layout must use the work area (`_NET_WORKAREA` = 1920x1034).
- [x] **Cost of one instance** — MK7 title: 60 FPS, 1.43 ms frame, ~651 MB
      RSS, ~16% CPU at 1×. Four instances: 60 FPS each, 639–745 MB RSS each,
      17–22% CPU each (see `history.md` 2026-11-13 (4)).

## 🟢 Milestone 1 — thin fork patch surface (only what Phase 0 proves is missing)

Each item is a patch to the Azahar fork, kept isolatable so a rebase is cheap.
Nothing goes in `src/core` that can live in the frontend.

- [x] `--user <dir>` — calls `FileUtil::SetUserPath(dir/)` before anything
      touches config. `SetUserPath` also had to stop using `emplace` (only the
      first call took effect).
- [x] Window geometry: `--position x,y --size WxH` plus `--borderless`, applied
      after `show()` so saved geometry does not win.
- [x] Room auto-join: `--join-room <host:port>` (not `--room`: that is the
      room-server switch) with `--player-name`, joining ~4 s after boot and
      printing state/error on stdout. Nicknames must be 4–20 chars.
- [x] Joining no longer opens the room/chat dialog over the game: one line
      removed in `MultiplayerState::OnNetworkStateChanged`; the chat is still
      reachable from the Room action.
- [ ] `--list-controllers` — machine-readable gamepad list (GUID + name) so
      mapping does not depend on a second SDL stack in Rust.
- [ ] Screen-swap hotkey (top/bottom) — one input binding plus a renderer
      swap; disabled while the touch cursor is active.
- [ ] Right-stick virtual touch cursor — stick moves a pointer on the bottom
      screen, a button clicks. Only if Phase 0 shows the touch path is needed
      for MK7 menus.
- [ ] `--audio-focus <slot>` / null audio for non-focused instances, and
      `--vsync-master` so one instance drives presentation.

**Order matters:** `--user` + geometry unblock everything else; hotkeys and
touch are playability polish and may slip to Milestone 4.

## 🟢 Milestone 2 — Tauri orchestrator (2 players first)

- [x] `src-tauri/` scaffold: Tauri v2, one window, ROM picker, player count
      2–4, Start/Stop + log pane. No settings screen yet.
- [x] `slots.rs`: creates `slots/pN/user/`, writes the slot's `qt-config.ini`.
      No template/manifest yet — the emulator seeds a fresh dir on first boot.
- [x] `process.rs`: free loopback port, fresh `azahar-room` per session, spawns
      2 s apart with per-slot logs, tears down room + children on Stop **and**
      on window close (verified: `pgrep -af "Release/azahar"` empty).
- [x] `layout.rs`: monitor **work area** → 2×1 / 2×2 with a 1 px gutter, feeds
      the geometry flags (4P tiles are 959x516 on this box).
- [ ] `controllers.rs`: currently lists `/dev/input/js*` only (no pads here) and
      does not yet write pad profiles; needs `--list-controllers` or an SDL
      probe, and a seat→port mapping.
- [ ] Failure path: one instance dies → stop the session cleanly and say which
      slot failed (do not leave three orphans).

## 🟢 Milestone 3 — identity, saves and the 4-player wall

- [ ] Confirm per-slot identity (console ID / friend code seed) is distinct;
      if not, provision per slot before first boot.
- [ ] Save story: "clone a save set into every slot" (progress-unlocked games)
      vs "independent saves". Ship clone-once, no UI beyond a checkbox.
- [ ] Shader cache warm-up: boot once, copy the cache into each slot.
- [x] 4 players: measured at 1× — 60 FPS and 1.43 ms per seat, ~640–750 MB
      each, ~0.8 core total; 2×2 layout verified on 1080p (see `history.md`).
- [x] 4 players **in one MK7 race** — verified 2026-11-13 by the owner: four
      seats in one Local Play lobby, fully playable at near full speed. Host
      serves three DLP clients, then the race network; logs in `history.md` (9).
- [x] **Measured fps per seat** — `--stats <file>` writes `fps/speed/frame_ms`
      every second; the orchestrator passes it, so each seat owns
      `slots/Player N.fps`. Four seats at 60.0 fps (min 59) in the demo race.
- [x] **QA sampler** — `tools/qa/monitor.sh start|stop|report`: per-seat
      fps/CPU/RSS plus new Error/Critical lines, one sample a second, summarised
      with min/avg/max and an fps<55 list. Built for human-driven sessions.
- [ ] Soak a human-driven 4P session with the sampler running (race, several
      races, pause/exit) and triage whatever the report flags.
- [x] **One window** — `tools/quadview` adopts the four seat windows (XEmbed,
      X11/XWayland), spawned and torn down by the orchestrator. Verified: one
      window, four 959x516 tiles, 59-60 fps, keyboard/touch follow the pointer.
- [x] **P1 of the shared-memory mission** — `--shm-frame <name>` publishes BGRA
      frames into `/dev/shm/<name>` at 57 fps (400x480, 3 slots); a decoded PNG
      matches the game. +3 % CPU per instance. See `plans/single-window-shm.md`.
- [x] **P2 of the shared-memory mission** — `tools/viewer`: one window, one GL
      context, a tile per ring, letterboxed; 4 seats composited and verified from
      a framebuffer dump (four live MK7 races).
- [x] **P3 — input** — `--input-socket <path>` + viewer forwarding, key datagrams
      verified on the wire; HOME to seat 1 only took seat 1 off the race.
- [x] **P4 — orchestrator** — seats spawn headless with ring+socket, the viewer
      spawns once, teardown unlinks rings/sockets (SIGTERM left nothing running,
      no `/dev/shm` object, no socket, no window). `tools/quadview` deleted.
- [x] **P5 — measurement & docs** — `tools/qa/monitor.sh` samples the viewer
      (cpu/rss/presented fps) next to the seats. Numbers in `history.md` (12).
- [x] **P3 follow-up — touch proven**: a tap at frame (440,195) on seat A's
      bottom screen took seat A from MK7's title into a single-player race
      (change 166/255) while seat B stayed at the title (0.01–0.04). Two bugs
      had to be fixed for it: the seat's touch layout was never sized (headless
      window) and the viewer mapped clicks to 400x480 instead of its own frame
      geometry. See `history.md` (13).
- [x] **Native-Wayland viewer** — fixed 2026-11-14. Root cause was not the viewer:
      this session's compositor maps toplevels but never delivers frame callbacks
      (proven with a raw `wl_shm` client and two minimal Qt windows), and Qt
      inexposes a window after one missed callback. The viewer now raises
      `QT_WAYLAND_FRAME_CALLBACK_TIMEOUT` for itself, so it paces on its own timer:
      one window 1829x1029, `presented_fps=62.4`, seats 59–60 fps, rings 49–53 fps,
      targeted input, clean teardown. See `history.md` (15).
- [ ] **Screen-visible check (needs a human): what the panel shows.** presented
      fps is now measured (62.4 on native Wayland, `history.md` (15)), but this
      session's compositor never delivers frame callbacks (`glxgears` 1.0–1.8 FPS),
      so a human has to confirm the picture is on the screen: `./run.sh` → Start →
      `tools/qa/monitor.sh report`, or `./run.sh --start "<rom>" --players 4`.
- [ ] **In-process quad-system spike — no-go (second pass, 2026-11-14).** Presentation
      fixed (own loop per seat, compositor window) and teardown clean, but one seat's
      guest exits (`status=16`) while four separate processes with the same profile are
      fine — so the blocker is process-global emulator state, plus process-global keys.
      Evidence and the next bisect in `plans/in-process-seats.md`, `history.md` (17).
      First pass (history (16)) had not yet built the per-seat loop and leaked the
      emulation thread on stop; both are fixed now.
- [ ] Pre-flight ROM check: flag a seat that fails to boot (e.g. an encrypted
      dump with no keys) instead of leaving four error dialogs on screen.
- [ ] Resolution policy: 1× for 3–4, 2× for 2, no per-game overrides unless a
      game is broken.

## 🟢 Milestone 4 — couch input UX

- [ ] Screen-swap hint on first launch per slot (one overlay, dismissible).
- [ ] Pause/exit: one hotkey that stops the whole session, not four windows.
- [ ] Controller disconnect/reconnect handling per seat.
- [ ] Optional: player badges (P1–P4) drawn by the orchestrator overlay —
      only if a bare window proves ambiguous.

## 🔴 Milestone 5 — acceptance (Mario Kart 7 benchmark)

- [x] **Local Play group discovery works**: the joiner lists the host's group
      and both seats enter a local multiplayer game (owner-driven; root cause
      was DLP beacon `child_title_id`, fixed in `dlp_srvr.cpp`).
- [ ] 2P: both consoles join a local room from the orchestrator (done: both
      join, MK7 boots) — **reach a race** and hold it 5 minutes (not done).
- [x] 3P: host + 2 joiners in one MK7 local game, 959x516 tiles, running at
      speed (nodes 2 and 3 on an 8-node UDS network; 805–820 MB RSS each).
- [ ] 4P: same on one machine, 1× native, audio on P1 only (session + join
      verified; race not reached).
- [ ] Cold start → race in under 60 s (including room + spawn).
- [ ] Stop session → zero leftover processes, zero orphan ports, slots intact
      for the next run.
- [ ] Save a short capture + `history.md` entry naming the machine spec.

## ❓ Open questions for the owner

- [ ] ROM formats for v1: decrypted `.3ds`/`.cci` only, or also installed
      `.cia` titles (saves live in `sdmc/`, which changes the clone story)?
- [ ] Target hardware for "low-end": the dev box only, or a specific TV/mini-PC
      to benchmark against?
- [ ] Clone saves by default, or independent saves by default?
- [ ] Is a Steam Deck / Gamescope target in scope for v1 (it changes the
      Wayland answer)?

## ⛔ Not doing (YAGNI)

- Online/remote play, WebRTC, join codes.
- Download Play / single-card sessions.
- In-process multi-core emulation (singleton core state; rejected).
- A custom link/sync layer (Azahar's room is the link layer).
- An SDL/lightweight frontend (does not exist in this tree).
- Per-game fix registry, cheat/ mod support, video recording.
- More than 4 players.
- Mobile/Android front end.

## Backlog (only if the owner asks)

- Per-seat audio mixing (all players audible, ducked by focus).
- Save-state set export/import across slots (the sisters' "state set" idea).
- HDR / multi-monitor aware layout (2 players on two displays).
- Auto-update of the bundled Azahar sidecar.
- Linux packaging: AppImage/Flatpak for the orchestrator, room binary inside.
