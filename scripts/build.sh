#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
node tools/component-versions.mjs generate
image=${BRICKOS_TOOLCHAIN_IMAGE:-brickos-trimui-arm64}
mount=$(pwd)
if command -v cygpath >/dev/null 2>&1; then mount=$(cygpath -m "$mount"); fi
docker build -t "$image" toolchain
MSYS_NO_PATHCONV=1 docker run --rm -v "$mount:/workspace" -w /workspace "$image" sh toolchain/build-container.sh
