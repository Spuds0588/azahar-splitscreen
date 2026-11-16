# history.md — What we tried vs what to try next

> **Append-only.** This file exists so a future session can pick up without
> re-deriving dead ends. Add a dated entry after every experiment — especially
> failures. Never delete old entries; supersede them. Read the bottom-most
> entries first.

---

## 2026-11-13 (3) — v0.0.2: Azahar built, two instances linked in a room

**Verdict: the link path works end to end on stock Azahar. Two instances, each
with its own user dir, one `azahar-room` on loopback, `azahar-linktest.3dsx` in
both: `LINK ACTIVE - 2 PS`, node mask `0x0003`, 100 ms heartbeats crossing both
ways, RTT 16–69 ms. No emulator patch was needed for any of it.**

Build (this box, 4 cores, Debian 13, 15 GiB):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=OFF \
  -DENABLE_QT_UPDATE_CHECKER=OFF -DENABLE_QT_TRANSLATION=OFF \
  -DENABLE_DISCORD_RPC=OFF -DENABLE_CCACHE=ON -DENABLE_FFMPEG=OFF
ninja -C build -j4 citra_meta citra_room_standalone
```

2420 targets, ~30 min cold with an empty ccache, no swap death at `-j4`.
`build/` = 1.5 GB; `build/bin/Release/azahar` 62 MB, `azahar-room` 6.9 MB.
Submodules had to be fetched shallow (`--depth 1 --jobs 4`) — full clones were
far too slow on this link.

Run: `azahar-room --room-name linktest --preferred-app "Azahar Linktest"
--port 24872 --max_members 4`, then two instances with **different cwds**
(`slot1`, `slot2`, each pre-seeded with `user/`), joined by hand through
Multiplayer → Direct Connect to `127.0.0.1:24872`. Nicknames `Player1` /
`Player2`. A on slot1 (host), B on slot2 (join).

Counters, from `sdmc:/linktest.log` in each slot (the ROM mirrors its output
there because `svcOutputDebugString` never appears in `azahar_log.txt` at any
log filter):

| | host (slot1, Player1) | join (slot2, Player2) |
|---|---|---|
| `udsGetConnectionStatus` | state 6, node 1 | state 9, node 2 |
| nodes / mask | 2/4, `0x0003` | 2/4, `0x0003` |
| samples logged | 50 (waiting for 12, then active) | 39 (active from the first sample) |
| tx / rx at end | 470 / 723 packets (≈18/s each way) | 363 / 725 packets (≈18/s each way) |
| RTT, last 20 samples | 32–49 ms, avg 36.5 | 32–33 ms, avg 32.5 |
| RTT min/max, whole session | 16 / 69 ms | 16 / 65 ms |
| peers | `0x04` | `0x02` |

The screen agreed with the log on both instances: `LINK ACTIVE — 2 PS`, tx/rx
climbing at 18–19/s, `rtt: 33 ms (min 16, max 69)`, `last error: 0x0`. The host
needs ~5 s of beaconing before a joiner sees it, so start the host first.

Latency note: 32 ms typical on loopback is not the network — the ROM only
samples on its 100 ms heartbeat grid and the echo rides the same relay tick, so
this is quantisation plus scheduling, not transport cost. A game's own timing
will be tighter than this number suggests; treat 16–69 ms here as the
measurement floor, not as room latency.

### Issues hit (all automation, none in the emulator)

- **Fresh slots have no console nickname.** Direct Connect's nickname is the
  emulated CFG username (block `0xA0000`), which is empty on a new user dir, so
  Connect fails validation with an error dialog. Fixed with
  `tools/diagrom/set-slot-name.py <slot> <name>`, which writes the block in
  `user/nand/data/*/sysdata/00010017/00000000/config` (16-byte header, 12-byte
  block entries, data at `0x455C`). Needs one boot first so the save exists.
- **`azahar-room` refuses to start without `--preferred-app`** and warns
  without `--preferred-app-id`; `--max_members` uses an underscore. A missing
  `--ban-list-file` only warns (“Could not open ban list”).
- **A room server that has seen clients die can stop accepting joins** — the
  client sits on “Connecting” and the server logs nothing. Restarting the room
  fixed it immediately. Start a fresh room per session in the orchestrator
  rather than reusing one.
- **The Direct Connect dialog does not close itself after a successful join.**
  `OnConnection` checks state right as the join future finishes, so the window
  can stay open in the Joined state. Closing it through the window manager
  leaves the app with dead keyboard input (hidden modal grab); leaving it open
  is harmless. The orchestrator should not need this dialog at all.
- **Two instances at the default position sit exactly on top of each other**,
  so `xdotool` clicks meant for one land on the other, and every key press went
  to the top window. Moving them apart (`wmctrl -i -r <id> -e 0,x,y,-1,-1`) made
  input reliable. More evidence for the window-geometry flag and for the
  orchestrator owning placement.
- `SetProbeResponseParam` (UDS 0x21) still logs one `unimplemented function`
  error per host start — expected, harmless (`service.cpp:155` returns success).

Not tested: same user dir for two instances (config stomping), four players,
and a real game. This run used the diagnostic ROM only.

## 2026-11-13 (2) — v0.0.1: diagnostic link-test ROM (`tools/diagrom`)

**Verdict: built a 134 KB `azahar-linktest.3dsx` that tests the UDS link with
no game ROM. It compiles and has a valid 3DSX header; it has NOT been run in
Azahar yet (no emulator binary on this box), so the link path is still
unverified end to end.**

Why a homebrew and not a found ROM: Azahar loads `.3dsx` directly
(`src/core/loader/3dsx.cpp`), so a libctru app is a self-contained test payload
— no cartridge image, keys or NAND, and it can be rebuilt with one command.
The 3DS analogue of mgba-splitscreen's `linktest` ROM.

What it does (one `source/main.c`, ~330 lines): host (A) calls
`udsCreateNetwork` on a fixed `wlancommID` (0x5A5A0001), broadcast-heartbeats
every 100 ms and echoes peers' heartbeats; joiners (B) `udsScanBeacons` for up
to 5 s, `udsConnectNetwork`, and do the same. The top screen prints the role,
`udsGetConnectionStatus` (status, node id, nodes/max, node bitmask), tx/rx with
a 1 s rate, per-source-node counters, RTT (heartbeat→echo), the last parsed
payload, `LINK ACTIVE - N PS` / `STALL`, and the last failing result code.

Build: `tools/diagrom/build.sh` uses `devkitpro/devkitarm:latest` in docker
(image 2.09 GB; no devkitPro on this box, and no root needed). Makefile is the
devkitPro 3DSX application template with only the metadata changed. Output is
gitignored; `build.sh clean` removes it.

Emulator-side facts checked before writing it (all from this tree):

- `nwm::UDS` implements `InitializeWithVersion`, `BeginHostingNetwork` (0x1D),
  `ConnectToNetwork` (0x1E), `Bind`/`Unbind`, `SendTo`, `PullPacket`,
  `GetConnectionStatus`, `RecvBeaconBroadcastData`, `DecryptBeaconData`,
  `Set/GetApplicationData`, `DestroyNetwork`, `DisconnectNetwork` — i.e. the
  whole libctru path (`src/core/hle/service/nwm/nwm_uds.cpp:1684`).
- `SetProbeResponseParam` (0x21) is **null** in that table, but
  `ServiceFrameworkBase::ReportUnimplementedFunction` answers with success
  (`src/core/hle/service/service.cpp:155`, "Hack - ignore error"), so
  `udsCreateNetwork` (which calls 0x21 and bails on failure) still works. The
  log prints `unknown / unimplemented function ...0x210080` once per host
  start — expected, not the failure.
- UDS packets travel as room `WifiPacket`s (`SendWifiPacket`), and the console
  MAC comes from the room member when connected. So: **no joined room, no
  link**, and two instances sharing one user dir would share identity/MAC.
  Both are called out in `tools/diagrom/README.md`.
- `ndm::u` implements `EnterExclusiveState`/`LeaveExclusiveState`, which
  `udsInit` needs; no single-instance lock in the emulator.

Not done: running it. That needs an Azahar build and two instances in a room
(Phase 0). No source change was made to the emulator, and nothing in `src/` was
touched.

## 2026-11-13 — v0.0.0: project documents + verified starting facts

**Verdict: scaffolding only. The tree is still stock Azahar; no split-screen
code has been written or built. The value of this pass is the fact-check:
several assumptions in the PRD export are now confirmed or corrected against
the source, and the first real work is Phase 0 of `todo.md`.**

What was verified by reading the tree (master `11eceda5e`, clean apart from the
untracked PRD export):

- **Qt is the only desktop frontend.** `src/` has `citra_qt`, `citra_cli`,
  `citra_libretro`, `android` — no SDL frontend. SDL2 is linked for gamepad
  input (`src/input_common/sdl/`) and audio (`src/audio_core/sdl2_sink.cpp`).
  The PRD's "patch the lighter SDL frontend if it still exists" branch is
  therefore dead; four Qt instances is the target to make fit.
- **The executable is `azahar`**, built from `src/citra_meta` with
  `OUTPUT_NAME azahar` → `build/bin/Release/azahar`.
- **Existing CLI is thin**, `src/citra_qt/citra_qt.cpp:281-400`:
  `--fullscreen/-f`, `--windowed/-w`, `--dump-video/-d`, `--gdbport/-g`,
  `--install/-i`, `--movie-play/-p`, `--movie-record/-r`, `--version/-v`,
  `--help/-h`, bare game path last. `--multiplayer/-m` is parsed then ignored
  with a warning. No `--user`, no geometry, no room join, no controller list.
- **User-dir resolution is the useful find** (`src/common/file_util.cpp:935`
  and the POSIX branch after it): a `user/` directory **in the process's cwd**
  takes precedence over XDG. So per-slot isolation can be a spawn-time
  convention (`cwd = slot_dir`, `slot_dir/user/` present) with **no Azahar
  patch at all** — the `--user` flag is a robustness improvement, not a
  prerequisite. Caveat: ROM paths must then be absolute.
- **Room server defaults**: `src/network/room.h:18` `DefaultRoomPort = 24872`;
  `src/citra_room/citra_room.cpp` exposes `--room-name`, `--port`,
  `--max_members` (2–16), `--password`, `--log-file`.
- **No single-instance lock** anywhere in `src/` (no `QSharedMemory`,
  `QLockFile`), so multiple copies already run concurrently.
- **Console identity is NAND-derived** (`src/core/hw/unique_data.cpp`:
  `SecureInfo_A`, `LocalFriendCodeSeed_B`), so separate user dirs should yield
  separate identities — *expected*, not yet observed; Phase 0 confirms it.

Decisions recorded (from the PM/architecture export, not re-derived):

- Path 1: multiple full Azahar processes, tiled borderless windows, plus a
  Tauri v2 orchestrator. In-process multi-core is rejected on singleton-state
  grounds.
- Azahar's own room server on loopback is the link layer; no custom sync.
- v1 ROM scope: decrypted `.3ds`/`.cci`; `.cia` saves live in `sdmc/` and are
  an open question.
- Low-end posture: 1× native for 3–4 players, audio only for the focused
  player, one instance drives VSync.

Not done, on purpose:

- No build was run (cold Azahar build is ~GB-scale output on a 15 GiB box);
  Phase 0 owns that, and the dev env was left clean: no `build/`, repo at
  134 MB, no processes started.
- No Tauri scaffold, no patch, no ROM/window/room experiment.

## Lessons inherited from the sister projects (not yet re-tested on 3DS)

Kept because they cost those projects weeks each. Re-verify only where 3DS
differs.

- **Use the emulator's own link support.** mgba-splitscreen's wins came from
  mGBA's lockstep SIO driver; hand-rolled sync layers (`rendezvous.c`) only
  served as instrumentation. melonDS-Multiplayer spent dozens of passes on its
  bus because the games' own handshakes were the real wall. Apply: make the
  Azahar room work before improving it.
- **Per-instance state is the whole game.** melonDS's bus bug class was shared
  state between consoles; mgba's save-state work existed to keep N pads in
  lockstep. Apply: separate user dirs per slot, absolute paths, no shared
  config writes.
- **Staggered spawn, not simultaneous.** melonDS needed a join stagger to stop
  clients authenticating in the same millisecond. Apply to four Azahar boots
  and room joins.
- **Hidden work is not free.** Both projects eventually routed audio/video per
  player instead of "just render it". Apply: do not render the hidden 3DS
  screen, do not open audio devices for background seats.
- **Window tiling is an OS problem, not a code problem.** Wayland forbids
  external window moves; mgba/melonDS both ended up documenting platform
  caveats rather than solving them. Apply: force XWayland or state the rule.
- **Instrument before you theorise, and keep a log of verdicts.** The sisters'
  `history.md` files are why their dead ends stay dead. This file is that
  contract.
- **Clean the environment every session.** mgba-splitscreen once hit 16 GB of
  regenerable output; melonDS added a dry-run `clean_repo.sh`. We start with
  the manual sweep in `AGENTS.md` and add a script only if it earns its keep.

## What to try next (in order)

1. Phase 0: build, boot, two instances, per-slot user dirs, manual room join.
   Write the results here before touching C++.
2. Only the patches Phase 0 proves necessary — expected first: `--user`,
   window geometry, room auto-join.
3. Orchestrator with 2 players; 4 players after the layout math is proven.

---

## 2026-11-13 (4) — v0.1.0 MVP: fork flags + Tauri orchestrator, MK7 on four tiles

**Verdict: the orchestration MVP works — pick ROM, pick 2–4 players, press Start,
and N borderless Azahar windows come up tiled, each in its own user dir (own
NAND/MAC/config), all joined to a fresh loopback `azahar-room`. MK7 boots in
every tile, and one instance reaches MK7's Local Play lobby. What is *not*
verified: two MK7 instances seeing each other's Local Play group, and a race
(see "Not verified").**

### Flags added to the fork (all uncommitted, all in `src/citra_qt/` + two small
`src/common/file_util.cpp` fixes)

| Flag | Behaviour |
| --- | --- |
| `--user <dir>` | `FileUtil::SetUserPath(dir/)` before `QApplication`; prints `user: <dir>` |
| `--position X,Y` | applied after `show()` (saved geometry would otherwise win) |
| `--size WxH` | same; needs `QT_SCALE_FACTOR=1` or Qt rounds 960x540 → 1000x563 |
| `--borderless` | `FramelessWindowHint` + hides menu/status bar |
| `--join-room host:port` | `RoomMember::Join(...)` 4 s after boot, prints state/error |
| `--player-name <name>` | nickname for the room; **4–20 chars** or the room rejects it |

Bugs this flushed out, in the order they bit:

1. `citra_cli`'s `CheckForOptions` used GNU getopt, which split `--size` into
   `-s -i -z -e` and hit the compression CLI ("Invalid option combination").
   Fixed by returning early when any argv starts with `--`.
2. `--room` collided with `citra_meta`'s *room-server* switch: every "instance"
   launched `azahar-room` instead of the emulator. Renamed to `--join-room`.
3. `SetUserPath` used `emplace`, so a second call only fixed `UserDir`/`Config`/
   `Cache` and left `nand/`, `sdmc/`, `log/` on XDG. Assignment instead; the
   function is now re-callable.
4. Paths are built by concatenation: `--user /tmp/ub` produced `/tmp/ublog/`.
   The flag now appends the trailing separator.
5. Room nicknames shorter than 4 chars are rejected as a name collision —
   "P1" failed, `Player 1` works. `slots.rs` uses `Player N`.

### Orchestrator (`src-tauri/`)

Tauri v2 app (ROM picker from `Test Roms/` + repo root, players 2–4, Start/Stop,
log pane). `process.rs` picks a free UDP port, starts a fresh `azahar-room`
(needs `--preferred-app`), waits for "Room is open", then spawns one instance per
seat 2 s apart with `--user/--position/--size/--borderless/--join-room/
--player-name`, `QT_QPA_PLATFORM=xcb`, `QT_SCALE_FACTOR=1`,
`QT_ENABLE_HIGHDPI_SCALING=0`, each in its own `setsid` group with a per-slot
log. `layout.rs` uses the monitor **work area** (`xprop -root _NET_WORKAREA`,
1920x1034 here) so a panel does not push the bottom row up; 2P = 2x1, 4P = 2x2,
1 px gutter. `slots.rs` seeds `slots/pN/user/` and writes
`Layou_large`-style config (`layout_option=2`, `pauseWhenInBackground=false`).
`controllers.rs` reports `/dev/input/js*` pads; none attached here → keyboard.

Teardown verified both ways: **Stop** and **closing the window** both leave
`pgrep -af "Release/azahar"` empty and exit the orchestrator. (Killing the
orchestrator with `SIGKILL` still orphans instances — that is the one path that
cannot clean up, and it is why early runs left stale rooms around.)

### Measurements (this box: i5-8350U, UHD 620, 1920x1080 X11, 1x native)

- One instance, MK7 title: **60 FPS, frame 1.43 ms, RSS ~651 MB**, ~16% CPU.
- Four instances (windowed measurement run so the status bar is readable,
  900x500 each, MK7 title/menu): **App 60 FPS, frame 1.43 ms** on every seat,
  **RSS 639–745 MB each (~2.7 GB total)**, **CPU 17–22% each (~0.8 core)**.
- Product 4P run through the orchestrator: tiles **959x516** at (0,0) (960,0)
  (0,517) (960,517), 4/4 joined the room, 4 MK7 title screens, RSS 648–749 MB,
  CPU 15–20% each. Boot to title ≈ 15–20 s per seat, all four up in ~25 s.

### Not verified (do not read as done)

- **MK7 ↔ MK7 Local Play discovery.** P1 created a group and sat in
  "Finding players…" (bottom screen shows its room nickname). P2's "Choose a
  group" list stayed at four empty `0/8` slots for 15 s+, so the two consoles
  did not see each other's group. The room itself is fine (server logs
  `Player 1..4 has joined`), so this is game-level, not link-level; the
  link-test ROM remains the transport proof. Needs another session with time
  to check whether MK7 needs a Mii per console or a specific group flow.
- **A race.** No Mii exists in any slot, so MK7 shows "There are currently no
  Mii characters in Mii Maker" on every boot (dismiss with A). Single Player
  does reach a race on one instance (observed 60 FPS, 1.4 ms), but no lobby
  race was run.
- **Gamepads** — none attached, so per-seat pad profiles are written but unused.

### Driving the windows from a script (traps)

- `xdotool key --window` is ignored by Qt; the window must be **activated**
  (`xdotool windowactivate --sync`) and keys sent without `--window`.
- MK7's main menu is **touch-only**: the d-pad does not move the highlight.
  Mouse clicks map to the touchscreen only over the small bottom-screen panel
  (bottom-right of a LargeScreen tile); in a 959x1034 tile the rows sit at
  y≈653/685/715/743, x≈805–950.
- `--borderless` hides the status bar, so the FPS/speed readout only exists in
  a windowed run — measure with `--windowed`, not through the orchestrator.
- Two Azahar instances in the same user dir stomp each other; every seat must
  get its own `--user` dir (this is why the flag exists).

## 2026-11-13 (5) — Room join no longer pops the chat window; Mii seed proven

**Verdict: the pop-up is gone.** `MultiplayerState::OnNetworkStateChanged` called
`OnOpenNetworkRoom()` on every `Joined`/`Moderator` transition, which built and
raised the `ClientRoomWindow` (nickname list + chat) on top of the game. That
call is the one line removed; the toolbar's Room action still opens it on
demand. Rebuilt `bin/Release/azahar`, started a fresh 2-player session from the
orchestrator: both instances print `room: state 3` (Joined) and `wmctrl -l`
lists only the two game windows plus the orchestrator — no room/chat dialog.

Two facts worth keeping from the same session:

- **MK7's "no Mii characters" dialog is solved by the seed in
  `src-tauri/src/slots.rs`.** The database must be the full 0x4BD20-byte
  CFL_DB (CFOG header + 100 records + CFHE list + CFRA section), and the two
  CRC-16/XMODEM checksums are **big-endian**, not little-endian (written
  little-endian MK7 silently reformats the file to an empty database; the Mii
  ID also has to look real — bit 31/28 set, e.g. `0x98391EE4`). With that, MK7
  boots straight to its main menu with no dialog, on both seats.
- **The orchestrator window intercepts touch.** It is 900x640 in the middle of
  the screen and covers the bottom-right touch panel of a 959-wide tile, so
  taps silently do nothing while it is up. Minimise it (or move it clear)
  before driving a seat with `xdotool mousemove --window <win>` + mousedown/up.

## 2026-11-13 (6) — MK7 Local Play group discovery fixed (it is DLP, not UDS)

**Verdict: the joiner now lists the host's group and both seats reach a local
multiplayer game (confirmed by the owner driving it, and by the logs below).**

Root cause: MK7's “Choose a group” list is filled by **DLP (Download Play)
beacons**, not by UDS. The host runs `DLP_SRVR::StartHosting`, which broadcasts
packet 1 with `child_title_id = title_broadcast_info.title_id`; the joiner's
`DLP_Clt_Base::StartScan(title_id_filter)` drops any beacon whose
`child_title_id` does not match. Our ROM is a trimmed `.3ds`, so
`CacheContentFileInMemory` failed (“Could not open DLP child archive” →
“Unable to load cia file”), `title_broadcast_info` stayed zeroed, the beacon
carried `child_title_id = 0`, and every scan logged “Got title info, but it did
not match title id filter” — four empty `0/8` slots forever. UDS was innocent:
the room relayed 355-byte UDS beacons the whole time.

Fix (`src/core/hle/service/dlp/dlp_srvr.cpp`, 2 lines + comment): seed
`title_broadcast_info.title_id = (program_id & 0xFFFFFFFF) | DLP_CHILD_TID_HIGH`
before trying to load the child content, so the beacon advertises the right id
even when the CIA is missing. The filter MK7 passes is `0x0004000100030700` and
the patch broadcasts exactly that (logged at Debug from `StartScan`).

Evidence, live 3-seat session after the patch (959x516 tiles, 2x2 layout):

- P1: `DLP_SRVR: StartHosting` then `ServerConnectionManager: Started` →
  `Client connected` at 19.5 s and again at 22.9 s.
- P2/P3: `StopScan` → `StartSession` ×5 → group joined (no title-filter
  warnings at all, against dozens per second before).
- Per instance in that session: 686–759 MB RSS, 27–29 % CPU during boot.
- DLP content distribution is still unavailable without the CIA; only listing
  and joining are needed for MK7 local play, so this is fine.

Slot configs are back to a minimal `[Layout]`/`[UI]` snippet (the temporary
`Service.NWM:Debug` filter used for this hunt was removed); the `StartScan`
title-filter line stays as a Debug log.

## 2026-11-13 (7) — Three seats in one MK7 local game (verified)

**Verdict: 3-player local play works end to end.** Host + 2 joiners, one
`azahar-room` on loopback, 959x516 tiles in the 2x2 layout, all three in the same
MK7 multiplayer game and running at speed (owner-confirmed).

Node bookkeeping from the host's log in that session (temporary `uds-debug`
probes, since removed):

```
12.38  BeginHostingNetwork max_nodes=8 channel=0      (DLP group network)
16.98  spectator joined (no node id)                  (joiner 1 previews the group)
19.38  spectator joined (no node id)                  (joiner 2 previews the group)
20.69  client joined as node 2 (total 2/8)
23.45  client joined as node 3 (total 3/8)
30.53  DLP EndHosting
30.63  BeginHostingNetwork max_nodes=8 channel=0      (race network)
31.24  client joined as node 2 (total 2/8)
31.29  client joined as node 3 (total 3/8)
```

So the flow is exactly two UDS phases: a DLP network for group discovery (each
joiner connects once as a spectator to read the beacon's 5 broadcast packets),
then MK7 tears DLP down and hosts the real race network, where both joiners
rejoin as nodes 2 and 3. `max_nodes=8` throughout, so the UDS side is not the
limit for up to 8 players.

Per instance during that run (3 tiles): 805 / 809 / 820 MB RSS, ~40 % CPU each
(ps lifetime average, boot included).

**The earlier “hangs and player 3 drops” was not reproducible.** In that session
the DLP manager logged `GetClientInfo: Could not get node info for network node
id 0x2/0x3` and disconnected both clients, then the host never completed the
race-network phase — but that session had been left half-joined across several
script restarts. A clean start does not show it. If it reappears, the first
thing to check is whether `BeginHostingNetwork` for the race network happened at
all; without it the clients stay connected to a network that stops advertising.

## 2026-11-13 (8) — Per-seat names (lobbies were blank)

**Verdict: every seat now has its own name, in both places the game can read it.**

- **Mii name was written big-endian and MK7 rendered nothing.** `Mii::Nickname`
  is `std::array<char16_t, 10>` (`src/core/hle/mii.h`), i.e. native UTF-16, while
  every numeric field around it is an explicit `u16_be`/`u32_be`. The seed wrote
  the name as UTF-16BE, so MK7 decoded CJK code points and the lobby showed
  blank names. `slots.rs` now writes the Mii and author names little-endian and
  names each seat's Mii `Player 1`…`Player 4` (record at 0x08+0x1A). Existing
  slots are re-seeded when the stored name does not match.
- **Console username was empty.** `--player-name` only reached the room. It now
  also sets the console's CFG username in `GMainWindow::JoinRoom` (truncated to
  the 10 UTF-16 units a config block holds), so games and DLP beacons see
  `Player N`. Verified from stdout: `console: username is now "Player 1"` /
  `"Player 2"`, read back through `Service::CFG::GetUsername`.
- Payload check after a start: `p1/p2/p3` `CFL_DB.dat` names decode as
  `Player 1` / `Player 2` / `Player 3` with raw bytes
  `50006c00610079006500720020003100` (UTF-16LE).

## 2026-11-13 (9) — Four players in one race, and fps you can read

**Verdict: 4-seat couch play works end to end; frame rate is now a number, not a guess.**

- **Four seats, one lobby.** Owner ran `players: 4` on a 1920x1034 work area:
  tiles `(0,0)`, `(960,0)`, `(0,517)`, `(960,517)` at 959x516 (1 px gutter), room
  `127.0.0.1:47942` with 4 members. Host log: `DLP StartHosting` at 18.0 s,
  three `Client connected` (28.4 / 30.9 / 33.3 s), `EndHosting` at 37.1 s, then
  the race network with nodes 2–4. Joiners `StartSession` at 26.1–26.9 s.
  Owner's verdict: fully playable at near full speed.
- **Measured cost during a real 4-seat race:** 805 / 816 / 827 / 810 MB RSS and
  sampled 88.0 / 86.5 / 85.0 / 88.5 % CPU **per instance** (≈3.5 cores of 8,
  load 5.2, 7.5 GB of 15.5 GB used) — busy but not saturating the box.
- **`--stats <file>`** (`citra_qt.cpp`, `EnableStatsLog`): stops the status-bar
  timer that normally consumes `GetAndResetPerfStats()` and appends one
  `fps=… speed=… frame_ms=…` line per second instead. Orchestrator passes it per
  seat, so each instance writes `slots/Player N.fps`. Measured with four seats
  on screen: 60.0 fps (min 59) on every seat, `speed` 99.9–100.3 %, frame
  1.6–6.1 ms in menus/demo races.
- **`tools/qa/monitor.sh start|stop|report`** — the human-driven QA loop: samples
  cpu/rss + newest fps line per instance once a second, counts new
  Error/Critical lines per seat, and summarises avg/min/max, fps<55 samples and
  the offending log lines (benign `ClCertA` / FS-delay noise filtered).
- **Automation lesson (do not repeat):** MK7's main menu is touch-first and
  self-driving it is a tarpit — `xdotool --window` coordinate moves land on
  whichever tile owns that screen point, and synthetic key events reach only the
  focused instance. Hence: human drives, agent records. Bottom-screen taps for
  the 959x516 tile, if ever needed: menu rows y≈396/426/456, "Create a Group"
  at (899,484), group slot 1 at (880,392).

## 2026-11-13 (10) — One window: the seats are adopted, not composited

**Verdict: four seats now look and behave like one application window, with no
change to how the emulator renders.**

- **`tools/quadview/`** (new, standalone Qt6/Xlib project, ~190 lines): finds the
  emulator's own windows (`WM_CLASS == "Azahar"`, so the orchestrator's window is
  not a candidate), sorts them by geometry, and reparents them into a frameless
  top-level window with `QWindow::fromWinId()` + `createWindowContainer()`. One
  window, one taskbar entry, one alt-tab. Each instance still renders its own
  frames into its own GL context, so there is no compositing hop and no added
  latency.
- **Input:** an embedded window is a child, so the window manager never gives it
  the keyboard. The container polls the pointer and hands X input focus to the
  seat under it (`XGetInputFocus` guard, 200 ms timer). Verified: pointer over
  tile 1 → `xdotool getwindowfocus` returns the emulator window `0x2400013`, and
  START reaches that seat only (its screen went to the main menu while the other
  three stayed on the attract demo).
- **Automatic:** `process.rs` spawns the container after the seats when the
  binary exists, logs `viewer: single-window container adopted the seats`, and
  tears it down first on Stop (it reparents the children back to the root window
  on a clean exit, so the seats survive a container restart).
- **Measured with the container up:** 1920x1034 window, tiles 959x516 with the
  1 px gutter, 59.0-60.0 fps and 98.6-100.4 % speed on all four seats. One
  31.0 fps sample appeared exactly while a capture ran; steady state is 60.
- **Limits, stated plainly:** X11/XWayland only (both the emulator and the
  container need `QT_QPA_PLATFORM=xcb`); XEmbed is deprecated, so this is a
  stopgap, not the destination. The real single-window answer is the in-process
  quad-system (four `Core::System` in one process, thread-local
  `System::GetInstance()`, one composited frame) which is also the libretro
  shape.
- **Incidental finding:** `Test Roms/` now holds six proper `.3ds` dumps.
  Kirby's Extra Epic Yarn fails to load with
  `Failed to determine system mode (Error 8)` / "Encrypted application" — the
  dump is still encrypted and the slots carry no `aes_keys.txt`/`seeddb.bin`.
  The orchestrator happily launched four seats into that dead end; a pre-flight
  "this ROM did not boot" check belongs in the QA loop.

## 2026-11-13 (11) — P1: frames in shared memory

**Verdict: an instance can publish its frames to a viewer at 57 fps without
touching the renderer.** Mission: `plans/single-window-shm.md`.

- **`--shm-frame <name>`** (`src/citra_qt/citra_qt.cpp`, `bootmanager.cpp`,
  new `src/citra_qt/shm_frame.{h,cpp}`) creates `/dev/shm/<name>` with a 64-byte
  header (magic `AZSH`, version, size, stride, format, 3 slots, `seq`,
  `request`) and three BGRA8 slots, then publishes a frame per presentation.
  Format is BGRA, bottom-up — exactly what `Renderer::RequestScreenshot` already
  writes (`glReadPixels(GL_BGRA, ...)`), so no colour or row conversion happens
  in the producer.
- **Producer hook (the useful discovery):** the Qt widget's present loop is
  paced by the compositor and *stops entirely when the window is hidden* — an
  early hook there published **1 frame/s** (measured: 86 of 103 `Present()` calls
  bailed on `!isVisible()`). The fix is a 16 ms `QTimer` on `GRenderWindow` that
  arms `Renderer::RequestScreenshot`; the renderer fulfils it inside its own
  per-frame `SwapBuffers` at 60 Hz. Measured **57 frames/s** published, one
  readback in flight at a time (atomic `compare_exchange` gate, safe across the
  GUI and GPU threads).
- **Verified by decoding, not by trusting the counter:** `python3` maps the shm
  object, reads the header (400x480, stride 1600, 3 slots of 768,000 B), grabs
  the newest slot and writes a PNG — a correct, right-way-up Mario Kart 7
  title screen. 2,304,064 bytes total for the mapping.
- **Cost:** +3 % of one core per instance at the title screen (13 % vs 10 %,
  8 s samples), i.e. ~0.12 core for four seats. At 400x480x4 x 57 fps that is
  ~44 MB/s per seat, ~175 MB/s for the full session — the reason the plan pulls
  frames rather than pushing them.
- **Open for P2:** confirm the ring keeps publishing with the window hidden
  (`--windowed` off-screen or 1x1), which is what the mission's hidden-producer
  design needs.

## 2026-11-13 (12) — one window: the shared-memory rework is the path

**Verdict: four seats render into shared memory and one window composites them;
the XEmbed stopgap is gone.** Mission: `plans/single-window-shm.md`.

- **`tools/viewer`** (new, standalone Qt6 + GL, ~450 lines): one window, one
  context, one textured quad per seat. It maps each ring read-only, copies the
  newest complete slot (re-reading `seq` after the copy, seqlock style, so a torn
  frame is never shown), uploads it as BGRA and draws it letterboxed into its
  cell. Click a tile to select a seat; keys and touch go to that seat only.
  Escape quits.
- **Producer input**: `--input-socket <path>` (new `src/citra_qt/input_socket.{h,cpp}`)
  binds a unix datagram socket and a `QSocketNotifier` on the GUI thread applies
  each 12-byte message through the same calls the widget's own handlers make
  (`PressKey`/`ReleaseKey`, `TouchPressed/Moved/Released`). The path is unlinked
  on close, and a stale socket from a crash is taken over on open.
- **Seats are headless**: `--shm-frame` now also sets `WA_DontShowOnScreen` and
  hides the menu/status bars, so a session maps exactly one window. `--position`
  / `--size` / `--borderless` are no longer used by the session.
- **Orchestrator** (`src-tauri/src/process.rs`): seats spawn with ring
  `azsh-<pid>-<n>` and socket `slots/<seat>.sock`, the viewer spawns once after
  the boot check, and teardown unlinks every ring and socket (covering killed
  seats, not just clean exits). `QT_QPA_PLATFORM` comes from
  `AZAHAR_QT_PLATFORM` (default `xcb`, the proven path; `wayland` for the
  native test). New `--start <rom> [--players N]` runs a session without the
  picker and tears it down on SIGINT/SIGTERM. `tools/quadview` is deleted.

Verified:
- **Composite**: a 4-seat session (orchestrator, 4 rings of 1,382,464 B + 4
  sockets, one `azahar-viewer`), and the viewer's own framebuffer read back with
  `--dump /tmp/composite.png 4` shows four live MK7 races — correct colours,
  right way up, 2x2 grid with letterboxing. `wmctrl -l` showed the session as one
  `Azahar Splitscreen` window.
- **Input isolation**: pressing HOME (Qt key 66) as a datagram to seat 1's socket
  only — seat 1 went from racing back to MK7's title screen; seat 2 was
  untouched. The producer-side socket and the viewer-side send were checked
  separately: a real X keypress in the viewer produced `type=1 key=65` and
  `type=2 key=65`, 12 bytes each, against a listener bound to the seat path.
- **Teardown**: SIGTERM to the orchestrator left 0 azahar processes, 0 rings,
  0 sockets and 0 windows.

Not verified, and why:
- **Presented fps.** The whole GL stack is stalled in this environment: glxgears
  itself reports **1.0–1.8 FPS** on `:1` while nothing is being composited, and
  the viewer's own `paint_ms` is 0.3–1.3 ms, so its 1 Hz is swap-wait, not work.
  This is the same phenomenon as the earlier emulator "1 Hz present loop"
  diagnosis. The viewer paces itself with a timer and `setSwapInterval(0)` so a
  stalled compositor costs latency, not throughput; re-measure with the screen
  live via `./run.sh`.
- **Native Wayland.** The viewer initializes its GL context under
  `QT_QPA_PLATFORM=wayland` but never gets a paint there, consistent with the
  same stalled compositor. `AZAHAR_QT_PLATFORM=wayland ./run.sh` is the test.
- **Touch effect in-game** (mouse → touch datagrams): the path is the same as
  keys and the message was verified on the wire for keys; no touch was acted on
  in a running game.

Readback cost, for the record: +3 % of one core per seat at the title screen
(13 % vs 10 % over 8 s), ~44 MB/s per seat at 57 fps.

## 2026-11-13 (13) — touch reaches the game; the seat's layout was the bug

**Verdict: a touch datagram to a seat's socket now moves the emulated 3DS's UI.
It did not before — the coordinates were dropped one layer below the socket,
because a headless seat has no window to size its touch layout from.**

- **The bug**: `EmuWindow::TouchPressed` rejects any point outside
  `framebuffer_layout.bottom_screen`, and that layout is derived from the
  *window* size. A seat that only publishes frames has a 1x1 window, so every
  forwarded touch failed `IsWithinTouchscreen` and returned false. Keys were
  unaffected (they never consult the layout), which is exactly why HOME worked
  while touch silently did nothing.
- **The fix** (`bootmanager.cpp`, `PublishSharedFrame`): when the emulator's
  layout differs from the frame geometry the viewer draws, call
  `UpdateCurrentFramebufferLayout(layout.width, layout.height)`. Three lines; the
  seat's touch layout now matches the frames it publishes.
- **A second, viewer-side bug found by the same test**: the viewer mapped mouse
  clicks to a hardcoded 400x480 and to the *tile* rect rather than the
  letterboxed image rect, so a click could never land on the bottom screen (its
  position depends on the seat's layout). Now `image_rect()` / `to_frame()`
  derive both from the seat's own frame size.
- **Verified from the seat's own ring frames**, seats A and B both parked at
  MK7's title screen (inter-frame activity < 2/255, bottom menu lit):
  - press + move + release at frame (440,195) — the bottom-screen "Single
    Player" row — changed **only that row** on seat A (band diff 30.8; rows 2, 3
    and the footer 0.00) while seat B's same band moved 1.95 from animation. The
    point maps where intended and reaches only the addressed seat.
  - a clean tap (down, 250 ms, up, no move) at the same point took seat A from
    the title screen into a **single-player race**: change 165.9 within 0.4 s,
    166.2 sustained, and 4 s later the frame is the race; seat B stayed at the
    title throughout (change 0.01–0.04).
- **Viewer click path**: a real click on the viewer's bottom-screen area against
  a listener socket produced `touch down x=422 y=186` and `touch up`, 12 bytes
  each — frame-space coordinates inside the bottom screen.
- **The session still holds** after both fixes: `--start … --players 4` →
  4 azahar + 1 room + 1 viewer, 4 rings, 4 sockets, exactly one `Azahar
  Splitscreen` window in `wmctrl -l`, seat fps 60.0 (frame_ms 8.97 with the
  readback); SIGTERM → 0 processes, 0 rings, 0 sockets, 0 windows.
- **Known blocked check — presented fps.** `glxgears` on this display reports
  1.0–1.8 FPS while nothing is composited, so any swap-paced loop reads ~1 Hz
  regardless of our code (the viewer's own `paint_ms` is 0.3–1.3 ms: swap wait,
  not work). This is an environment limitation, not a result. Rerun it once the
  screen is compositing: `./run.sh` (then click Start) and
  `tools/qa/monitor.sh report`, which now summarises the viewer's
  `presented_fps` alongside each seat; or `./run.sh --start "<rom>" --players 4`
  for a session without the picker. The native-Wayland run is the same test with
  `AZAHAR_QT_PLATFORM=wayland` set.

## 2026-11-13 (14) — native-Wayland run: seats yes, viewer window never exposed

**Verdict: a 4-seat session works end to end on native Wayland as far as frames
and input go; the viewer's window is never exposed by this compositor, so it
presents nothing. The mission's "no XWayland for the viewer" acceptance item is
therefore still open — and it is now recorded as a specific, reproducible
failure rather than an unknown.**

How it was run:
`AZAHAR_QT_PLATFORM=wayland ./src-tauri/target/release/azahar-splitscreen
--start "<rom>" --players 4`, with `WAYLAND_DISPLAY=wayland-0` and no `DISPLAY`.

What worked (all measured, not inferred):
- **Room and seats on native Wayland**: room open, 4 azahar + 1 viewer + 1 room
  processes; every seat's `--stats` line reports 59–60 fps (speed ~99–100 %), so
  the platform does not change emulation behaviour.
- **Frames**: four 480x240 rings created and each publishing **~46–50 frames/s**
  when sampled over 2 s (four seats share the box; xcb measured ~55 when fewer
  seats were running).
- **Input**: with both seats parked at MK7's title, a tap at frame (440,195) on
  seat 1 only produced a **127.5/255** frame change (the game left the title)
  while the control seat changed **0.04**. So the socket → touch → emulated
  screen path is platform-independent.
- **Teardown**: SIGTERM → 0 azahar processes, 0 `/dev/shm` rings, 0 sockets.

What failed, precisely:
- **The viewer never paints on native Wayland.** It starts, opens all four rings,
  and creates a GL 4.6 core context, but `paintGL` is never called, so there is
  no `presented_fps` line and nothing is shown. Temporary instrumentation (added,
  measured, removed) reported `platform=wayland widget=1280x720 visible=1
  exposed=0` windowed and `1829x985 ... exposed=0` frameless — the window is
  created and sized but **never exposed**.
- The compositor is not silent: `WAYLAND_DEBUG=1` shows `xdg_toplevel.configure`
  arriving (3x), `xdg_surface.configure` (2x), and Mesa's EGL queue attaching and
  committing buffers with `wl_callback.done` replies. `qdbus org.kde.KWin
  /Compositor org.kde.kwin.Compositing.active` is `true`, type `gl2`.
- Variants tried, all identical (context OK, no paint): `QT_OPENGL=software`,
  `QT_WAYLAND_DISABLE_WINDOWDECORATION=1`, `QT_SCALE_FACTOR=1` +
  `QT_ENABLE_HIGHDPI_SCALING=0`, windowed vs frameless fullscreen.
- Read together with the X11 result (XWayland clients *are* exposed, but swap at
  ~1.8 fps: `glxgears` itself), the pattern is "this session composites X11
  windows and not new Wayland toplevels". That points at the session/compositor
  state rather than at the viewer's code, but it is not proven — a human at the
  machine should re-run this once and look at the screen.

Next step if it reproduces with a live screen: treat it as a viewer bug and give
the Wayland path a non-GL widget fallback (QPainter over `wl_shm`), or start the
viewer on xcb while keeping the seats on Wayland (they are unmapped anyway).

## 2026-11-14 (15) — Native Wayland viewer: root cause found, viewer presents

Entry (14) left the viewer unexplained: window created, `xdg_toplevel.configure`
arrived, `paintGL` never called, `isExposed=0`. The fault was in Qt's Wayland
handshake, not in the viewer's code, and the viewer now presents.

**Isolating it** — three minimal clients in the same session (`WAYLAND_DISPLAY=wayland-0`,
no `DISPLAY`), all built in `/tmp` and removed afterwards:

- **Raw `wl_shm` + xdg-shell client** (no Qt, no GL, ~150 lines of C): binds
  `wl_compositor`/`wl_shm`/`xdg_wm_base`, creates a toplevel, commits a 640x480 shm
  buffer. Gets `xdg_toplevel.configure 0x0` → `xdg_surface.configure` →
  `xdg_toplevel.configure 640x480`, so the window **is mapped**. But in 35 s it got
  **0 `wl_callback.done`** (1 in one earlier debug run). Nothing in this session is
  presenting at a usable rate.
- **Minimal Qt raster window** (no GL): configure 640x480 WindowActive, then Qt's own
  log names it — `qt.qpa.wayland.backingstore: Didn't receive frame callback in time,
  window should now be inexposed` → `exposed=0`, paints frozen at 2.
- **Minimal `QOpenGLWidget`**: `initializeGL ok`, EGL commits happen, same inexpose,
  paints frozen at 3.

So the viewer is not the only victim: **the compositor maps toplevels but never
delivers frame callbacks**, which matches `glxgears` at 1.0–1.8 FPS on XWayland —
this session's display path is stalled for every client, Qt or not.

**The fix** (viewer, 4 lines): Qt inexposes a window after a missed frame callback,
and an inexposed window is never painted again even though it is still mapped. The
viewer paces itself with its own timer, so it must not depend on that handshake:
`tools/viewer/main.cpp` sets `QT_WAYLAND_FRAME_CALLBACK_TIMEOUT=60000` when unset,
before the application exists. A healthy compositor answers in milliseconds and never
notices. With it, both probes and the viewer stay `exposed=1` and paint.

Also the paint timer moved 8 ms → 16 ms: with a swap interval, 125 present/s was
double the display rate for nothing (13.3 % of a core); 16 ms gives 62.5/s at 7.7 %.

**Verified in one clean 4-seat Wayland run** (`AZAHAR_QT_PLATFORM=wayland`, no `DISPLAY`):

- one window: `viewer: GL 4.6 (Core Profile) Mesa 25.0.7 | 4 seats | window 1829x1029`;
- presented: `ticks=62 presented_fps=62.4 paint_ms=0.56 seat0_seq=…`, sustained
  62.4–62.6 over the run with all four seat seqs climbing;
- seats: 60.0 / 60.0 / 59.0 / 60.0 fps at 98.6–100.5 % speed (the first ~30 s of boot
  dips to 44–55, then settles);
- frames: 49–53 fps per ring over 2 s;
- input: a tap at frame (440,195) on seat 1 only changed seat 1 by 127.71/255 and the
  control seat by 0.03;
- teardown: SIGTERM → 0 processes, 0 rings, 0 sockets.

**Honest limit:** `presented_fps` counts the viewer's own swap/commit pace, so it
proves the window is exposed and painting, not that the panel actually shows the
frames — here no `wl_callback.done` ever arrives, so the compositor is not charging
for them. Whether the physical screen shows the picture is still a human check.

## 2026-11-14 (16) — In-process seats: core is ready, presentation is not (no-go, for now)

The mission asked whether four `Core::System` instances can live in one process and
present in one window, so the four-process + ring design can be dropped. Spike built
and run; the answer is **no-go for now**, and the reason is not the one the plan
expected.

What was built (`--inproc-seats N <rom>`, `src/citra_qt/inproc_seats.{h,cpp}`): N
Systems, N `EmuThread`s and N `GRenderWindow`s tiled in one window, each seat keeping
its production ring and input socket so the numbers compare directly. The core change
it needed is small: `Core::System::GetInstance()` returns a thread-local current
System (`SetCurrent`, fallback to the singleton), and `GRenderWindow::main_context`
stopped being a process-wide static — one GL context per window is what allows seats
to render on their own threads. Two more small fixes fell out: the frame limiter has
to be released per seat (otherwise the emulation thread waits forever before its first
frame), and `Service::AttemptLLE` threw on an empty `lle_modules` map when no config
was loaded.

Measured (60 s run, xcb, 4 seats): seats 0/2/3 published rings at **53–58 fps** with
distinct, advancing frame hashes; **seat 1 never published a frame** (seq 0) although
its emulation and present threads were alive — its screenshot request never completed.
Process totals: **1 900 MB RSS, 34 % CPU**, one process. A tap on seat 0's socket
changed no seat's frame, so in-process input routing was not proven. SIGTERM left the
process alive with its rings and sockets (hang in `EmuThread::~EmuThread` destroying a
condvar of an unjoined thread); one earlier run died with a SIGSEGV.

Against the four-process design (history (12)/(13)/(15)): 59–60 fps per seat, 49–53 fps
rings, ~700–880 MB **per** seat (~3.4 GB), ~35–40 % CPU **per** seat, and a clean
0/0/0 teardown. So in-process memory is clearly better, CPU is a wash, and reliability
is much worse.

Verdict and conditions are written up in `plans/in-process-seats.md`: keep the four
processes, and treat the in-process shape as worth one more attempt only after the
seat gets its own `EmuWindow`/presentation loop (no single-window present thread),
deterministic shutdown, and a 4/4-seat 60 s run.

## 2026-11-14 (17) — In-process seats, second pass: presentation fixed, guest exits remain (no-go)

The previous verdict blamed presentation; that was only half right, so the lever was
taken. Each seat now has its own publication loop (no shared present thread at all — a
hidden `GRenderWindow` per seat publishes its own ring from its own timer) and one
software compositor window draws the four rings. Teardown was fixed the same way:
stop wakes the frame limiter, waits in bounded steps and only then terminates, so
`EmuThread::~EmuThread` no longer destroys a condition variable a live thread waits on.

Result, one clean run of `--inproc-seats 4` (xcb, 50 s): seats 0/2/3 publish at
54–57 fps with per-seat sequences and hashes that differ whenever the screens differ;
**seat 1's guest exits on its own** (`status=16`, `ShutdownRequested`, i.e.
`svcExitProcess`/APT reset), so the seat is dead rather than starved. One process,
46 threads, 1 871 MB RSS, ~74 % CPU. SIGTERM → 0 processes, 0 rings, 0 sockets (one
run exited within a second). Input still not proven: taps at both the 480x240 and the
stacked 400x480 bottom screen changed nothing.

The discriminating experiment in the record: the same ROM and the same user directory
as **four separate processes** all publish (seq 2528/2320/2205/2051 over 40 s), and a
single in-process seat runs normally (59.0 fps emulation, 55 fps ring). So the profile
on disk is not the problem — being in one process is, which means remaining
process-global emulator state ends a guest seat. Keys are also process-global
(`InputCommon::GetKeyboard()`), so a seat cannot own its keys in one process.

Verdict stays **no-go**, with the cause corrected in `plans/in-process-seats.md`
(candidates and the bisect to run next are listed there). The four-process design
remains the shipping path; the reusable wins from this spike are the thread-local
`System`, one GL context per window, and the per-seat ring/socket/loop plumbing.

## 2026-11-14 (18) - in-process seats: input proven delivered, guest refuses it

Took the input gap of the four-seats-in-one-process spike to a measurement instead of
a retry of coordinates: the tap is driven straight into a seat's input socket from the
harness, and the seat's HID service is instrumented to show receipt.

Two real drops were found and fixed. The spike never loaded an input profile, so HID
created no devices at all and both touch and keys were discarded; and the touch factory
was process-global, so every window would have shared the first seat's touch state.
Each window now owns `emu_window_<ptr>` and `Core::System` carries a touch-device
override that HID prefers, leaving single-window and four-process configs untouched.

Measured, one clean four-seat run (53-57 fps each, 2 291 MB RSS, ~96 % CPU): a tap on
seat 0's socket is accepted by seat 0's window and reported by seat 0's System's HID
(`pressed=1 x=160 y=160`) with the other three seats' HID silent, and SIGTERM stops all
four seats leaving 0 processes, 0 rings and 0 sockets. On Mario Kart 7 the guest ignores
it: four different screens (dialog OK, 150cc row, Time Trials row with hold+move,
character tile) each received the tap and kept their state, while A/START on the same
seat advance four screens. Recorded as a distinct input-model blocker rather than
"not proven", together with two measured limits: keys are still process-global, and with
two or more seats every ring publishes a blank frame (1 seat: real picture), so a
per-seat visual delta cannot be compared in a four-seat run yet.

Verdict unchanged: **no-go** for one-process seats. See `plans/in-process-seats.md`.

## 2026-11-14 (19) - MK7 does take the synthetic touch; the earlier negative was aim

Built `tools/touchprobe`, a devkitARM homebrew that prints what libctru reads from the
emulated touch panel, so the comparison had a title that responds by construction.
Driven through a seat's socket it reads back `x=160 y=120 presses=1` for a tap at
`(200,360)` and `x=80 y=100` for `(120,340)`, and its ring content changes and stays -
on the stripped build too.

HID entry timing is the same in both cases: 192 pressed entries for a 0.8 s hold, one
every ~4.1 ms (240 Hz), correct panel coordinates, no gaps. With the path proven, a tap
on Mario Kart 7's kart-parts screen moved the selection from the wheels column to the
glider column and changed the top screen from "Normal" to "Super Glider".

So the earlier "MK7 ignores synthetic touch" finding was a targeting error, not an
input-model blocker: the 150cc tap landed below the 150cc row, the Mii dialog's OK takes
A/START, the mode-menu tap only highlights, and the run logged as a character-grid tap
was actually on the engine-class screen. Retracted in `plans/in-process-seats.md`; the
two real limits left are process-global keys and blank rings when two or more seats
publish.

## 2026-11-14 (20) - In-process seats: keys per seat, blank rings fixed, four-seat bar met

Both remaining defects were the touch bug in another place: state that belongs to one
seat was shared by the process.

**Keys.** `GRenderWindow::ApplyForwardedInput` pressed `InputCommon::GetKeyboard()`, so a
key sent to one seat's socket moved every seat. Now each `EmuWindow` owns a `KeyState`
factory registered as `keyboard_<ptr>`, `Core::System` carries
`SetKeyDeviceOverride`/`GetKeyDeviceOverride`, HID's `LoadInputDevices` rewrites
`engine:keyboard…` button strings to that per-System factory, and `ApplyForwardedInput`
calls the window's own `PressForwardedKey`/`ReleaseForwardedKey`. `engine:keyboard`
itself is untouched for single-window configs.

**Blank rings from two seats up.** `OpenGLState::cur_state` was one `static OpenGLState`,
the process-wide cache `Apply()` uses to skip redundant GL calls. Two renderers on two
emulation threads shared it, so each seat skipped binds the other had changed; the
screenshot framebuffer then read a different framebuffer/texture than the cache believed
and came back black. Instrumentation showed the readback path was healthy the whole time:
`glCheckFramebufferStatus` `GL_FRAMEBUFFER_COMPLETE`, `glReadPixels` 400x480, first pixel
`000000ff`, only occasional `glGetError` `0x502`. The same race crashed one two-seat run
with `Unhandled SIGSEGV` at ~27 s. `cur_state` is now `static thread_local` - one GL
context and one emulation thread per renderer, so per-thread is per-seat.

**Measured, four seats, stripped build** (xcb, 400x480 rings, `--inproc-seats 4`):

- probe title in all four seats: all four non-blank and static; `A` to seat 0 changes only
  seat 0 (seats 1-3 byte-identical, md5 `8b142f50` before and after); a touch at
  `(200,360)` to seat 1 changes only seat 1 (`3d57fa4e`), seats 0, 2, 3 unchanged;
- MK7 in all four seats: each ring carries that seat's own MK7 picture ("no Mii
  characters" dialog, 88 distinct byte values); `A` twice to seat 0 only walks seat 0
  dialog -> title -> main menu while seats 1-3 stay on the dialog in every dump; 4/4 live
  53-57 fps, 2 346 MB RSS, ~95-101 % CPU; SIGTERM -> exit 0, 0 processes, 0 rings, 0
  sockets;
- no SIGSEGV in any post-fix run (MK7 four-seat 67 s, probe four-seat, two-seat runs).

**Dead ends.** Per-seat GL contexts were already right (distinct share contexts and
surfaces, no `makeCurrent` failures, contexts created on the thread that uses them); the
screenshot FBO was complete and `DrawScreens` filled it; `RasterizerCache`/`res_cache` are
per-renderer members, not globals. MK7's "no Mii" dialog answers to A/START only - a tap on
its OK button does nothing - and its menu rows only highlight on a tap, so the game's
touch evidence remains the kart-parts tap from (19). MK7's screens pulse, so byte-identical
"untouched seat" comparisons are only possible with a static title; `tools/touchprobe`
(now also printing the last key mask) is that title. The "MK7 ignores touch" retraction
still stands.

**Next.** Soak four seats for several minutes (the runs above stopped at 67 s) and watch
for the second pass's `status=16` guest exit, which has not recurred since the per-seat
overrides.

## 2026-11-14 (21) - Ten-minute four-seat soak: no exit, crash, stall or leak

MK7 in all four in-process seats (`--inproc-seats 4`, xcb, 400x480 rings), 626 s of
per-second stats. Verdict: clean.

- **No guest exit.** 4/4 live from t=34 s (boot stagger) to t=626 s, every second
  reporting `live=4/4`; no `core stopped`, no `status=` line, no `seatN stopped`. The
  second pass's `status=16` exit did not recur in ten minutes.
- **No crash.** No SIGSEGV, no unhandled signal, process exit code 0. Only lines that
  look like errors are the three pre-existing `cubeb_set_log_callback failed` warnings
  emitted once per seat at boot.
- **No stalled ring.** Sequence numbers advanced every second on every seat: seat0
  34 -> 33 718, seat1 34 -> 33 544, seat2 26 -> 33 291, seat3 26 -> 31 407. Eleven
  ring samples spread over the run each read 88 distinct byte values (real picture,
  not blank) and changing hashes.
- **No leak.** RSS was 2 325 MB at t=36 s and still 2 325 MB at t=626 s (2 383 MB as
  reported by `ps`), thread count stayed at 51, CPU 93-113 % of one core, 50-59 fps per
  seat (mostly 53-57).
- **Clean SIGTERM.** `kill -TERM` -> `multiseat: stopped 4 seats`, process gone, 0
  rings, 0 sockets, no azahar processes left, no threads left behind (the process
  exit is the thread exit).

Method: the emulator ran under a persistent PTY writing `tee /tmp/soak.log`; samples
read `/dev/shm/azsh-inproc-N` (header + newest slot) and `ps` alongside the log. All
scratch drivers and the log were removed afterwards; no instrumentation was added to
the tree for this run.

## 2026-11-14 (22) - One compositor window, proven from the X side: four seats, four tiles

The rings were already proven; this closes the other half - that the single window really
shows all four guests - from the WM side, not from the app's own log. MK7,
`--inproc-seats 4`, xcb, 1280x720.

**Window.** `xwininfo` sampled every 2 s from launch through the staggered boots (seats
booted at t=2/12/20 s): one window, `Azahar in-process seats`, `0x1600013`,
`Map State: IsViewable`, `1280x720+319+171` in all 23 samples - no resize while seats
came up. `xprop -id ... _NET_WM_PID` puts it on the azahar pid; of the 9 toplevels that
process owns, exactly 1 is mapped (the rest are Qt selection-owner/utility windows, none
viewable). `xwd -id <window>` decodes to 1280x720.

**Tiles.** 2x2 tiles of 640x360. A 400x480 ring scaled with `KeepAspectRatio` is 300x360,
so each tile carries 170 px black bars left and right; measured in-picture
`sidebars=(0.0,0.0)` in every capture, and the 300x360 crop matches the ring downscaled to
the same size - the frame is not stretched to the tile.

**Mapping, one seat at a time.** From four MK7 "no Mii characters" dialogs (all mean 72),
pressing `A` twice on seat i turned only quadrant i bright (mean 155) and left the other
three on the dialog; each quadrant's nearest ring was its own seat (mean |diff| 3.2 vs
137+ for the other three). Four captures, order 0,1,2,3: b0 (all dialog), m0..m3.

**Advancing, three timed captures.** f1/f2/f3, 10 s apart, all four seats racing
(40-53 fps each, `live=4/4`):

| quad | f1 md5 / mean | f2 md5 / mean | f3 md5 / mean |
| ---- | ------------- | ------------- | ------------- |
| 0 | `0b33b8d1` / 123.5 | `71cdd26d` / 112.1 | `e2d4d32e` / 108.1 |
| 1 | `09aba90a` / 111.2 | `f9af5781` / 129.2 | `a021a593` / 97.8 |
| 2 | `4517a281` / 93.7 | `66513c9b` / 77.6 | `61e21987` / 82.8 |
| 3 | `cc2c5d0c` / 101.4 | `df54f9be` / 127.5 | `549e60aa` / 108.0 |

Every quadrant is non-blank (256 distinct byte values), its hash changes in all three
captures, and no two quadrants share a hash within a capture. Each quadrant's nearest ring
frame was its own seat in all 12 checks (|diff| 17.6-33.8 on moving pictures).

**No torn frame, no stale tile.** Repeated captures sometimes matched their ring poorly
(|diff| 15-50). That is the 0.2-0.5 s between `xwd` and the ring read on a 40-50 fps
screen, not a compositor drop: the same tiles match at 1.5-3.4 on static screens, and the
diagonal (own seat) still wins. A synthetic 3-slot ring publishing at 55 fps gave 0 torn
frames in 5207 reads using the compositor's exact read loop (copy one 768 KB slot in
~0.1 ms, far inside the 18 ms frame period, so the picked slot cannot be rewritten under
it). An experimental copy-then-recheck-sequence reader measured the same 0 and was
reverted: no defect to fix, no change kept.

**Shutdown.** SIGTERM -> `multiseat: stopped 4 seats`, exit 0, no azahar process, 0 rings,
0 sockets. XWD dumps, PNGs and logs lived in `/tmp` and were removed; no instrumentation
was added to the tree, `build/` untouched apart from the normal incremental rebuild.

## 2026-11-14 (23) - ROM matrix: two of six test ROMs boot four seats, four are encrypted

Ran the four-seat spike over every ROM in `Test Roms/` (80 s each: boot, four samples
20 s apart, one X capture, SIGTERM), then a five-minute soak on the second ROM that boots.

| ROM | seats | evidence | teardown |
| --- | --- | --- | --- |
| MK7 (EU, "Decrypted") | 4/4 | `live=4/4`, rings 88 distinct values, 2 760 MB RSS, tiles `051e6a/2f2656/0e6014/7bd110`, 1280x720 IsViewable | exit 0, stopped 4 seats, 0 leftovers |
| Monster Hunter Stories (EU, no suffix) | 4/4 | `live=4/4`, rings 85-88 distinct, 2 088 MB RSS, tiles `9e4da9/3672a9/afc001/829f5d` | exit 0, stopped 4 seats, 0 leftovers |
| NSMB2 (USA, "Decrypted") | 0/4 | `Failed to determine system mode (Error 8)`, `seatN failed to load the ROM (status 5)` | exit 0, stopped 4 seats |
| Mario Party - The Top 100 (USA) | 0/4 | same | exit 0, stopped 4 seats |
| Zelda - Tri Force Heroes (USA) | 0/4 | same | exit 0, stopped 4 seats |
| Kirby's Extra Epic Yarn (USA) | 0/4 | same | exit 0, stopped 4 seats |

- Loader status 8 is `Loader::ResultStatus::ErrorEncrypted` from `LoadKernelMemoryMode`
  (`src/core/core.cpp:407`): those four files are encrypted despite the "Decrypted" name,
  and no keys are installed. The stock frontend refuses the same Zelda file with an
  **"Encrypted application"** dialog (`xprop WM_NAME`), which puts the failure on the ROM,
  not the compositor.
- The spike handles that case without crashing: the window stays up with four black
  tiles, each seat logs its failure at boot, `live=0/4`, and SIGTERM still yields
  `stopped 4 seats`, exit 0, no rings, no sockets. What it does not do is say why on
  screen - that is the existing "pre-flight ROM check" item in `todo.md`, which now has a
  concrete case. Only MK7 and MHS are usable for four-seat testing on this machine.
- MK7 control numbers: 95-102 % CPU, RSS flat at 2 760 MB, four distinct tiles, one
  1280x720 mapped window, clean exit.

**MHS soak, 300 s.** 4/4 live in all ten samples, 417-430 % CPU, RSS 2 088 -> 2 089 MB
(flat), ring sequence 12 536/12 194/11 900/11 529 at 84-88 distinct byte values (64-78
during fades, never blank), window 1280x720 IsViewable throughout, four distinct tiles at
the end (`044d4a/f2ee1c/d514ae/3370de`, means 81/67/61/62), SIGTERM -> exit 0,
`stopped 4 seats`, 0 rings, 0 sockets. The heavier 2 GB ROM is as stable as MK7.

## 2026-11-14 (24) - Production path: `run.sh --start` soaks MK7 then MHS in one window

Ran the orchestrator's per-slot path (not the in-process harness) on `DISPLAY=:1`:
`./run.sh --start "<rom>" --players 4`, i.e. four headless seats with their own ring and
input socket, one `azahar-viewer` window, `azahar-room`. Two fixes were needed first.

**Fix 1 - the viewer could not present (GL -> QPainter).** With the old `QOpenGLWidget`
compositor the viewer logged `presented_fps=1.0` while every seat ran at 60 fps: this
session's compositor throttles `glXSwapBuffers` to about 1 Hz (`glxgears` 1.0-1.8 FPS,
history 15). `Viewer` is now a plain `QWidget` that paints each seat's `QImage` with
`QPainter::drawImage`; ring reading, tile math, input forwarding and `--dump` are
unchanged. Same session, same seats: `presented_fps` 60.7-62.7, `paint_ms` 4.7-11.9.

**Fix 2 - LargeScreen broke Monster Hunter Stories.** Every MHS seat showed the 3DS
system error dialog (ring md5 `12b159`, 197 byte values, mean 5) and ignored input, while
the same slots ran MK7 fine. Bisected against the slot's own seed data in throwaway user
dirs with the orchestrator's exact minimal `qt-config.ini`:

| seed in the temp dir | layout | ring | result |
| --- | --- | --- | --- |
| slot config + Mii DB | 2 (LargeScreen) | 480x240, `12b159` | system error dialog |
| slot config, Mii DB removed | 2 | 480x240, `12b159` | same - the Mii seed is innocent |
| slot config + extdata `metadata` | 2 | 480x240, `12b159` | same - not the extdata archive |
| minimal config | 2 | 480x240, `12b159` | same |
| minimal config | 0 (default) | 400x480, `7bee81` | game boots and renders |

`slots::prepare` therefore writes `layout_option=0` (top screen above the touch screen)
with a comment naming MHS. Cost: the top screen is 431x259 in a 960x517 tile instead of
the full tile, and the touch screen is visible, which is what a game with touch-only
menus needs anyway.

**MK7, 290 s, four seats.** 6 processes (4x `azahar`, `azahar-room`, `azahar-viewer`),
RSS 3 509 MB, rings 400x480 (stride 1600) with 85-88 distinct byte values:

| t (s) | ring seq 1..4 | md5 1..4 | viewer |
| --- | --- | --- | --- |
| 120 | 1004 / 825 / 745 / 653 | `e2dedd / c9e104 / f84ae9 / 424bdb` | 60.7 fps |
| 260 | 2004 / 1757 / 1720 / 1626 | `22f905 x3 / 3626ec` (transition: three seats black at once) | 62.6 fps |
| 280 | 2849 / 2578 / 2563 / 2445 | `fc75a1 / b7350f / 96f880 / 4ac0c1` | 61.9 fps |
| 290 | 3387 / 3131 / 3118 / 3000 | `3319c4 / c311c3 / 94d582 / eff56f` | 62.5 fps |

SIGTERM at t=290: 0 processes, 0 rings, 0 sockets, `slots/*.log` stops at the spawn
lines, no window left on `:1`.

**ROM swap into MHS, 345 s, same four slots.** Re-attach after the teardown worked with
no slot cleanup: 4 seats, rings 400x480, 85-88 distinct values, viewer 62.5 fps, RSS
2 889 MB. Two behaviours worth recording:

- A fresh slot shows the 3DS extdata prompt **"No extra data found. New extra data will
  be created."** on the bottom screen before MHS boots. It is touch-only: forwarded keys
  do nothing, one tap at frame (160,395) dismisses it, and the viewer's input path is
  confirmed end to end (each tap changes that seat's ring md5, other seats unaffected).
  It appears once per slot, so the first session per slot needs one tap per seat.
- After the prompt the four seats diverge on their own: t=300 s rings
  `2c65fd / 26c561 / 26c561 / 26c561` (seat 1 booted first), t=345 s
  `b6c483 / 9d822d / dd90ad / 7c1845` at seq 14 738 / 15 033 / 14 077 / 15 076.

SIGTERM at t=345: 0 processes, 0 rings, 0 sockets.

**Capture caveat for future sessions.** `xwd -id <viewer>` on this compositor returns the
unredirected surface: at one point all four tiles read near-black (mean 28) while
`azahar-viewer --dump` of the same rings showed the MHS dialog in all four. `xwd -root`
fails with `BadMatch`. Use the viewer's own `--dump` for composition evidence and `xwd`
only as a smoke test; the tile-vs-ring pixel diff computed from `xwd` was 43-104 even when
the diagonal (own seat) won every time, because the image is scaled 1.077x and the ring
advances during the ~0.2 s between capture and read.

## 2026-11-14 (25) - Fresh slots clear the "No extra data found" prompt themselves

The prompt from (24) is the last manual step in a fresh session: a slot with no extdata for
the game stops on "No extra data found. New extra data will be created." and only a touch on
the bottom-screen Close button moves it on. Seats have no window, so `process.rs` now sends
that tap for the player: `clear_boot_prompt` fires three times (10 s, 16 s, 22 s after the
viewer starts), sending the same 12-byte touch datagrams the viewer sends, to every seat
socket. No per-game table and no new flag - one coordinate, three tries, and it is a no-op
in any game that has its data already.

Reproduced the fresh-slot state by deleting `sdmc/.../extdata/00000000/00001BC6` from all
four slots (extdata only; saves and NAND untouched), then `run.sh --start` MHS with nobody
at the keyboard:

| wall clock | ring md5 1..4 | what it is |
| --- | --- | --- |
| 10:52:22 | `7bee81 x4`, 85 values | the prompt, all four seats |
| 10:52:32 | `4b24ed / bb1a9b / d5491f / d5491f` | past it, seats already diverging |
| 10:52:52 | `6c5bb8 / 25f61c / 62dcff / 4c6066`, 88 | game content, 62.6 fps presented |

`azahar-viewer --dump` at that point shows MHS's opening movie in all four tiles, and the
extdata directory exists again in all four slots. SIGTERM: 0 processes, 0 rings, 0 sockets.

**MK7 regression.** The taps land on MK7's boot screen, which ignores them: distinct rings at
25 s (`957c05/21863d/89df5e/168b29`) and 40 s (`d7877f/4eda7e/3dccb9/e04b5d`), the usual
all-black transition at 55 s, content again at 75 s (`8ef59f/a095d8/918dc2/ad8054`), 62.6 fps
throughout, teardown 0/0/0.

## 2026-11-14 (26) - A dump that cannot boot says so in the one window

`check_boots` already caught an encrypted dump within a few seconds and tore the session
down, but the only trace was a line in a terminal the player is not reading; the stock
frontend's alternative (four black tiles, or an "Encrypted application" dialog per window)
is what this replaces. Now the failure names the ROM, and the last step of `--start` opens the
same viewer in a notice mode: no rings, no tiles, the reason centred on the TV, `Esc` closes,
exit code 1.

- `tools/viewer`: `--message "<text>"` builds a `Viewer` with no seats. It paints a dark
  window, the text word-wrapped and centred, and a grey "Press Esc to close."; the frame
  timer is not started, so a notice costs nothing. Escape already closes the window.
- `process.rs`: `check_boots` takes the ROM path, so the message reads `This game did not
  boot.\n\n<file>\n\nThe emulator reports an encrypted or unsupported application, and
  nothing was started...`; new `show_message` spawns the viewer with `--message`.
- `main.rs`: `--start` prints the same text and, when there is a viewer binary, holds that
  window open until `Esc`/SIGTERM before exiting 1. The picker path is unchanged: the
  command returns the string and the UI shows it in its status line.

**Evidence, all four seats.** `run.sh --start "New Super Mario Bros. 2 (USA) (En,Fr,Es)
Decrypted.3ds" --players 4`: stdout is the notice text above, the window (1920x1034,
captured with `xwd`) shows it with the file name and the keys hint, and `ps` holds only the
orchestrator and that window - the four seats and the room are already gone. SIGTERM:
`EXIT=1`, 0 processes, 0 rings, 0 sockets. A mistyped ROM name takes the same path with
`ROM not found: <path>`, so a bad path is on screen too. MK7 afterwards: four distinct rings
(`635e84/62a0dc/0b9f03/3f1a81`, 88 values each) at 62.5 fps, `EXIT=0`, 0 leftovers - the
normal path is untouched.

## 2026-11-14 (27) - P1-P4 badges on the tiles

A bare 2x2 of four identical screens tells a couch nothing about which pad is which seat.
`Viewer::draw_badge` now paints a `P<seat+1>` chip per tile, right after that tile's
`drawImage`: bold text at `tile.height()/26` px, a rounded fill of `QColor(18,18,24,200)` and
a 1 px light border so it reads on black or on the game. It sits immediately left of the
picture when the letterbox margin has room for it, which a 400x480 frame always does (the
image is 431 px wide in a 960 px cell) - so nothing of the game is covered - and drops into
the picture's top-left corner when the image fills the cell. Tiles, ring reading, the paint
timer and input forwarding are untouched; the notice mode returns before the seat loop, so
it never draws a badge.

First pass put the chip in the tile's outer corner, and the capture showed why that is wrong:
on a portrait frame it floated ~260 px away from its own picture, looking like a window
label rather than a seat label. The chip was moved beside the picture and given the visible
fill/border, then re-captured.

| session | capture | fps | exit |
| --- | --- | --- | --- |
| MK7, 4 players | 1920x1034 window: P1 top-left tile, P2 top-right, P3 bottom-left, P4 bottom-right, each beside its picture | 62.5 (`paint_ms` 4.84) | 0, 0 processes/rings/sockets |
| MK7, no `--players` (2) | 1920x1034 window: two tiles side by side, P1 and P2 | 62.6 | 0, 0 processes/rings |
| `--message` notice | text and Esc hint only, no badges | - | - |

The capture was `xwd -id <viewer>` converted with `xwdtopnm` and read back as an image, not
inferred from logs. The 2-player badge is larger (tiles are twice as tall) and sits in the
picture's corner because that layout leaves only ~49 px of margin.

## 2026-11-14 (28) - A dead or frozen seat restarts itself

One seat dying used to leave a frozen tile for the rest of the session (`is_running` only
needs *one* live child, so nothing noticed). `Session::watch()` now runs from the headless
loop (every 500 ms) and from the `session_running` command the picker polls: it reads each
ring's `seq` with a 8-byte `pread` at offset 32, and restarts the seat when its child has
exited or when it has published nothing for 15 s. The stall test only arms 30 s after a
spawn, because a cold MK7 seat stops publishing for several seconds while it compiles
shaders and joins the room - arming earlier made the watchdog fight the boot. Three
restarts per seat, then it gives up and the log says so.

Two things had to be fixed to make the replacement actually stick:

- **`room: error 9` = `RoomIsFull`.** The replacement rejoined while the room still counted
the dead member, was rejected, and then sat idle until the next restart. The room now gets
`players + 1` member slots, and the respawn waits 2 s after the old process is gone.
- **The stale ring.** A SIGKILLed seat leaves its `/dev/shm` object behind (no clean close),
and a replacement that inherited it published no frames at all - it was restarted again 30 s
later for "no frames". The watchdog unlinks the ring before respawning, so the replacement
creates a fresh object. The viewer now compares the object's inode with its mapping each
frame (`ring_replaced`) and re-attaches, logging `ring ... was replaced, re-attaching`.

| event | log | result |
| --- | --- | --- |
| SIGKILL seat 2 at t=35 s | `restarted (1/3): exited (signal: 9 (SIGKILL))` | one restart, new pid, ring 0 -> 2263 over 50 s, `presented_fps` 62.6, viewer re-attached |
| SIGSTOP seat 3 (frozen, still alive) | `restarted (1/3): no frames for 15s` | one restart, new pid, ring 0 -> 838 over 20 s, `presented_fps` 62.5 |
| both restarts in one session | - | window capture: P1 and P4 mid-race, P2 and P3 at the title, four badges, four distinct live tiles |

Clean session afterwards: 62.4 fps, `EXIT=0`, 0 processes/rings/sockets. The 2-player
default (no `--players`) still gives 2 rings, 62.4 fps, `EXIT=0`, 0 leftovers, and the
`--message` notice path is untouched (no `watch()` without a session). Note for anyone
testing by hand: a seat ignores plain SIGTERM, which is why the orchestrator's own teardown
uses SIGKILL after it.

## 2026-11-14 (29) - The tile says why it went black

A restart left the tile showing a frozen frame or black for ~15 s with no explanation; on a
TV that reads as a crash. The orchestrator now drops `slots/Player N.restart` beside the
seat's log at the moment it restarts it (the file holds the reason), removes it as soon as
that seat publishes another frame, removes it when the three-restart cap is reached so the
tile never claims a restart that will not happen, and clears leftovers on teardown and at
the next session's start. The viewer takes `--restart <ring>=<marker>` per seat and draws a
`P2 restarting...` pill centred in that tile for as long as the marker exists - no timers
and no guessing, the file's presence is the whole signal - and logs `restarting` /
`frames resumed` so the transition is in `slots/viewer.log` too.

The same pass found a second bug behind it: a seat that has just been restarted starts in
the 3DS's single-screen 400x240 mode and switches to 400x480 once the bottom screen
appears, but the viewer read the ring header once, so a re-attach during boot kept a
400x240 canvas and the tile showed only the top screen from then on. `ring_resized`
compares the header with the mapping every frame and re-opens when it changed.

| check | result |
| --- | --- |
| SIGKILL seat 2, capture at +4 s | `P2 restarting...` over that tile, other three live; marker present |
| same seat at +25 s | marker gone, `azsh-...-2 frames resumed` in `viewer.log`, tile live at full 400x480 |
| SIGSTOP seat 3 | `P3 restarting...` appeared with the stall restart, then cleared |
| in-session render (`--dump`) | pill crisp and centred over a live frame, `P1 restarting...` fully visible |

Clean session afterwards: 63.0 fps, `EXIT=0`, 0 processes/rings/sockets, no `*.restart` left
in `slots/`. Note when checking captures by hand: `xwd` can catch the window mid-repaint and
clip a freshly drawn chip in half; the viewer's own `--dump` writes the composed frame.

## 2026-11-14 (30) - Save sets: one for the group, or one player's imported

Two things a couch needs before a session: everyone starting from the same unlocked
profile, and one player bringing their own single-player progress to their own seat.
Both are the same operation - copy a save set into a seat before it boots - so there is
one module and two ways to choose the source.

A save set is an Azahar user dir (a slot's `slots/pN/user`, or an existing single-player
install). `saves::apply` copies two things: `sdmc/Nintendo 3DS` (SD save data and extra
data) and `nand/data/<title id>` (cart/installed save data, the id read from the ROM's
NCSD partition 0 NCCH header at 0x118). It deliberately does **not** copy `nand/private`
(console identity) or the shared Mii extdata `nand/data/<system>/extdata/00048000/F000000B`,
so each seat keeps its own identity and its own Mii name while playing someone else's
progress. `Mode::Keep` is the default; `Grouped` seeds every seat; `PerSeat` seeds only the
players named, and a missing path aborts the start before the room opens rather than
booting the couch without the save they asked for.

| check | result |
| --- | --- |
| grouped, four seats, sentinel file in the save set | four `save set from ... (12 files)` lines, the sentinel in p1-p4, 62.5 fps, `EXIT=0` |
| per-seat import (`--import 3=...`), p3 wiped first | only `Player 3` seeded, sentinel only in p3, the other three clean, `EXIT=0` |
| bad path (`--saves /tmp/nope`) | `save set for Player 1 not found: /tmp/nope`, the notice window shows it, `EXIT=1`, 0 processes left |
| seat identity | p2's Mii database is byte-identical across a grouped copy and still holds `Player 2`, not `Player 1` |
| MK7 boots from the copied set | four seats, 62.5 fps, rings advancing, clean teardown |

Limits worth knowing: NAND extdata whose id is not the title id (some installed titles)
is not covered - MK7 keeps its extra data on SD, which is the case tested. The copy assumes
the same SD console-id directory as the source (all-zero in these slots); a real console id
would need a mapping, which only matters once installed `.cia` saves are in scope.
The picker gained `Saves` (keep / same for every player / per player, one path field each).

## 2026-11-14 (31) - Saves driven from the window, not just the CLI

Driving the real picker (xdotool against the 900x640 Tauri window: type into fields, pick
ROM/players/saves by type-ahead, click Start and Stop, capture with `xwd`) found two things
that reading the HTML could not:

- **The per-player fields were always visible.** `#seatRows { display: grid }` is an author
  rule, so it beat the user-agent `[hidden] { display: none }`; the row now has
  `#seatRows[hidden] { display: none }`.
- **A failed start only updated the status line.** `show_message` (the one-window notice)
  was called on the CLI path only, so a window-driven bad path went quiet on the TV. The
  picker's command now keeps the notice as a child in `AppState`, shows it on a failed
  start, closes any previous one on the next start, and closes it when the window or app
  exits. A bad save path and an unreadable dump both land in that notice.

The window-driven runs, all on MK7 with a sentinel file inside the save set:

| action in the window | result |
| --- | --- |
| grouped, 4 players, `/tmp/gs` | four `save set from /tmp/gs (12 files)` lines, `GROUPED` in p1-p4, 4 seats + room + viewer, 62.4 fps |
| same run, seat identity | p2's Mii database byte-identical before and after (`b13bb8f6`) and still holds `Player 2` |
| per-seat, only P3 given `/tmp/ps` | one `Player 3` line, `PERS3` only in p3, the other three clean, 62.4 fps |
| Stop button | 0 processes, 0 rings, 0 sockets, status `Stopped.` |
| bad save path, grouped `/tmp/nope` | notice window reads `save set for Player 1 not found: /tmp/nope`; picker reads `Start failed: ...`; one process (the notice), no room |
| Kirby (encrypted) picked by mistake | notice reads `This game did not boot ... Kirby's Extra Epic Yarn ...` - the same path covers the pre-flight check |

CLI regression after the change: `--saves` on 2 players gave two lines and `EXIT=0`,
`--import 2=` gave one line and `EXIT=0`, `--saves /tmp/nope` gave the message, the notice
window and `EXIT=1`. All four Mii databases still hold their own name afterwards
(`57a4503d`, `b13bb8f6`, `8179b2ce`, `a1d0bff5`), so no save copy touched identity.
Two notes for the next session: the picker's `start_session` seeds the save sets *before*
the pre-flight boot check, so a dump that fails to boot still leaves the copy behind; and
activating the picker above a fullscreen notice sends Esc to the picker, so the notice is
closed by the next Start (or by clicking it first).

## 2026-11-14 (32) - A save set replaces the seat's saves, it does not merge

Importing used to copy over whatever was there, so a set with *less* progress inherited the
seat's stale files and a "clean" set could not reset anyone. `saves::replace_tree` now
removes the target tree first: `sdmc/Nintendo 3DS` and `nand/data/<title id>` become exactly
what the set holds, and a set without one of those trees clears the seat's copy - which is
what a clean save means. Zero files is therefore a valid import (logged `(clean)`), so the
old "no save data to import" error is gone; instead a source must look like an Azahar user
dir (`nand/` or `sdmc/`) or it is rejected as `not a save set`, which is what stops a typo
pointing at `/tmp` from wiping a seat. Identity and the Mii database still stay put, and
the picker's label now says the copy replaces that player's saves.

| check | result |
| --- | --- |
| stale file in the seat, set without it | after `--import 2=/tmp/setA`: p2's tree hash equals the set exactly (`f5e059e9`), its `STALE` file gone, the set's `KEEP` present |
| clean set (empty `nand/` and `sdmc/`) | log reads `(clean)`; p2's `sdmc/Nintendo 3DS` and `nand/data/0004000000030700` are gone; Mii still `Player 2`; 2 seats boot, 62.4 fps, `EXIT=0` |
| path that is not a save set (`/tmp`) | `not a save set (no nand/ or sdmc/): /tmp`, `EXIT=1`, nothing left running |
| window-driven grouped run after the change | MK7 2P from the picker, both seats `save set from /tmp/setA (12 files)`, p2 has `KEEP`, 62.6 fps, Stop left 0 processes and 0 rings |

One consequence worth stating: the whole `sdmc/Nintendo 3DS` tree is replaced, so other
titles' SD saves on that seat go too unless the set has them. That is the point of a set
being the seat's save card, and it is why a clean set really resets.

## 2026-11-14 (35) - Right stick as a touch pointer, per seat

A 3DS menu needs a finger, and a pad player had no way to provide one: the viewer
forwarded keys and mouse clicks only. The pad that owns the **focused tile** now
drives a touch pointer on that seat's bottom screen.

- SDL's `RIGHTX`/`RIGHTY` are polled with the pad events and integrated on a 16 ms
tick, so speed does not depend on how often the axis reports. Deflection is
normalised, a **0.2 deadzone** kills stick slack (a centred stick cannot drift),
and speed scales from the deadzone edge up to **320 px/s** - 1.25 s to cross the
400 px screen, fast enough to reach an entry, slow enough to stop on it.
- The pointer lives in that seat's frame coordinates inside the bottom screen, is
**clamped to the screen's last pixel** (verified: holding the stick pins it at
399,479 and sends nothing more once it cannot move), and **stays where it was
released** rather than recentring.
- Releasing the stick therefore needs no datagram at all: a move is a `type 4`
`TouchMoved`, which is what the seat already does while a mouse drag moves. Taps
are `type 3`/`type 5` (`TouchPressed`/`TouchReleased`) at the pointer, exactly the
mouse's pair.
- **Tap button is A.** Both shoulders already hold a peek, and F5-F8 are keys, so A
collides with nothing that exists; no pad button is forwarded to the console yet,
so A is not shadowed for gameplay either.
- Ownership is strict: only the pad bound to the focused seat steers, only the
focused seat's socket is written to, and each seat keeps its own pointer, so
moving focus never teleports a cursor. Two pads therefore cannot fight over one
pointer, and no datagram can reach another seat.
- The pointer is drawn as a small crosshair through the *same* mapping a click
uses (`from_frame` is the exact inverse of the mouse's `to_frame`), so what a
player aims at is what the tap hits. It is drawn on the focused tile only.

Verified without hands on a controller, since none is attached on this box now
(`/proc/bus/input/devices` lists no USB pad; the two entries are the virtual
ones): `tools/qa/virtual_pads.c` (uinput pads with the real 360's USB id, now
with stick and A commands) plus `tools/qa/touch_probe.py`, which binds the seat
sockets and prints every datagram with its coordinates. Stub frame rings (a valid
64-byte ring header and one slot) let the viewer open the tiles without emulators.

| check | result |
| --- | --- |
| stick right 0.6 s, down 0.3 s, focused P1 | 44 `type=4` datagrams, `(200,360)` -> `(327,456)`, ~5 px per 16 ms tick = 320 px/s, P1's socket only (`docs/evidence/stick-touch-sockets.txt`) |
| tap A | `type=3 (327,456)` then `type=5`, P1's socket only |
| hold right 1.4 s, down 1.0 s | pointer pins at `(399,479)`, the bottom screen's last pixel, and stops sending repeats |
| stick released | pointer stays at its last position; a later tap lands there |
| focused P2, P2's own pad left | 25 `type=4`, `(200,360)` -> `(71,360)`, p2's socket only, then its tap |
| focused P2, **P1's** pad stick + A | not one datagram on either socket - no cross-seat leakage |
| mouse click at the pointer's spot (top-only) | two calibration clicks solve the window->console map (0.6 frame px per window px); the stick tapped at `(363,360)` and a mouse click at the mapped window point sent `(363,360)` - identical (`docs/evidence/stick-touch-mapping.txt`) |
| live 4P MK7 session | pointer moved and tapped on P1 (`(358,424)`), P1+P2 shoulder peeks ran together, F5 peeked P1, a click on P3's tile still selected it, 64.6 fps |
| teardown | `EXIT=0`, 0 processes, 0 rings |

The crosshair is visible on the tile: `docs/evidence/stick-touch-pointer.png`.

What this does **not** do: pad buttons still do not reach the console (A taps the
touch screen and nothing else; a player still cannot drive a 3DS button with a
pad), the pointer only follows the focused seat's own pad, and pads SDL cannot
map still only peek (their axes are not read). Those are the next pad steps, not
hidden gaps - `todo.md` carries them.

## 2026-11-14 (34) - Couch reach for the peek: pads and a remembered layout

The view options of (33) worked, but only for whoever had the keyboard: F5-F8 and a
`AZAHAR_VIEW` variable are not a couch feature. Two things changed.

**Each pad owns a seat.** The viewer now opens every attached gamepad through SDL and
binds it to the lowest seat without one - the first pad is P1, the second P2, the order
players find them in - so a player reaches their *own* tile with their *own* shoulder
button. Left and right shoulder both hold the peek; releasing returns to the top screen.
Keys and pads share one `set_peek()` path, so the behaviour is identical in every layout
and a peek cannot land on another seat. The held state is input truth: the tile shows the
touch screen only while the layout is `Top`, so Tab mid-peek changes layout without
inventing or dropping a peek, and there is no "cleared while still held" desync. An
unplugged pad releases its own seat's peek (its `CONTROLLERDEVICEREMOVED` clears it), and
losing window focus drops every held peek, since a release the window never sees would
otherwise stick. F5-F8 still work for a seat without a pad.

**The layout is remembered.** The viewer writes `slots/view.txt` (`top` or `both`) every
time the layout changes and reads it at launch, so the session comes back the way the couch
left it with no flags at all. `--view both|top` still overrides for one run and
*deliberately does not persist*, so a one-off look cannot change what the next session does.

**A live test caught a real defect.** SDL queues an add event for every already-attached
device, so the four-player run registered the same pad twice: it logged `pad 0 -> P1` and
`pad 0 -> P4`, and that pad's shoulder would have peeked two tiles at once - exactly the
leak the requirement forbids. `add_pad` now ignores a device instance already open, and
the rerun logs exactly three pads for four seats (P4 legitimately has none).

Testing pads with no hands: the machine has a real `Microsoft X-Box 360 pad`, and nothing
can press it from a script. `tools/qa/virtual_pads.c` therefore creates uinput gamepads
with that pad's exact USB identity (bus 0003, vendor 045e, product 028e, version 0114), so
SDL maps them through the same game controller entry it uses for the real one - the peek
is reached as `leftshoulder`, not as a guessed button number. It needs root only to open
`/dev/uinput`, chmods nothing but the event nodes it creates, and is driven through a FIFO
(`0 ld`, `0 lu`, `0 remove`, `quit`). What that does **not** cover: a genuine unplug of
physical hardware (removal is via `UI_DEV_DESTROY`), a pad whose SDL mapping has no
shoulder (the joystick fallback path is untested on such a pad), and non-shoulder pad
buttons - those are still not forwarded to the seats at all, which is unchanged from
before this pass.

| check | result |
| --- | --- |
| 4P MK7, no flags, previous layout `top` | logs `resuming the top screen from slots/view.txt`; all four tiles open top-only (`docs/evidence/view-persisted-layout.png`) |
| two pads held at once | P2 and P3 both log a peek and both tiles show their touch screen; P1/P4 keep the action screen (`docs/evidence/view-multi-peek.png`) |
| one release | `P2 back to the top screen`, P2's tile restores while P3 keeps peeking (`docs/evidence/view-peek-release.png`) |
| peek held across both Tabs | `P2 peeking` -> `view both screens` -> `view top screen only` -> tile still shows P2's touch screen -> `P2 back to the top screen`; no clear-on-toggle desync (`docs/evidence/view-peek-across-layout.png`) |
| pad unplugged mid-peek | `P3 back to the top screen` then `P3 pad "Xbox 360 Controller" unplugged`; P3's tile is back on the top screen |
| F5 fallback | `P1 peeking at the touch screen` / `P1 back to the top screen` |
| pads registered | exactly 3 `pad` lines for 4 seats after the duplicate fix |
| layout persisted | session ended `both`, `slots/view.txt` = `both`; next 2P launch logs `resuming both screens` (`docs/evidence/view-2p-stacked.png`) and paints 59.8-62.1 fps |
| teardown | `EXIT=0`, 0 processes, 0 rings after every run |

One measurement note, because the fps number is the session's health check: this desktop got
busier while the work ran (load 2-5 from unrelated applications - Chrome, Freebuff, a Vite
dev server), and 2P runs swung between 37 and 62 fps with `paint_ms` swinging with them.
Pads were not the variable: a run with the pads gone measured the same ~45, and a
back-to-back A/B against the pre-pad build (b0f01effe) came out at 46-55 fps for the old
binary and 42-50 for this one, overlapping within the same drift. The same committed code
measured 59.8-62.1 fps in a quieter minute, which is the baseline, so the ceiling here
follows the machine rather than this change. Polling was still halved to one tick per frame
(16 ms): twice per frame bought nothing a player can feel.

The 62.5 fps figure from (33) is therefore a quiet-machine number, not a floor on a loaded
one. If a future pass needs it to hold under load, the lever is the paint path, not the
input plumbing.

Two smaller traps worth remembering. A standalone viewer started without
`QT_QPA_PLATFORM=xcb` maps a Wayland surface that `xdotool` cannot see at all - the
orchestrator sets xcb, so only hand-run viewers need it - and `xdotool key --window <id>`
is dropped by Qt, so pad/key probes need real focus (`windowactivate`) rather than a
targeted send.

## 2026-11-14 (33) - One screen per player, with a held peek at the touch screen

Most 3DS games put the action on the top screen, so the stacked pair wastes half of every
tile on a touch screen nobody is looking at. Following the sister project's flip layout
(its v0.1.8 "per-player peek flip"), the viewer now has two layouts:

- `View::Both` - today's stacked pair, the default, unchanged.
- `View::Top` - only the top screen, filling the tile (a 400x240 picture is nearly twice
  as wide as the 400x480 pair at the same height).

Tab switches between them while the session runs (taken in `event()` before Qt can route
Tab as focus navigation) and the orchestrator can open a session straight into it with
`AZAHAR_VIEW=top`. Each player holds their own key to peek: **F5/F6/F7/F8 for P1-P4**.
F-keys rather than the sister's RShift/RCtrl because Qt reports both Shift keys as the
same `Qt::Key_Shift`, so per-player shift keys are not expressible on this side; F-keys are
outside the 3DS button keymap either way, so a peek never shadows a game binding. A release
clears the peek in *every* layout, so switching layouts with a key down cannot leave a tile
stuck on the touch screen (the sister project's sticky-peek bug).

Touch follows the sister project's rule that input must not depend on what is visible: in
top-only mode a click maps onto the *bottom* half at the same relative spot, so menus stay
operable while the player watches the action screen.

| check | result |
| --- | --- |
| `--view top`, standalone | logs `view top screen only`; F5 gives `P1 peeking at the touch screen` / `P1 back to the top screen`; two Tabs give exactly two layout lines |
| 4P MK7 session with `AZAHAR_VIEW=top` | each tile's picture measures ~904 px wide against ~470 px for the stacked layout, so the layout really changed |
| F5 held | only P1 logs a peek and only P1's tile shows the touch screen; P2-P4 stay on the top screen (captured) |
| F6 held | only P2 peeks; release logs `back to the top screen` |
| teardown | `EXIT=0`, 0 processes, 0 rings |

Two things this pass ran into. `xdotool key --window <id>` is ignored by Qt (synthetic
sends are dropped), so driving peek keys needs `xdotool windowfocus <id>` and ordinary key
events; and `xwd` can catch the window mid-repaint, so the layout check was done by
measuring the picture width per tile rather than trusting a single capture. **Also found,
not fixed:** after the save-set imports all four seats show MK7's own "The save data is
corrupted and will now be reset." dialog, so the copied save set is not accepted by the
game even though the files are byte-identical across slots. That is the save path, not the
viewer, and it is the next thing to chase (candidates: the NAND save metadata/HMAC that
pairs with the SD save, or a file the copy leaves behind).

## 2026-11-15 (36) - Pad gameplay input, and forwarded keys that never reached the guest

A pad could peek but not play: the viewer forwarded touches and keys, and no pad button
touched the key path. The pads now drive their own seat's console through the forwarded-key
datagrams the keyboard already uses (`type 1`/`2` with a `Qt::Key`), so a seat needs no new
protocol and the seat-side keymap stays the one in `slots/pN/.../qt-config.ini`.

| pad input | Qt key | 3DS |
| --- | --- | --- |
| A / B / X / Y | 65 / 83 / 90 / 88 | A / B / X / Y |
| right shoulder | 87 | R |
| left trigger / right trigger | 49 / 50 | ZL / ZR |
| left stick click | 81 | L |
| Start / Select / Home | 77 / 78 / 66 | Start / Select / Home |
| d-pad | 84 / 71 / 70 / 72 | d-pad |
| left stick | 16777235/37/34/36 | circle pad (`analog_from_button` arrows) |
| right stick | - | touch pointer (focused seat only, unchanged) |
| right stick click (R3) | - | tap at the pointer |
| left shoulder (hold) | - | peek (unchanged) |

Two rebindings fall out of A becoming a game button. **Tap moved from A to R3**, because A is
the button a player mashes on a menu; and **the right shoulder is now R**, so the peek stays
on the left shoulder only (F5-F8 still peek for a seat without a pad). L sits on *left stick
click* because both shoulders and both triggers are taken.

Buttons go to **the pad's own seat**, not the focused tile - a player pressing A is pressing
their own console - while the right stick and the tap still only act on the seat the player is
looking at. The viewer tracks each pad's held buttons, axes and hat, and `release_pad` sends
the matching key-ups when a pad disappears, so unplugging mid-press cannot leave a button
stuck down in a game.

### The defect this uncovered

Forwarded keys did nothing in a game. The datagrams went out (socket probe, `docs/evidence/
pad-input-sockets.txt`) and the touchpath worked, but the guest saw no button. Cause: seats
run as **separate `azahar` processes** launched with `--input-socket`, and their profile
binds plain `engine:keyboard` (`profiles\1\button_a="code:65,engine:keyboard"`), while
`PressForwardedKey` pressed a per-window `KeyState` registered as `keyboard_<ptr>`. Nothing
in the profile ever named that state, so the presses landed in a device no HID slot read.
The per-System override that exists for in-process seats (`inproc_seats.cpp`) is not set on
this path, and its rewrite in `hid.cpp` only matches specs that *start* with
`engine:keyboard`, which the ini's `code:65,engine:keyboard` form does not.

The fix mirrors what touch already does: the first window in a process also registers its
key state under the historical name `keyboard`, and the window's own key handlers press that
same state through `PressForwardedKey`. A seat process has exactly one window, so
`engine:keyboard` now resolves to the seat's own state - including the circle pad's nested
`up/down/left/right` specs, which name the same device, so pad steering works without a
second change. Physical typing in a visible window keeps working because its events go to the
same state.

### Audio: player 1 only, by null sink

`slots.rs` now writes an `[Audio]` block per seat: seat 1 keeps `volume=1` and the default
output, seats 2-4 get `volume=0` and `output_type` set to the null sink. The value has to be
Qt's escaped QVariant `@Variant(\0\0\0\x84\0\0\0\0\0\0\0\1)`: `output_type` is read with
`value.value<u32>()`, so a plain `output_type=1` reads back as a QString and falls back to
Auto (checked by reading the ini back with a throwaway QSettings program).

| check | result |
| --- | --- |
| 4P MK7 session, `wpctl status` | exactly one `Azahar Output` stream (`output_FL/FR` > ALC257 analog); no stream for seats 2-4 |
| ini after the session | p1 `Auto (0)`, p2-p4 `Null (1)` - Azahar rewrites the file on exit with what it loaded, so the null sink was parsed, not ignored |
| pad input, socket level | P2's pad A while P1's tile is focused writes `key=65` to p2's socket only; R/L/ZL/d-pad/left stick/R3-tap/peek all as in the table; unplug after a press emits the key-up |
| pad input, in the guest (`tools/touchprobe`) | P1's tile counts `keys=5` from five pad presses, P2's stays `keys=0` (capture: `docs/evidence/pad-keys-in-guest.png`) |
| live MK7 2P | ~62 fps, `EXIT=0`, 0 processes, 0 rings |

Not done, on purpose: MK7's "Connecting to Nintendo Network" screen stays up and ignores the
pad in this session, so the in-game feel (steering, tap menus) is the manual step. Sticks are
still 8-way: `analog_from_button` turns arrow keys into a full-deflection circle pad, so a
half-tilt on the stick is not expressible yet. And "player 1's music, everyone's sound
effects" cannot be a setting - the null sink is all-or-nothing per console, so it needs core
audio work (split the HLE audio stream into music and effects, or mix the other seats'
effects in).

## 2026-11-15 (37) - A pad plays Mario Kart 7, and why the title looked stuck

The last pass proved the pad reached the guest with a homebrew counter; nobody had seen a
game move. This pass did, and the earlier "the title is stuck on Connecting to Nintendo
Network and ignores a pad Start" turned out to be **timing, not wiring**.

**The boot wait.** MK7 sits on "Connecting to Nintendo Network" for roughly 90-120 s and does
ignore input there, then continues on its own: at ~130 s one seat was already running its
attract-mode demo race. Every check before this one sampled at 30 s, so the screen looked
frozen forever. A second capture 40 s later still showed the same screen, which is what
produced the wrong diagnosis. Nothing to fix: the game is waiting on its own network init.

**The pad in the menu.** Two seats of MK7, virtual pads 0 -> P1 (left tile) and 1 -> P2, both
mapped by the viewer's own log lines. Waiting ~150 s and pressing pad 0's START put P1 on the
Mario Kart 7 main menu; from there:

| step | P1 (pad 0) | P2 (pad 1) |
| --- | --- | --- |
| menu, before anything | Single Player highlighted | still loading |
| pad 0 d-pad down | **Online Multiplayer highlighted** | Single Player highlighted |
| pad 1 d-pad down | Online Multiplayer (unchanged) | **Online Multiplayer highlighted** |

as pixel deltas over the tile crops: P1 89,284 on its own d-pad, 6,513 on the other pad's,
with an idle drift of 5,172; P2 90,241 on its own d-pad. Captures:
`docs/evidence/pad-game-mk7-{1-before,2-pad0-dpad,3-pad1-dpad}.png`.

**A second title, for a still menu.** Monster Hunter Stories at its main menu:
own pad d-pad down 51,913, the reverse press restored the highlight in 361, the other pad's
identical press 1,902 against an idle drift of 848 (`pad-game-menu-*.png`). Pressing START on
that menu faded it out into the new game (678,614), which is the "screen advancing" case.

**Audio with four seats.** One `Azahar Output` node under PipeWire (`output_FL/FR` to the
analog sink), no azahar node for seats 2-4; `p2`-`p4` still read `Null (1)` in their inis
after the session. Same as history (36), re-checked here because the mission asks for it.

**Tooling.** `tools/qa/pad_game_test.py` presses a seat's pad until its tile is still, then
measures before/after and an isolation press. Its `--wait` flag exists because of MK7's long
boot (`--wait 130`); the default 25 s is enough for a small game. Two bugs it needed: a
window from a finished session keeps its geometry and `xwd` cannot read it (skip windows
whose pid is gone), and `pgrep -f 'Release/azahar'` inside a cleanup shell matches that
shell's own command line, so it must be written `Release/azah[a]r`.

Also worth knowing before the next session: of the six ROMs in `Test Roms/`, only MK7 and
Monster Hunter Stories boot; the other four report "an encrypted or unsupported application"
despite "Decrypted" in their names. And the seat processes still create their own 1280x540
windows - unmapped, so the visible session stays one window, but they show up in `xdotool`
and must not be mistaken for the compositor window.

## 2026-11-15 (38) - The circle pad gets the stick's real position

Until now the left stick was forwarded as the four arrow keys the profile binds through
`analog_from_button`, so the console saw a full-deflection circle pad in eight directions:
a half-tilt of the stick steered like a full one. A 3DS circle pad is an axis, and games
expect the magnitude (Mario Kart's steering is the obvious one), so it now travels as its
own value.

**The message.** A seventh datagram type, `6`, using the same 12-byte struct: the two `u16`
fields hold the bits of two signed int16s, x right and y up. No size change, no extra
socket, and the seat needs nothing but the existing input socket.

**The path.** The viewer sends the raw stick position (SDL's -32768..32767, y flipped to the
input layer's "up is positive"), and only when it moved by more than ~1/64 of the travel, so
a resting stick is silent. `GRenderWindow::ApplyForwardedInput` turns it into a -1..1 pair on
`Core::System`. HID wraps the profile's circle pad in a `ForwardedCirclePad`: while the
forwarded position is off centre it wins, and while it is 0,0 the profile's own device is
read - which is why a seat with no controller still steers with the arrow keys its profile
binds, exactly as before.

**Verified in three places** (`docs/evidence/pad-analog.txt`):

- the guest, sent known values straight to a seat socket: 0.50 right reads `x=77`, 1.00
  right `x=154`, 0.71 up `y=109`, centred `0,0` - HID's own +/-0x9A range;
- the viewer, virtual pad driven with raw axis values: `8192 / 16384 / 32767 / 0,+16384` on
  P1's socket, and nothing when the stick rests;
- the whole stack with a pad: 50% reads 77, 100% reads 154, 50% up reads `y=77`, centred
  `0,0`, and P2's pad at full deflection leaves P1 at `0,0`.

`tools/touchprobe` now prints `hidCircleRead` on its top screen, which is what makes the
guest side visible at all.

**One bug worth remembering.** The axis handler had already normalised the stick to -1..1,
so the first version (which took that float as an int and compared it against a raw-scale
threshold) silently never sent anything. The socket probe printing zero datagrams is what
caught it - the viewer looked completely healthy in its own log.

**Not done:** the viewer applies no deadzone of its own. A stick that rests more than about
1.5% off centre reads as a permanent slight deflection, which games' own deadzones absorb;
if a worn controller drifts more than that, a deadzone belongs in `pad_stick`. The right
stick keeps its own deadzone for the touch pointer, which is unchanged.
## 2026-11-15 (39) - Shared audio is one console's worth, measured

"Player 1's music with everyone's sound effects" turned out to be a request the emulator
cannot answer, and the owner chose the other way. The HLE DSP mixes a console's music and
effects into one stereo stream (`FinalMixSamples`) before any sink can see them, and the four
seats are four processes with four separate mixes. Nothing downstream can take music from one
console and effects from another, so the choice is which console reaches the device - and the
decision is player 1, i.e. the policy `slots.rs` already writes (commit (36)): seat 1 keeps the
default output at `volume=1`, seats 2-4 get the null sink at `volume=0`.

That means this pass changed no code. What it changed is the evidence: until now "audio works"
meant "exactly one `Azahar Output` node exists", which is a wiring check, not a listening
check. `docs/evidence/shared-audio.txt` records the device output itself.

**The trap.** `pw-record --target <sink>.monitor` does not fail when the name does not resolve:
it records the default source instead. Every early capture in this pass was the laptop
microphone, a flat -1 dBFS noise wall whose spectrogram had no notes and no gaps - which reads
exactly like an emulator spraying garbage, and nearly sent me into the audio core. Targeting
the sink's node id and checking `pw-link -l` *during* the recording (`monitor_FL/FR ->
pw-record:input_FL/FR`) fixed it; a 330 Hz tone in the sink then measured 0.0265 peak
amplitude, where the name-targeted capture had shown nothing.

**Measured** (MK7, sink monitor, sink volume 1.0 for the test and restored to its 0.40/muted
afterwards):

| session | seats | audio streams | capture RMS | peak |
| --- | --- | --- | --- | --- |
| 2 players, boot | 2 | 1 (pid -> `slots/p1/user`) | -28.2 dBFS | 24639 |
| 2 players, attract | 2 | 1 | -14.5 dBFS | 26080 |
| 1-player request | 2 | 1 | -13.6 dBFS | 26317 |
| 4 players, attract | 4 | 1 | -13.7 dBFS | 26477 |
| 2 seats + control console | 3 | 2 | -10.2 dBFS | 32768 |

The four-seat session's node belongs to `slots/p1/user`'s pid and p2-p4 have none, while all
four seats run at 60 fps. The 1-, 2- and 4-seat captures are within 1 dB of each other: audio
from a second console would show up as +3 to +6 dB and a second node. The spectrograms
(`docs/evidence/shared-audio-{1p,2p,2p-boot,4p}.png`) show harmonics, note changes and gaps,
i.e. the game, not a stuck buffer.

**The control that makes "silence" mean something.** Copying `slots/p2/user` and deleting only
its `[Audio]` keys, then running it as a standalone console, adds a second `Azahar Output` and
raises the same capture from -14.5 to -10.2 dBFS. Seat 2's console therefore does produce
audio; the null sink is what keeps it out, and the measurement would have seen it.

**Single seat.** `players.clamp(2, 4)` means a one-player request still runs two seats, because
the layout needs two tiles. Seat 1 opens the device and seat 2 does not, and that capture is
within 1 dB of the two-seat one, so nothing that was audible before has changed.

**Not done:** the split itself needs per-console access to the DSP's sources - a game-specific
classifier, or mixing inside core audio - and is declined for now. Seats 2-4 also still run the
DSP into the null sink (only the device is avoided, not the CPU), and there is no per-seat
volume: the policy is all-or-nothing per console.
## 2026-11-16 (40) - The couch mix: every seat on one output, one music owner

The previous entry proved "player 1 only" because a real music/effects split is unreachable
at the sink. The owner chose the middle path instead: every seat feeds the one device, player
1 keeps its music at full, and seats 2-4 are high-passed and trimmed so their copy of the
same track recedes while their effects stay. This entry is that change and its measurement.

**Where the filter lives.** `DspInterface::OutputCallback` already applied the volume slider
to every sink's buffer, so the tilt goes there: `AudioCore::SetEffectsTilt` (set from the
fork flag `--effects-tilt` before emulation starts) and a 2nd-order Butterworth high-pass at
300 Hz followed by -6 dB, with per-channel state and coefficients that follow the sink's
sample rate. One implementation, every backend (SDL2, cubeb, OpenAL, null).

**Who gets it.** `slots.rs` now leaves every seat on the default output with `volume=1` (the
null-sink QVariant is gone) and `process.rs` appends `--effects-tilt` to seats 2 and up. Seat
1 has no flag, so the music owner's path is byte-for-byte what it was.

**Measured on the device** (MK7, 2 seats at 60 fps, sink monitor at 48 kHz, each seat soloed
by muting the other's stream node, sink at 0.75 as the owner left it):

| capture | RMS | peak | 60-250 Hz | 800-2500 Hz | 2500-8000 Hz | bass-mid |
| --- | --- | --- | --- | --- | --- | --- |
| mix | -13.3 dBFS | 30050 | -19.6 | -21.7 | -29.5 | +2.1 dB |
| seat 1 | -13.5 dBFS | 26326 | -20.6 | -20.3 | -27.0 | -0.3 dB |
| seat 2 | -20.7 dBFS | 15476 | -36.6 | -24.8 | -31.8 | -11.8 dB |

Seat 2 is 16 dB down where the music's body is and only 4.5 dB down in the midband - the
high-pass shape. The paired test agrees: unmuting seat 2 in six alternating windows lifts the
midband by +1.3 dB on average (theory for its -4.5 dB contribution: +1.3 dB) and the bass by
+0.2 dB. So the mix carries player 1's music and both players' effects, which is what the
policy asks for.

**Two process notes.** The spectrogram pictures are content checks only: ffmpeg normalises
its colour scale per input, so the level difference lives in the table. And the "overlapping
music" the owner heard was my own doing - a standalone control console left running beside a
two-seat session; the numbers above come from a single clean session, and everything was shut
down and the rings cleared before starting it.

**Not done:** four seats were not measured (the 2-seat mix peaked at 30050/32767, so four
consoles at this trim can clip at full volume); the trim and cutoff are constants, not a
setting; and the tilt is still an EQ guess - the other seats keep their melody and cymbals
above 300 Hz and lose engine rumble, because a real split needs per-console access inside the
DSP.
## 2026-11-16 (41) - One launch, one window, and nothing left behind

Every earlier session in this log was started from a terminal (`run.sh --start`), so nobody
had yet proved the app a user actually launches: the window, its picker, and its own quit.
This pass did that and needed no repair - it worked on the first attempt.

**The run.** All three binaries rebuilt first. Then
`DISPLAY=:1 ./src-tauri/target/release/azahar-splitscreen` with no arguments, Mario Kart 7
chosen in the Game dropdown and Players left at 2, Start clicked in the window. From that one
click the orchestrator opened the room, spawned two headless seats with their rings and
sockets, and started the viewer: `slots/session.log` lists
`area: 1920x1034`, `room: 127.0.0.1:47207`, `azsh-35570-1/2` and
`viewer: one window compositing all seats`. No second session, scratch console or `--start`
was used at any point.

**Rendering.** 140 s after Start (after MK7's ~90-120 s network boot) both seats read
`fps=60.0`, `speed≈100`, `frame_ms≈4.9`, and `docs/evidence/app-session-window.png` shows P1
and P2 side by side at the game's main menu inside that single composite window.

**Audio.** Two output streams, one per seat process, both on the default sink -
node 77 from pid 36126 (`slots/p1/user`, no flag) and node 79 from pid 36210
(`slots/p2/user`, `--effects-tilt`). The sink monitor read L -12.4 dBFS across 8.98 s of the
menu. Worth noting for later: the peak hit full scale with two consoles at the owner's 0.75
sink volume, which is the four-seat headroom question this pass was told not to chase.

**The quit.** Closing the app window (`wmctrl -ic` on the picker window) is the app's quit:
the close handler drops the session, which terminates the room, every seat and the viewer,
then exits. Eight seconds later `pgrep` found no orchestrator, room, seat, emulator or viewer;
`/dev/shm/azsh-*` was empty and PipeWire had no `Stream/Output/Audio` nodes. The shared-memory
rings are released by the same teardown.

**Observed, kept as-is:** the one sink is fed by two streams rather than one process mixing,
because each seat renders its own audio and the device sums them. (The launcher window that
sat behind the session in this run was closed in (42): the picker now hides on Start and the
session window is the quit path.)
## 2026-11-16 (42) - The picker hands over: one window owns the session

Entry (41) ended with a caveat: after Start the picker stayed open behind the composite, and
only closing the picker quit. That is a second window, and the wrong one to quit from. This
pass moved the session's ownership to the session window.

**Two changes, nothing else.** `start_session` hides the picker once the session is up, so
after Start the window list holds one `Azahar Splitscreen` entry. And
`Session::viewer_gone()` plus a watcher thread in `main.rs` (500 ms poll, because a hidden
picker leaves the event loop idle) turns the session window closing into the app quitting:
the session is dropped - room, seats and viewer terminated - then `app.exit(0)`. Escape in the
viewer already closes the window, so that is a quit as well. No audio, input, save or render
path was touched.

**The run** (all three binaries rebuilt first; launched from the repo root with no arguments;
Mario Kart 7 picked in the window, Players left at 2, Start clicked in that window):

- after Start, one window: `0x0280000d`, the composite; the picker `0x01600003` is gone from
  the window list;
- orchestrator 40771, room 40958, seats 40962/41037, viewer 41266, rings `azsh-40771-{1,2}`,
  room `127.0.0.1:33137`, `area: 1920x1034`;
- 155 s after Start both seats read `fps=60.0 speed=100.3` (frame 5.69 / 5.33 ms) and
  `app-session-window.png` shows both at MK7's menu in that one window;
- audio: node 79 from pid 40962 (`slots/p1/user`) and node 77 from pid 41037
  (`slots/p2/user`, `--effects-tilt`), both into the default sink; 8.97 s of the sink monitor
  at L/R -12.6 dBFS;
- quit: closing the session window left no orchestrator, room, seat, emulator or viewer
  process, no `/dev/shm/azsh-*`, no `Stream/Output/Audio` nodes, and only the user's own
  windows on screen.

The launcher-behind-session caveat in (41) is gone. The mix still reaches full scale with two
consoles at the owner's 0.75 sink volume; that headroom work was out of scope here.
## 2026-11-16 (43) - Six seats, one screen each, one voice

Three things changed together because each changes what the others look like: player identity
colors, a six-seat layout, and a single audio owner.

**Colored outlines, taken from mgba-splitscreen.** Its libretro core marks each player with a
3 px inset outline in a per-player color plus a matching P-number badge
(`src/platform/libretro-splitscreen/instances.c`, palette from its `styles.css`: P1 `#ff4444`,
P2 `#4488ff`, P3 `#2eaa44`, P4 `#e08a1e`). The viewer now does the same: a 3 px edge inset in
the cell's own boundary, the badge filled in the same color, and two extra colors for seats
five and six (`#9b5cff`, `#24c8db`). The outline is painted after the picture, so it holds for
any layout and for a letterboxed screen, and the boot placeholder tile uses it too instead of
a fixed red border.

**Six players.** `layout::grid` is 2x1 for two, 2x2 for three or four, 3x2 for five or six; the
clamp is 2..6 and the picker offers 2-6. Verified with six live MK7 seats in one window:
`players: 6`, `viewer grid: 3x2 cells of 639x516`, all six at `fps=60.0 speed≈100.3`
(frame 4.9-8.2 ms), load average 8.7 on 8 threads and 8 of 15 GiB in use, so six is within
this box's reach but at its edge.

**Top screen only for three or more.** A 3+ seat session opens in `View::Top` instead of
stacked screens: the tile is the action screen, and each seat's peek key - now F5-F10 so six
players each have one - reveals that seat's touch screen while held. Verified:
`viewer: no pads attached, F5-F10 peek for seats 1-6`, F6 logged `P2 peeking at the touch
screen` followed by `back to the top screen`, and `docs/evidence/six-top-only.png` is the
window with six single-screen tiles. Two seats are deliberately unchanged (both screens);
re-checked in a 2-seat run so the new default could not leak into it.

**One voice.** The effects-tilt experiment is deleted, not tuned: seats 2+ now use Azahar's
own null sink (`[Audio] output_type=1`, `SinkType::Null`), so they open no device stream at
all, and `--effects-tilt`, its argument parsing and the DSP filter are gone from the fork -
which also shrinks the fork diff. A six-seat session now opens exactly one audio stream, from
`slots/p1` (node 91, pid 55137); the 2-seat run also opened one. Overlapping music is what
the earlier mix tried to solve and what a single owner removes outright.

**Quit stayed clean:** closing the session window after the six-seat run left no orchestrator,
room, seat, emulator or viewer process, no `/dev/shm/azsh-*` and no audio streams.

Question answered along the way: save states are not a feature of this app (it manages save
*data* sets), and gamepad input is working but only for pads that are attached - the picker
reported "Gamepads: none detected" in these runs.
## 2026-11-16 (44) - Four test dumps that never booted, deleted

`Test Roms/` held six dumps and only two had ever booted. Rather than trust that note, each
dump was run once more before anything was deleted: one Azahar at a time, its own user dir, the
fork's unmapped-window path (`--shm-frame`), 25 seconds, then SIGKILL. (A plain `timeout` is not
enough - a standalone instance ignores SIGTERM and the first attempt sat on the first dump
until the tool timed out; the SIGKILL run finished six in two and a half minutes.)

| dump | verdict |
| --- | --- |
| Kirby's Extra Epic Yarn (USA) Decrypted | `Failed to determine system mode` - deleted (2.0 GB) |
| Zelda - Tri Force Heroes (USA) Decrypted | `Failed to determine system mode` - deleted (1.0 GB) |
| Mario Party - The Top 100 (USA) Decrypted | `Failed to determine system mode` - deleted (0.5 GB) |
| New Super Mario Bros. 2 (USA) Decrypted | `Failed to determine system mode` - deleted (0.5 GB) |
| Mario Kart 7 (Europe) | boots: 24 fps lines in the stats file - kept (1.0 GB) |
| Monster Hunter Stories (Europe) | boots: 24 fps lines in the stats file - kept (2.0 GB) |

About 4 GB was freed; `Test Roms/` now holds the two dumps that run. The folder is gitignored,
so the tree change is the todo note: the QA gap is now "only MK7 and MHS can be boot-tested"
rather than a count of broken dumps. The rings the SIGKILLed runs left in `/dev/shm` were
removed by hand.
## 2026-11-16 (45) - The docs say what the app is now

A documentation pass, plus the two code changes it produced.

**Per-seat ROMs: built, then removed.** To put both booting dumps in one window the
orchestrator briefly grew `--seat-rom <seat>=<rom>` (resolved per seat in `process::start`),
and a mixed MK7 + Monster Hunter Stories session started cleanly under it: one window, two
rings, both seats spawned with their own dump. The owner rejected the capability outright -
every seat in a session plays the same game - so it was deleted instead of kept as a hidden
QA flag. No `seat-rom` reference remains.

**`--start` quits like the app.** `run_headless` now also stops when the session window closes
(`Session::viewer_gone()`), instead of only on Ctrl-C, so "close the window" is the quit path
in both entry points. Verified on the mixed run before it was abandoned: closing the viewer
took the room, both seats and the viewer with it - no processes, no rings, no streams.

**Documentation rewritten against the shipped app.**

- `README.md` is the fork's own front door: what it is, why it exists next to upstream (with a
  comparison table), the process tree, controls, build/run, and a status section that states
  the limits plainly (two bootable dumps, no save states, per-seat ROMs unsupported, 4-6 seat
  audio headroom open).
- `docs/one-window-flow.md` (new) describes the shipped flow end to end: process tree,
  lifecycle from picker to quit, the ring format, layout and peek rules, the single audio
  owner, teardown guarantees, and how to reproduce and check a run.
- `AGENTS.md`: project description, architecture (one window, not tiled windows), current
  state, build/run (a headless seat plus the viewer instead of per-seat geometry flags) and
  the gotchas (seats must stay headless, no per-window capture on this compositor, one audio
  owner, SIGKILL on hang) now match reality.
- `plans/single-window-shm.md` status is "shipped", with the old presented-fps open question
  closed by the 62 fps measurements.
- `todo.md` gains a "where this is now" header; the effects-tilt and P1-only audio entries
  record the supersession, the badge entry covers `P1`-`P6`, and the "one hotkey to exit"
  item is closed by the window-close quit.
- `docs/evidence/effects-tilt.txt` carries a superseded banner so it is not read as current
  behaviour.

`cargo build --release` and `cargo test --release` pass on the final tree.
## 2026-11-16 (46) - Loose ends audited, beta bundles built

**The audit** (controllers, save states, usability), checked against the code rather than the
notes:

- **Controllers** are real and verified: pads are opened through SDL, each pad owns a seat,
  buttons go through forwarded keys, sticks through the type-6 datagram, peek on the left
  shoulder, and the picker lists what the kernel exposes (history (36)-(38)). The open ends are
  product, not bugs: assignment is by connection order, there is no remap UI, and a replug can
  land on a different seat. Recorded in `todo.md` for a 1.0 rather than rushed into the beta.
- **Save states** were never implemented and nothing claims otherwise: the app manages save
  *data* sets (keep / grouped / per-seat import) and the seats are headless, so Azahar's own
  save-state hotkeys are not forwarded. Added as an explicit todo line so the gap is stated
  once, not rediscovered.
- **Usability** found one real hole: if `tools/viewer/build/azahar-viewer` was missing, the
  start succeeded, the picker hid, and the player was left with no window at all. `start()` now
  fails that start, tears the seats and room down and says to build the viewer.
- **Platform** is the other limit: the frame ring is `shm_open`/`mmap` and the orchestrator uses
  `setsid`, `kill` and Unix sockets, so there is no Windows build without a port. Recorded in
  `README.md` and `todo.md` instead of shipping an artifact that cannot run a session.

**Beta bundles.** `packaging/pack.sh` assembles a runnable tree - emulator, `azahar-room`, the
viewer, the orchestrator in the paths the app resolves, an empty `Test Roms/`, a `BETA.txt`
and a `run.sh` that pins `AZAHAR_SPLITS_ROOT` so the bundle runs from anywhere. Verified by
unpacking it outside the repo: the picker listed the symlinked dumps, Start opened one window,
both seats held `fps=60.0 speed≈100.3` with the viewer at `presented_fps=62.5`, and closing the
window left nothing behind (0 windows, 0 rings).

**Release CI.** `.github/workflows/release.yml` builds on tags: `emulator-linux` in upstream's
build container, `app-linux` on ubuntu-24.04 with the viewer and Tauri dependencies, and
`app-macos` on an Intel runner with `CMAKE_OSX_ARCHITECTURES=x86_64` plus the
`x86_64-apple-darwin` Rust target - the Intel flag, stated where it belongs. Both bundles are
attached to a pre-release by the `release` job. Only Linux x86_64 and macOS x86_64 artifacts are
produced, for the platform reason above.
