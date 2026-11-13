#!/usr/bin/env bash
# Launch azahar-splitscreen for a testing session.
#
#   ./run.sh                                  # QA sampler + the orchestrator window
#   ./run.sh --start "<rom>" --players 4      # session from a terminal, no picker
#
# Pick a game and a player count in the window and press Start; the seats render
# headless into shared memory and one window (tools/viewer) composites them, so
# the session is a single window. The sampler records fps/CPU/RSS per seat and
# for the viewer to slots/monitor.log; `tools/qa/monitor.sh report` summarises it.
set -u
cd "$(dirname "$0")"

orchestrator=src-tauri/target/release/azahar-splitscreen
if [ ! -x "$orchestrator" ]; then
    echo "not built yet — run:" >&2
    echo "  ninja -C build bin/Release/azahar" >&2
    echo "  (cd src-tauri && cargo build --release)" >&2
    exit 1
fi

if [ ! -x tools/viewer/build/azahar-viewer ]; then
    echo "note: the single-window viewer is not built, the seats would be headless:" >&2
    echo "  cmake -S tools/viewer -B tools/viewer/build -G Ninja && ninja -C tools/viewer/build" >&2
fi

./tools/qa/monitor.sh start >/dev/null 2>&1 || true
exec "$orchestrator" "$@"
