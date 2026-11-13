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
