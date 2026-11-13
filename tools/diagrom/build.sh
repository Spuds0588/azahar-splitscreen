#!/usr/bin/env bash
# Build azahar-linktest.3dsx with the devkitPro docker image (no system devkitARM needed).
#
#   tools/diagrom/build.sh          # build, output in tools/diagrom/out/
#   tools/diagrom/build.sh clean    # remove build output
#
# The image is ~2 GB on disk. If the box is short on space, run `docker rmi
# devkitpro/devkitarm:latest` afterwards — the source rebuilds in a minute.
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
    sh -c 'make -j"$(nproc)" && cp -v azahar-linktest.3dsx out/' \
    && rm -rf build azahar-linktest.3dsx azahar-linktest.elf azahar-linktest.smdh azahar-linktest.map

ls -l out/azahar-linktest.3dsx
