# tools/diagrom — azahar-linktest

A tiny 3DS homebrew (`.3dsx`) that tests the **local-wireless link** without a
game: it creates or joins an emulated UDS network, broadcasts heartbeats,
echoes peers' heartbeats back, and reports what arrives. It is the 3DS
counterpart of mgba-splitscreen's `linktest` ROM: rule the emulator/link layer
in or out before blaming a game.

No game ROM, BIOS, keys or NAND are involved. Azahar loads `.3dsx` files
directly, so this is the whole test payload.

## Build

```bash
tools/diagrom/build.sh          # docker (devkitpro/devkitarm), output in out/
tools/diagrom/build.sh clean    # remove build/ + out/
```

- Output: `tools/diagrom/out/azahar-linktest.3dsx` (~154 KB, plus an `.smdh`).
- The image is ~2 GB on disk and 2.09 GB extracted; `docker rmi
  devkitpro/devkitarm:latest` afterwards if space is tight. Rebuilding only
  needs the image again.
- Nothing in `out/` or `build/` is tracked.

## Run the test

1. Start the Azahar instances, **each with its own user directory** (a `user/`
   folder in the process's cwd is picked up automatically — see `AGENTS.md`).
   Two instances sharing one user dir get the same console identity and the
   same MAC, which defeats the room.
2. **Give each slot a console nickname** before joining a room. Direct
   Connect's nickname is the emulated console username (CFG block `0xA0000`)
   and a fresh user dir has it empty, so Connect fails validation:

   ```bash
   # after one boot has created user/nand:
   tools/diagrom/set-slot-name.py /tmp/slot1 Player1
   tools/diagrom/set-slot-name.py /tmp/slot2 Player2
   ```

3. Start a **fresh** room server per session. It needs `--preferred-app`; the
   max member flag is spelled with an underscore:

   ```bash
   build/bin/Release/azahar-room --room-name linktest \
     --preferred-app "Azahar Linktest" --port 24872 --max_members 4
   ```

4. Join every instance to the **same Azahar room** (Multiplayer → Direct
   Connect to `127.0.0.1`, port `24872`). The UDS layer rides on the room's
   Wi-Fi packet relay; with no room joined, beacons have nowhere to go. The
   dialog stays open after a successful join — leave it open (closing it from
   the window manager kills the app's keyboard input).
5. Load `azahar-linktest.3dsx` in every instance (File → Load File; `.3dsx` is
   a supported format).
6. Move the windows apart before sending keys. Two instances spawn on top of
   each other, so a click aimed at one lands on the other:

   ```bash
   wmctrl -i -r <window-id> -e 0,0,0,-1,-1      # instance 1 at the top left
   wmctrl -i -r <window-id> -e 0,0,540,-1,-1    # instance 2 below it
   ```

7. On **one** instance press **A** (host). On the others press **B** (join).
   Joiners scan for beacons for ~5 s, so start the host first.
8. Read the screen. **START** quits.

### What the screen means

| Line | Meaning |
|---|---|
| `uds: host / client` | UDS connection state from `udsGetConnectionStatus` |
| `my node N`, `nodes k/m`, `mask 0x....` | NetworkNodeID and the node bitmask (bit 0 = host) |
| `tx … rx …` | Heartbeats sent and packets received, with a 1 s rate |
| `peers: n2:41 n3:38` | Packets received per source node — this is the link proof |
| `rtt: 12 ms (min …, max …)` | Heartbeat → echo round trip through the relay |
| `last: node N seq S kind K` | The last payload actually parsed |
| `LINK ACTIVE - N PS` | Two or more nodes are on the network |
| `STALL - no packets for N ms` | Peers were present but traffic stopped |
| `last error: 0x…` | Last failing result code (0 = none) |
| `host failed` / `join failed` | `udsCreateNetwork` / scan+connect failed; code printed |

Success is `LINK ACTIVE - 2 PS` (or more) on every instance with `peers`
counters climbing and a sane RTT. That means the room relay, the UDS service
and the per-slot identity are all working — any game that then fails to link
is a game-level issue, not the transport.

## What Azahar emulates (verified by reading `src/`)

`nwm::UDS` (`src/core/hle/service/nwm/nwm_uds.cpp`) implements the commands
libctru uses: `InitializeWithVersion`, `BeginHostingNetwork`,
`ConnectToNetwork`, `Bind`/`Unbind`, `SendTo`, `PullPacket`,
`GetConnectionStatus`, `GetNodeInformation`, `RecvBeaconBroadcastData`,
`DecryptBeaconData`, `SetApplicationData`/`GetApplicationData`,
`DestroyNetwork`, `DisconnectNetwork`, `EjectClient`.

Two findings that matter for homebrew:

- **`SetProbeResponseParam` (0x21) is unimplemented**, but the HLE framework
  answers unimplemented functions with **success** (`service.cpp:155`,
  "Hack - ignore error"), so libctru's `udsCreateNetwork` still works. The
  emulator log prints `unknown / unimplemented function '...0x210080'` once per
  host start — that line is expected, not the failure.
- **`nwm::UDS` relays through the room, not through the OS.** Packets are
  `Network::WifiPacket`s sent via the room member's `SendWifiPacket`; without a
  joined room there is no peer to hear the beacon. Console MAC comes from the
  room member when connected, otherwise the console's own identity — another
  reason each slot needs its own user dir.

## Verified: two instances, one room (2026-11-13)

Built `citra_meta` + `citra_room_standalone` from this tree, ran two instances
with separate user dirs and joined both to a loopback room. Host reported
`state=6 node=1 nodes=2/4 mask=0x0003 peers=0x04 tx=470 rx=723`; the joiner
reported `state=9 node=2` with the same mask and `peers=0x02`, tx/rx climbing at
≈18 packets/s each way, RTT 32 ms typical (16–69 ms min/max over the session).
Both screens showed `LINK ACTIVE — 2 PS`.

Caveat on the RTT: the ROM samples on its 100 ms heartbeat grid and the echo
rides the same relay tick, so 32 ms is quantisation, not transport latency.
Use it to prove traffic flows, not as a latency budget.

## Machine-readable output

Every line is also appended to `sdmc:/linktest.log` in the instance's user dir
(`<slot>/user/sdmc/linktest.log`) because `svcOutputDebugString` output never
reached Azahar's log at any `log_filter` setting:

```
LT boot wlancomm=5a5a0001 max_nodes=4
LT hosting wlancomm=5a5a0001 max_nodes=4
LT role=host state=6 node=1 nodes=2/4 mask=0003 peers=04 tx=470 rx=723 rtt=33/16/69 link=active
LT role=join state=9 node=2 nodes=2/4 mask=0003 peers=02 tx=363 rx=725 rtt=32/16/65 link=active
```

`rtt=current/min/max`; `link=` is `waiting` or `active`. The file is truncated
at boot, so one run per slot dir.

## Notes

- Console-only (top screen, `consoleInit`); no graphics, no audio. Writes one
  file, `sdmc:/linktest.log` (see above).
- `WLANCOMM_ID` is fixed at `0x5A5A0001`; change it only if it collides with a
  game you are also testing.
- 60 Hz loop, heartbeat every 100 ms, 32-bit `osGetTime()` deltas.
- The screen is the top display: if a game/slot is configured to show the
  bottom screen, swap screens first (that is exactly what the planned
  screen-swap hotkey is for).
