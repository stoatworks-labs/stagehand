#!/bin/sh
#
# Everything. No external dependency: the suite runs against the synthetic
# source built from this repo.
#
#   tools/verify.sh

set -e

ROOT=$( cd "$( dirname "$0" )/.." && pwd )
BUILD="$ROOT/build-verify"

say() { printf '\n=== %s ===\n' "$1"; }

say "configure"
cmake -S "$ROOT" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      > "$BUILD.log" 2>&1 || { tail -30 "$BUILD.log"; exit 1; }

say "build"
cmake --build "$BUILD" -j8 >> "$BUILD.log" 2>&1 || { tail -40 "$BUILD.log"; exit 1; }
echo "ok"

say "the synthetic source exports exactly one symbol"
# Hidden visibility except the entry point: a source's internals have no
# business in the host's symbol namespace.
EXPORTS=$( nm -gU "$BUILD/libstagehand_testsource.dylib" 2>/dev/null \
           | grep -c "stagehand_source_api" || true )
if [ "$EXPORTS" -ne 1 ]; then
    echo "FAIL: expected one exported entry point, found $EXPORTS"
    exit 1
fi
echo "ok"

say "stagetest"
"$BUILD/stagetest" | grep -E "^  (ok|FAIL)|checks,"
"$BUILD/stagetest" > /dev/null 2>&1 || exit 1

say "all passed"
