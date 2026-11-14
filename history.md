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
