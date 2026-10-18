#!/bin/sh
# Builds every component in the Docker toolchain, then writes the release package to dist/
# and prints its path and build identity. When the build or a check fails, it exits with an
# error and writes no package.
set -eu
cd "$(dirname "$0")/.."
if ! sh scripts/build.sh; then
    echo "release: the build failed; no package was written" >&2
    exit 1
fi
node tools/release-package.mjs --out dist "$@"
