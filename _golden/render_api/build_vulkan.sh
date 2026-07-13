#!/usr/bin/env bash
# Build the Vulkan render-API harness against the in-tree libmpv build.
#   build_vulkan.sh   -> builds rapi_harness_vulkan[.exe]
# Needs libplacebo + vulkan dev headers. Portable across the WSL fork (lavapipe,
# build/ + libmpv.so) and the Windows MSYS2 UCRT64 worktree (real GPU, bld/ +
# libmpv.dll.a). Run from the matching shell (UCRT64 bash on Windows).
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if [ -f "$ROOT/bld/libmpv.dll.a" ]; then
  # Windows MSYS2: import lib + .exe, DLLs resolved via PATH at run time.
  BUILD="$ROOT/bld"
  OUT="$HERE/rapi_harness_vulkan.exe"
  cc -O2 -g -Wall -o "$OUT" "$HERE/rapi_harness_vulkan.c" \
     -I"$ROOT/include" $(pkg-config --cflags libplacebo vulkan) \
     -L"$BUILD" -lmpv $(pkg-config --libs libplacebo vulkan)
elif [ -f "$ROOT/build/libmpv.so" ]; then
  # Linux/WSL: shared object + rpath.
  BUILD="$ROOT/build"
  OUT="$HERE/rapi_harness_vulkan"
  cc -O2 -g -Wall -o "$OUT" "$HERE/rapi_harness_vulkan.c" \
     -I"$ROOT/include" $(pkg-config --cflags libplacebo vulkan) \
     -L"$BUILD" -lmpv $(pkg-config --libs libplacebo vulkan) \
     -Wl,-rpath,"$BUILD"
else
  echo "no libmpv build found (looked for bld/libmpv.dll.a and build/libmpv.so)"
  exit 1
fi

echo "built $OUT (links $BUILD libmpv)"
