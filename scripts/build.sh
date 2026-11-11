#!/bin/sh
# Build OpenJKDF2 natively for this Mac (Apple Silicon, arm64).
# Output: OpenJKDF2.app at the repo root. Build files live in build_darwin64/.
#
# Usage: scripts/build.sh          incremental build
#        scripts/build.sh clean    wipe build_darwin64 first
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ "$1" = "clean" ]; then
    rm -rf build_darwin64 OpenJKDF2.app
fi

# Python build helpers (generate-iconset for the app icon) live in a private
# venv inside the repo so the system/conda Python is never touched.
VENV="$ROOT/build_openjkdf2_venv"
if [ ! -x "$VENV/bin/generate-iconset" ]; then
    echo "Setting up build helper venv..."
    python3 -m venv "$VENV"
    "$VENV/bin/python3" -m pip install --quiet --disable-pip-version-check cogapp generate-iconset
fi
export PATH="$VENV/bin:$PATH"

export OPENJKDF2_RELEASE_COMMIT="$(git log -1 --format=%H)"
export OPENJKDF2_RELEASE_COMMIT_SHORT="$(git rev-parse --short=8 HEAD)"
export OPENJKDF2_NO_ASAN=1

mkdir -p build_darwin64
cd build_darwin64
cmake -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release ..
make -j"$(sysctl -n hw.ncpu)"

echo "Build done."
