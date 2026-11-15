#!/usr/bin/env python3
"""QA: prove a real game reacts to a pad press on that pad's own seat.

The seats play the same opening cutscene at their own pace, so a fixed sleep
cannot land on a still screen. This harness instead drives one seat's pad until
its tile stops changing, captures that frame, presses the pad, and captures
again; then it presses the *other* pad and checks the first tile again. Pixel
deltas are measured on the tile crops, so "unchanged" is a number.

    python3 tools/qa/pad_game_test.py "<rom>" --seat 1

`--wait` is the boot budget before settling starts: 25 s is enough for a small
game, while Mario Kart 7 spends ~2 minutes on its "Connecting to Nintendo
Network" screen and ignores input there, so it needs `--wait 130`.
"""

import argparse
import os
import subprocess
import sys
import time

from PIL import Image, ImageChops

CTL = "/tmp/pads.ctl"
TMP = "/tmp/padgame"


def sh(*args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, **kwargs)


def viewer_window(timeout=180):
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = sh("xdotool", "search", "--name", "Splitscreen").stdout.split()
        for wid in out:
            # A window from a session that already ended stays in the tree with its
            # old geometry, and xwd cannot read it; skip anything without a live pid.
            pid = sh("xdotool", "getwindowpid", wid).stdout.strip()
            if not pid or not os.path.isdir(f"/proc/{pid}"):
                continue
            geo = sh("xdotool", "getwindowgeometry", wid).stdout
            if "Geometry:" not in geo:
                continue
            size = geo.split("Geometry:")[1].split()[0]
            try:
                w, h = (int(v) for v in size.split("x"))
            except ValueError:
                continue
            if w >= 800 and h >= 400:
                return wid
        time.sleep(2)
    return None


def capture(wid, name):
    xwd = os.path.join(TMP, name + ".xwd")
    pnm = os.path.join(TMP, name + ".pnm")
    sh("xwd", "-id", wid, "-out", xwd)
    if not os.path.exists(xwd):
        return None
    with open(pnm, "wb") as handle:
        subprocess.run(["xwdtopnm", xwd], stdout=handle)
    try:
        return Image.open(pnm).convert("RGB")
    except Exception:
        return None


def tile(frame, index, count):
    if frame is None:
        return None
    w, h = frame.size
    return frame.crop((w * index // count, 0, w * (index + 1) // count, h))


def delta(first, second):
    if first is None or second is None:
        return -1
    diff = ImageChops.difference(first, second).convert("L")
    return sum(1 for value in diff.getdata() if value > 24)


def press(pad, button, hold=0.35):
    with open(CTL, "w") as handle:
        handle.write(f"{pad} {button} 1\n")
        handle.flush()
    time.sleep(hold)
    with open(CTL, "w") as handle:
        handle.write(f"{pad} {button} 0\n")
        handle.flush()
    time.sleep(0.25)


def hat(pad, axis, value, hold=0.4):
    with open(CTL, "w") as handle:
        handle.write(f"{pad} {axis} {value}\n")
        handle.flush()
    time.sleep(hold)
    with open(CTL, "w") as handle:
        handle.write(f"{pad} {axis} 0\n")
        handle.flush()
    time.sleep(0.25)


def settle(wid, seat, count, presses=8, noise=4000):
    """Press this seat's A until its tile is still between two captures."""
    for attempt in range(presses + 1):
        first = tile(capture(wid, f"s{attempt}a"), seat, count)
        time.sleep(2.2)
        second = tile(capture(wid, f"s{attempt}b"), seat, count)
        moved = delta(first, second)
        print(f"  settle {attempt}: delta {moved}")
        if 0 <= moved < noise:
            return second
        press(seat, "a")
    return second


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("rom")
    parser.add_argument("--seat", type=int, default=1, help="tile to drive (0 = P1)")
    parser.add_argument("--players", type=int, default=2)
    parser.add_argument("--wait", type=int, default=25, help="seconds of boot before settling")
    args = parser.parse_args()

    os.makedirs(TMP, exist_ok=True)
    env = dict(os.environ, DISPLAY=":1")
    orch = subprocess.Popen(["./run.sh", "--start", args.rom, "--players", str(args.players)],
                            stdout=open("/tmp/padgame.log", "w"), stderr=subprocess.STDOUT,
                            env=env)
    try:
        wid = viewer_window()
        if wid is None:
            print("no viewer window")
            return 1
        print(f"window {wid}")
        time.sleep(args.wait)

        still = settle(wid, args.seat, args.players)
        if still is None:
            print("no capture while settling")
            return 1
        still.save(os.path.join(TMP, "before.png"))
        other = 1 - args.seat

        # Noise floor: the same wait with no input at all.
        quiet = tile(capture(wid, "quiet"), args.seat, args.players)
        noise = delta(still, quiet)
        print(f"idle drift (no input, same wait): {noise}")

        # The press under test: d-pad down on the seat's own pad, then back up.
        hat(args.seat, "hy", 1)
        down = tile(capture(wid, "sel_down"), args.seat, args.players)
        own_down = delta(still, down)
        print(f"own pad d-pad down: {own_down} changed pixels")
        hat(args.seat, "hy", -1)
        back = tile(capture(wid, "sel_back"), args.seat, args.players)
        restored = delta(still, back)
        print(f"own pad d-pad up (back to start): {restored} changed pixels")

        # Isolation: the other pad, same input, must not move this seat's screen.
        hat(other, "hy", 1)
        cross = tile(capture(wid, "cross"), args.seat, args.players)
        crossed = delta(back, cross)
        print(f"other pad d-pad down: {crossed} changed pixels")

        # And the visible advance off this screen.
        hat(args.seat, "hy", -1)
        time.sleep(1.0)
        press(args.seat, "start")
        time.sleep(1.2)
        moved = tile(capture(wid, "after_start"), args.seat, args.players)
        advanced = delta(back, moved)
        print(f"own pad START: {advanced} changed pixels")

        down.save(os.path.join(TMP, "selection_moved.png"))
        moved.save(os.path.join(TMP, "advanced.png"))
        print(f"RESULT own_dpad={own_down} restored={restored} idle={noise} other={crossed} "
              f"start={advanced}")
        return 0
    finally:
        orch.terminate()
        for _ in range(20):
            if orch.poll() is not None:
                break
            time.sleep(0.5)
        if orch.poll() is None:
            orch.kill()
        # The bracket keeps pgrep from matching this very command line, which would
        # kill the cleanup shell before it kills anything else.
        sh("bash", "-c", "for pid in $(pgrep -f 'Release/azah[a]r'); do kill -9 $pid; done")


if __name__ == "__main__":
    sys.exit(main())
