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
- [x] Right-stick virtual touch cursor (2026-11-14) — the focused seat's own pad
      moves a touch pointer on its bottom screen (0.2 deadzone, 320 px/s, clamped to
      the screen, stays where released) and **A** taps at it (`type 3`/`5`); only that
      seat's socket is written to. Proven at the socket level with virtual pads and
      `tools/qa/touch_probe.py`. History (35), captures in `docs/evidence/`.
- [ ] Pad buttons for the console — A taps the touch screen, but no pad button reaches
      a 3DS button, so a couch still cannot drive a game with a pad.
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
- [x] **Save sets** (2026-11-14): the picker's `Saves` row offers each player's own
      saves (default), one save set copied into every seat (grouped, for unlocks), or a save
      set per named seat (import one player's existing progress). `saves.rs` copies
      `sdmc/Nintendo 3DS` plus `nand/data/<title id>` and leaves console identity and the
      shared Mii database alone; `--saves <dir>` / `--import <seat>=<dir>` do the same from a
      terminal. Verified with sentinel save sets: grouped seeds all four, per-seat seeds only
      the named seat,       a bad path exits 1 with a notice, and a seat's Mii stays its own.
      History (30).
- [x] **Saves through the window** (2026-11-14): the picker's own controls were driven with
      xdotool, not just read - grouped seeds all four seats, per-player seeds only the named
      seat (sentinel files prove it), Stop leaves nothing running, and a bad path puts its
      reason in the one-window notice instead of booting. Two bugs fixed on the way:       the per-player row ignored `hidden`, and the notice was CLI-only. History (31).
- [x] **Save sets replace** (2026-11-14): seeding removes the seat's `sdmc/Nintendo 3DS`
      and the title's NAND save data before copying, so a set with less progress resets the
      seat (a set with neither tree clears both). A path without `nand/` or `sdmc/` is
      rejected instead of wiping a seat, and `(clean)` is a valid import. History (32).
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
- [x] **Seat watchdog** (2026-11-14): a seat whose process exits or whose ring publishes
      nothing for 15 s is restarted in place (ring unlinked first, room given one spare slot
      so the rejoin is not `RoomIsFull`), and the viewer re-attaches when the object is
      replaced. Verified with SIGKILL and SIGSTOP on individual seats: one restart each,
      four live tiles afterwards, 62.5 fps, clean `EXIT=0`. History (28).
- [x] **Restart notice** (2026-11-14): a restarting seat's tile shows `P2 restarting...` for
      as long as the orchestrator's marker file is there, i.e. until that seat publishes a
      frame again; the viewer also re-opens a ring whose header size changed, which a
      just-booted seat does when its bottom screen comes up. History (29).
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
- [x] **End-to-end from the app window** (2026-11-16): `azahar-splitscreen` launched with no
      arguments, MK7 and 2 players picked in the window, Start pressed. Room, two headless
      seats and the viewer came up from that one click; both seats held 60 fps into the one
      composite window; both seats' audio was on the default sink (P2 through
      `--effects-tilt`); closing the app window left zero orchestrator/room/seat/emulator/
      viewer processes, zero rings and zero audio streams. History (41),
      `docs/evidence/app-one-window.txt`.
- [x] **One window after Start** (2026-11-16): the picker hides as soon as a session starts,
      so the session window is the only one on screen, and `Session::viewer_gone()` plus a
      watcher thread makes closing that window end the app (session dropped, orchestrator
      exits). Verified in one app-driven run: one window, 60 fps both seats, both streams on
      the sink, and the window-close quit left nothing behind. History (42),
      `docs/evidence/app-one-window.txt`.
- [x] **Colored player outlines** (2026-11-16): the viewer copies mgba-splitscreen's rule - a
      3 px inset outline on each tile in the player's color plus a badge in the same color
      (P1 red, P2 blue, P3 green, P4 orange from its palette; purple and cyan for seats 5-6).
      History (43), `docs/evidence/six-top-only.png`.
- [x] **Six players** (2026-11-16): `layout::grid` is 2x1 / 2x2 / 3x2, the clamp is 2..6 and
      the picker offers 2-6. Six live MK7 seats in one window, 60 fps each, load 8.7 on 8
      threads and 8 of 15 GiB. History (43), `docs/evidence/six-seat-top-only.txt`.
- [x] **Top screen only for 3+ seats** (2026-11-16): three or more seats open in top-screen
      mode, with each seat's peek on F5-F10 to reveal its touch screen; two seats still show
      both screens. Verified in a six-seat run plus a two-seat check. History (43).
