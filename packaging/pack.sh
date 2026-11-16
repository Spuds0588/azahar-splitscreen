#!/bin/bash
# Packs a runnable beta bundle: the emulator, the room, the one-window viewer and
# the orchestrator in the layout the app resolves its paths from, plus a launcher
# that pins AZAHAR_SPLITS_ROOT so the bundle runs from anywhere. The picker's DOM
# is embedded in the orchestrator at build time (Tauri frontendDist), so no dist/
# copy is needed at run time.
set -euo pipefail

os=${1:?usage: pack.sh <linux|macos> <version>}
version=${2:-dev}
name="azahar-splitscreen-${version}-${os}-x86_64"
root="$(cd "$(dirname "$0")/.." && pwd)"
cargo_target=${CARGO_TARGET:-src-tauri/target/release}

cd "$root"
# Where the emulator lands depends on the platform: build/bin/Release on a
# Ninja build, and inside azahar.app on macOS, where upstream builds an app
# bundle. The bundle always uses the Release layout, because that is what the
# orchestrator resolves at run time.
bin_dir=build/bin/Release
[ -d "$bin_dir" ] || bin_dir=build/bin
emulator="$bin_dir/azahar"
[ -x "$emulator" ] || emulator="$bin_dir/azahar.app/Contents/MacOS/azahar"
room="$bin_dir/azahar-room"
if [ ! -x "$emulator" ] || [ ! -x "$room" ]; then
    echo "pack: azahar ($emulator) or azahar-room ($room) is missing; build first" >&2
    exit 1
fi
out="out/$name"
rm -rf "$out"
mkdir -p "$out/build/bin/Release" "$out/tools/viewer/build" "$out/Test Roms"

cp "$emulator" "$room" "$out/build/bin/Release/"
cp "$cargo_target/azahar-splitscreen" "$out/build/bin/Release/"
cp tools/viewer/build/azahar-viewer "$out/tools/viewer/build/"
cp packaging/run.sh packaging/BETA.txt "$out/"
chmod +x "$out/run.sh" \
         "$out/build/bin/Release/azahar" \
         "$out/build/bin/Release/azahar-room" \
         "$out/build/bin/Release/azahar-splitscreen" \
         "$out/tools/viewer/build/azahar-viewer"

tar -C out -czf "out/$name.tar.gz" "$name"
echo "packed out/$name.tar.gz"
