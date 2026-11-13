#!/usr/bin/env bash
# Launch azahar-splitscreen for a testing session.
#
#   ./run.sh            # QA sampler in the background + the orchestrator window
#
# Pick a game and a player count in the window and press Start; the four seats
# open inside one window (tools/quadview), and the sampler records fps/CPU/RSS
# per seat to slots/monitor.log. `tools/qa/monitor.sh report` summarises it.
set -u
cd "$(dirname "$0")"

orchestrator=src-tauri/target/release/azahar-splitscreen
if [ ! -x "$orchestrator" ]; then
    echo "not built yet — run:" >&2
    echo "  ninja -C build bin/Release/azahar" >&2
    echo "  (cd src-tauri && cargo build --release)" >&2
    exit 1
fi

if [ ! -x tools/quadview/build/azahar-quadview ]; then
    echo "note: single-window container not built, seats will open as four windows:" >&2
    echo "  cmake -S tools/quadview -B tools/quadview/build -G Ninja && ninja -C tools/quadview/build" >&2
fi

./tools/qa/monitor.sh start >/dev/null 2>&1 || true
exec "$orchestrator"