- [x] **Audio: player 1 only, restored** (2026-11-16): seats 2+ use the null sink
      (`[Audio] output_type=1`), so a six-seat session opens exactly one stream, and the
      `--effects-tilt` flag and its DSP filter are deleted from the fork. History (43).
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
- [x] **In-process quad-system spike — go (third pass, 2026-11-14).** Four seats in
      one process, one compositor window: every ring carries its own seat's picture, a
      key and a touch each reach only the addressed seat, SIGTERM leaves 0
      processes/rings/sockets. Keys were process-global (`InputCommon::GetKeyboard`) and
      rings went blank from two seats up because `OpenGLState::cur_state` was one
      process-wide cache; both are per-seat now (`keyboard_<ptr>` factory per window plus
      `thread_local` GL state). Evidence in `plans/in-process-seats.md` ("Third pass")
      and `history.md` (20). Second pass's `status=16` guest exit and the presentation
      leak were fixed earlier (history (17)); the guest exit has not recurred. The
      earlier "MK7 ignores touch" result stays retracted: the game's touch evidence is
      the kart-parts tap, not the four-seat run, where MK7's screen was not
      touch-operable.
- [x] Soak four in-process seats: 626 s on MK7 (2026-11-14), 4/4 live throughout,
      50-59 fps per seat, RSS flat at 2 325 MB, 51 threads, no guest exit, no crash, no
      stalled ring, SIGTERM left 0 processes/rings/sockets. Evidence in `history.md`
      (21). The second pass's `status=16` exit is now ruled out for ten minutes.
- [x] **One-window proof from the X side** (2026-11-14): four MK7 seats in one
      1280x720 window - exactly one mapped toplevel on the azahar pid, geometry unchanged
      through all four boots, each tile matched its own ring, pressing A on one seat
      changed only that quadrant, and three captures 10 s apart show all four regions
      non-blank and changing (256 distinct values each, hashes all different, 0.0 side
      bars = 400:480 kept). No torn frame in 5207 synthetic reads. Evidence in
      `history.md` (22).
- [x] ROM matrix + second-ROM soak (2026-11-14): MK7 and Monster Hunter Stories boot
      4/4 seats in one window; NSMB2, Mario Party Top 100, Zelda TFH and Kirby fail with
      `ErrorEncrypted` despite "Decrypted" names (no keys installed). MHS soaked 300 s,
      4/4 live, RSS flat at 2 088 MB, clean SIGTERM. Evidence in `history.md` (23).
- [x] **Production path soak** (2026-11-14): `run.sh --start` with four headless seats, one
      viewer window, MK7 290 s then MHS 345 s on the same slots. Both fixes it needed are in:
      the viewer paints with QPainter because this compositor throttles GL swaps to ~1 fps,
      and `slots::prepare` uses the default two-screen layout because LargeScreen makes MHS
      abort with a system error dialog. 60-63 fps presented, distinct per-seat rings, SIGTERM
      left 0 processes/rings/sockets. Evidence in `history.md` (24). A fresh slot's "No extra
      data found" prompt is now cleared without a player: `clear_boot_prompt` taps each seat
      three times after the viewer starts (history (25)).
- [x] **Pre-flight ROM check** (2026-11-14): a dump the loader rejects (encrypted, no keys)
      now stops the session before any window shows tiles - seats and room are torn down, the
      viewer opens in a notice mode naming the ROM and saying nothing was started, and
      `--start` exits 1 (`history.md` (26)). Before this the only trace was a terminal line,
      and the stock frontend's alternative is four black tiles. Four of the six test ROMs
      hit it (`history.md` (23)).
- [ ] Resolution policy: 1× for 3–4, 2× for 2, no per-game overrides unless a
      game is broken.

## 🟢 Milestone 4 — couch input UX

- [x] **View modes** (2026-11-14): the window shows both screens (default) or only the top
      screen, switched with Tab or `AZAHAR_VIEW=top` at launch, and each player holds F5-F8
      to peek at their own touch screen (releases clear in every layout, and a click in
      top-only mode still taps the touch screen). Verified on 4P MK7: tile picture widths
      904 px vs 470 px, and F5/F6 flip only P1/P2. History (33).
- [x] **Pad peek and a remembered layout** (2026-11-14): each pad owns a seat and holds
      that seat's peek with either shoulder button, F5-F8 still work for a seat without a
      pad, an unplug or a focus loss releases the peek, held peeks survive layout toggles
      without desync, and `slots/view.txt` brings back the last used layout with no flags
      (`--view` still overrides for one run without persisting). Verified in a live 4P
      session with two seats peeking at once; the run also caught a pad being registered to
      two seats, now fixed. History (34), captures in `docs/evidence/`.
