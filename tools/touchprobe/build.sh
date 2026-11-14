#!/usr/bin/env bash
# Build azahar-touchprobe.3dsx with the devkitPro docker image (no devkitARM install).
#
#   tools/touchprobe/build.sh          # build, output in tools/touchprobe/out/
#   tools/touchprobe/build.sh clean    # remove build output
set -euo pipefail

cd "$(dirname "$0")"

IMAGE="${DEVKITPRO_IMAGE:-devkitpro/devkitarm:latest}"

if [ "${1:-}" = "clean" ]; then
    rm -rf build out ./*.elf ./*.3dsx ./*.smdh
    echo "cleaned"
    exit 0
fi

mkdir -p out
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$PWD:/work" \
    -w /work \
    "$IMAGE" \
    sh -c 'make -j"$(nproc)" && cp -v touchprobe.3dsx out/' \
    && rm -rf build touchprobe.3dsx touchprobe.elf touchprobe.smdh touchprobe.map

ls -l out/touchprobe.3dsx
