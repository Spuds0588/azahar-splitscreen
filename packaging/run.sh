#!/bin/sh
# Beta launcher. The app finds the emulator, the viewer and the picker assets
# relative to AZAHAR_SPLITS_ROOT, so the bundle can live anywhere.
here=$(cd "$(dirname "$0")" && pwd) || exit 1
cd "$here" || exit 1
export AZAHAR_SPLITS_ROOT="$here"
exec ./build/bin/Release/azahar-splitscreen "$@"
