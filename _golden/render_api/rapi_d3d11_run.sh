#!/usr/bin/env bash
# Self-baselined D3D11 render-API golden gate for the gpu_next render backend.
#
#   rapi_d3d11_run.sh              capture the matrix into cand_d3d11/, diff vs baseline_d3d11/
#   rapi_d3d11_run.sh --baseline   capture the matrix into baseline_d3d11/ (re-baseline)
#
# Drives rapi_harness_d3d11 (libmpv render API -> MPV_RENDER_API_TYPE_PL_D3D11
# -> render_backend_gpu_next -> wrapped ID3D11Texture2D), reads each frame back
# as raw RGBA8 and compares sha256. The host is a WARP software D3D11 device,
# deterministic run-to-run on one machine, so this gate SELF-baselines: capture
# once on a known-good build, re-verify byte-identical afterwards. This is the
# render-API analogue of the WSL llvmpipe pl-opengl gate, on the Windows rig.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
HARNESS="$HERE/rapi_harness_d3d11.exe"

# Clips: prefer the worktree's own _golden/clips, else the sibling mpv-src clone.
CLIPS="${CLIPS:-$ROOT/_golden/clips}"
[ -d "$CLIPS" ] || CLIPS="/c/DEV/ai-dev/projects/mpv-src/_golden/clips"

[ -x "$HARNESS" ] || { echo "no rapi_harness_d3d11.exe -- run build_d3d11.sh first"; exit 1; }
[ -d "$CLIPS" ]   || { echo "no clips dir ($CLIPS)"; exit 1; }

# Ensure libmpv-2.dll is found at runtime.
export PATH="${MPV_BUILD:-$ROOT/bld}:$PATH"

MODE="${1:-verify}"
if [ "$MODE" = "--baseline" ]; then
  OUT="$HERE/baseline_d3d11"
else
  OUT="$HERE/cand_d3d11"
fi
mkdir -p "$OUT"

# label     clip            pts   w     h     csp     fmt
# Only hdr10_4k.mp4 is synced to the Windows rig. SDR cases render into an
# R8G8B8A8 target; the hdr10_* cases render into a true HDR10 target
# (R10G10B10A2_UNORM) with a BT.2020/PQ TARGET_COLORSPACE -- the Phase-4a HDR
# render path (PQ into a >=10-bit surface that an 8-bit target cannot hold).
# Determinism + the Phase-3 no-op/effect invariants + hdr10!=sdr are the gate
# here; absolute HDR fidelity vs the windowed path is Phase 4b (NVIDIA display).
MATRIX="
sdr_t0     hdr10_4k.mp4    0.5   1280  720   none    rgba8
sdr_t2     hdr10_4k.mp4    2.0   1280  720   none    rgba8
srgb_t0    hdr10_4k.mp4    0.5   1280  720   srgb    rgba8
pq_t0      hdr10_4k.mp4    0.5   1280  720   pq2020  rgba8
hdr10_t0   hdr10_4k.mp4    0.5   1280  720   pq2020  rgb10a2
hdr10_t2   hdr10_4k.mp4    2.0   1280  720   pq2020  rgb10a2
"

rc=0
while read -r label clip pts w h csp fmt; do
  [ -z "${label:-}" ] && continue
  raw="$OUT/$label.raw"
  rm -f "$raw" "$raw.txt"
  if ! "$HARNESS" "$CLIPS/$clip" "$pts" "$w" "$h" "$raw" "$csp" "$fmt" >/dev/null 2>"$OUT/$label.err"; then
    echo "  FAIL $label  ($clip @ $pts, csp=$csp, fmt=$fmt)  -- harness error:"
    sed 's/^/      /' "$OUT/$label.err"
    rc=1
    continue
  fi
  rm -f "$OUT/$label.err"
  if [ ! -s "$raw" ]; then
    echo "  FAIL $label  ($clip @ $pts, csp=$csp, fmt=$fmt)  -- no output"; rc=1; continue
  fi
  echo "  OK   $label  ($clip @ $pts, csp=$csp, fmt=$fmt)  $(sha256sum "$raw" | cut -c1-16)"
