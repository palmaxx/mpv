#!/usr/bin/env bash
# Build the D3D11 render-API harness against the in-tree libmpv build.
#   build_d3d11.sh            -> builds _golden/render_api/rapi_harness_d3d11.exe
#
# Run from an MSYS2 UCRT64 shell (the only environment where pl_has_d3d11=1).
# Links against the worktree's ninja build dir (bld) import lib + d3d11.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${MPV_BUILD:-$ROOT/bld}"

[ -f "$BUILD/libmpv.dll.a" ] || { echo "no libmpv.dll.a in $BUILD -- build mpv first (ninja -C bld)"; exit 1; }

cc -O2 -g -Wall -o "$HERE/rapi_harness_d3d11.exe" "$HERE/rapi_harness_d3d11.c" \
   -I"$ROOT/include" \
   -L"$BUILD" -lmpv -ld3d11

echo "built $HERE/rapi_harness_d3d11.exe (links $BUILD/libmpv-2.dll)"