- [x] **Pad buttons other than peek** (2026-11-15): every pad button, both sticks, both
      triggers and the d-pad now drive the console that owns the pad, forwarded as the keys
      the seat profile already binds (A/B/X/Y, R on the right shoulder, L on left stick
      click, ZL/ZR on the triggers, peek on the left shoulder, tap on R3). This pass also
      found and fixed why forwarded keys never reached a game: a seat process binds
      `engine:keyboard`, and the window's key state was registered under a name the profile
      never used. Verified in the guest (`tools/touchprobe`: P1 `keys=5`, P2 `keys=0`) and at
      the socket level. History (36), evidence in `docs/evidence/`.
- [x] **Audio: player 1 only** (2026-11-15): seat 1 keeps the default output, seats 2-4 are
      muted onto the null sink (`@Variant` escaped ulong, because `output_type` is read as
      `u32`), so a 4-player session opens exactly one audio stream. History (36).
      (Replaced by the effects tilt in (40), restored in (43) with plain `output_type=1`.)
- [x] **In-game pad verification** (2026-11-15): the seat's own pad moved Mario Kart 7's
      main-menu selection (89,284 px against 5,172 idle) while the other pad's identical
      press moved it 6,513 px, and the same held for a second title (Monster Hunter Stories:
      51,913 / 848 idle / 1,902 other). `tools/qa/pad_game_test.py` reproduces it. History
      (37), captures in `docs/evidence/`.
- [ ] **MK7's long boot**: the title spends ~90-120 s on "Connecting to Nintendo Network" and
      ignores input there, which reads as a hang. A line in the picker (or a "still loading"
      hint on the tile) would stop the next session chasing a pad bug that is not there.
- [ ] **Only two of the six test ROMs boot**: Kirby, Zelda Tri Force Heroes, Mario Party Top
      100 and New Super Mario Bros. 2 all report "an encrypted or unsupported application",
      so a bootable dump would widen QA past MK7 and MHS.
- [x] **Effects-tilted couch mix** (2026-11-16, owner's decision): every seat feeds the one
      output; seat 1 keeps its music at full and seats 2-4 are high-passed (300 Hz, 2nd order)
      and trimmed -6 dB in `DspInterface::OutputCallback`, behind the fork flag
      `--effects-tilt` that the orchestrator adds for seats 2+. A true split is not available
      (the HLE DSP mixes a console's music and effects before any sink), so this is an EQ
      guess. Measured on the device: the tilted seat is 16 dB down in 60-250 Hz and 4.5 dB down
      in 800-2500 Hz, and unmuting it lifts the mix's midband by +1.3 dB but its bass by only
      +0.2 dB. History (40), `docs/evidence/effects-tilt.txt`.
- [x] **Analog sticks** (2026-11-15): the left stick now sends its real position (a
      type-6 datagram, int16 x/y) instead of arrow keys, and HID hands it to the console as
      the circle pad; a seat with no controller still steers with the profile's arrow keys.
      Verified in the guest (`x=77` at half, `154` at full, `y=77` at half up), on the wire,
      and through a pad, with a second pad's deflection leaving the first seat centred.
      History (38), `docs/evidence/pad-analog.*`.
- [ ] **Save-set integrity**: after importing a set, MK7 says "The save data is corrupted
      and will now be reset." even though the copied files are identical across slots.
      Chase the NAND save metadata that pairs with the SD save before trusting imports.

- [ ] Screen-swap hint on first launch per slot (one overlay, dismissible).
- [ ] Pause/exit: one hotkey that stops the whole session, not four windows.
- [ ] Controller disconnect/reconnect handling per seat.
- [x] **Player badges** (2026-11-14): the viewer draws a `P1`–`P4` chip beside each
      tile's picture, so a couch can tell the seats apart without an overlay. Verified on a
      4-player MK7 window capture (four badges, correct quadrants, 62.5 fps) and the
      2-player default (`history.md` (27)).

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
- Mobile/Android front end.

## Backlog (only if the owner asks)

- Per-seat audio mixing (all players audible, ducked by focus).
- Save-state set export/import across slots (the sisters' "state set" idea).
- HDR / multi-monitor aware layout (2 players on two displays).
- Auto-update of the bundled Azahar sidecar.
- Linux packaging: AppImage/Flatpak for the orchestrator, room binary inside.