done <<< "$MATRIX"

[ $rc -ne 0 ] && exit $rc

# Phase-3/4a invariants (independent of the self-baseline): an all-AUTO sRGB
# TARGET_COLORSPACE is documented as "same as omitted" => srgb_t0 byte-identical
# to no-param sdr_t0; a real BT.2020/PQ target must change the render => pq_t0
# differs; and the HDR10 10-bit target must differ from the SDR render.
sh() { sha256sum "$OUT/$1.raw" 2>/dev/null | cut -d' ' -f1; }
if [ "$(sh srgb_t0)" = "$(sh sdr_t0)" ]; then
  echo "  PASS srgb==none  : TARGET_COLORSPACE sRGB is a no-op vs omitted"
else
  echo "  FAIL srgb==none  : sRGB target changed the render"; rc=1
fi
if [ "$(sh pq_t0)" != "$(sh sdr_t0)" ]; then
  echo "  PASS pq!=none    : BT.2020/PQ TARGET_COLORSPACE changes the render"
else
  echo "  FAIL pq!=none    : PQ target had no effect"; rc=1
fi
if [ "$(sh hdr10_t0)" != "$(sh sdr_t0)" ]; then
  echo "  PASS hdr10!=sdr  : HDR10 10-bit PQ target differs from the SDR render"
else
  echo "  FAIL hdr10!=sdr  : HDR10 target produced the SDR pixels"; rc=1
fi

# P5b.1 wrap-lifetime regression (hdr-phase5b-assessment.md §2): after
# mpv_render_context_render returns, the host must be the SOLE owner of the
# target texture -- an engine-retained COM reference between frames blocks
# IDXGISwapChain::ResizeBuffers for direct-backbuffer hosts. Fails on 304f611.
if "$HARNESS" --refcount "$CLIPS/hdr10_4k.mp4" 0.5 640 360 "$OUT/refcount.raw" \
     >"$OUT/refcount.out" 2>&1; then
  echo "  PASS refcount    : host is sole owner of the target after render"
else
  echo "  FAIL refcount    : engine holds a reference on the host texture between frames"
  sed 's/^/      /' "$OUT/refcount.out"; rc=1
fi
rm -f "$OUT/refcount.raw" "$OUT/refcount.raw.txt" "$OUT/refcount.out"

[ $rc -ne 0 ] && exit $rc

if [ "$MODE" = "--baseline" ]; then
  echo "--- baseline written to $OUT ---"
  exit 0
fi

# Verify cand_d3d11/ against baseline_d3d11/.
BASE="$HERE/baseline_d3d11"
if [ ! -d "$BASE" ] || [ -z "$(ls -A "$BASE" 2>/dev/null)" ]; then
  echo "--- no baseline yet; run: rapi_d3d11_run.sh --baseline ---"
  exit 0
fi

echo "=== VERIFY (cand_d3d11 vs baseline_d3d11) ==="
# Compare via sha256sum (coreutils) rather than cmp/diff (diffutils), which is
# not guaranteed installed in a minimal MSYS2 UCRT64 environment.
sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
while read -r label clip pts w h csp fmt; do
  [ -z "${label:-}" ] && continue
  b="$BASE/$label.raw"; c="$OUT/$label.raw"
  if [ ! -f "$b" ]; then
    echo "  WARN $label : no baseline entry"; continue
  fi
  if [ "$(sha "$b")" = "$(sha "$c")" ] && [ "$(sha "$b.txt")" = "$(sha "$c.txt")" ]; then
    echo "  PASS $label : pixels+meta identical"
  else
    echo "  FAIL $label : DIFFERS from baseline"; rc=1
  fi
done <<< "$MATRIX"

if [ $rc -eq 0 ]; then
  echo "--- ALL PASS (D3D11 render-API path byte-stable) ---"
else
  echo "--- REGRESSION ---"
fi
exit $rc
