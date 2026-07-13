#!/usr/bin/env bash
# Self-baselined Vulkan render-API golden gate for the gpu_next render backend.
#
#   rapi_vk_run.sh              capture into cand_vk/, diff vs baseline_vk/
#   rapi_vk_run.sh --baseline   capture into baseline_vk/ (re-baseline)
#
# Drives rapi_harness_vulkan (libmpv render API -> MPV_RENDER_API_TYPE_PL_VULKAN
# -> render_backend_gpu_next -> wrapped VkImage), reads each frame back as raw
# RGBA8 and compares sha256. The host is a lavapipe (software Vulkan) device,
# deterministic run-to-run, so this SELF-baselines like the GL/D3D11 gates.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CLIPS="$ROOT/_golden/clips"
HARNESS="$HERE/rapi_harness_vulkan"

[ -x "$HARNESS" ] || { echo "no rapi_harness_vulkan -- run build_vulkan.sh first"; exit 1; }

MODE="${1:-verify}"
if [ "$MODE" = "--baseline" ]; then
  OUT="$HERE/baseline_vk"
else
  OUT="$HERE/cand_vk"
fi
mkdir -p "$OUT"

# label     clip               pts   w     h
MATRIX="
sdr_t0     sdr_1080p25.mp4    0.5   1280  720
sdr_t3     sdr_1080p25.mp4    3.0   1280  720
hdr_t2     hdr10_4k.mp4       2.0   1280  720
mot_t5     motion_720p24.mp4  5.0   1280  720
"

rc=0
while read -r label clip pts w h; do
  [ -z "${label:-}" ] && continue
  raw="$OUT/$label.raw"
  rm -f "$raw" "$raw.txt"
  if ! "$HARNESS" "$CLIPS/$clip" "$pts" "$w" "$h" "$raw" >/dev/null 2>"$OUT/$label.err"; then
    echo "  FAIL $label  ($clip @ $pts)  -- harness error:"
    sed 's/^/      /' "$OUT/$label.err"
    rc=1
    continue
  fi
  rm -f "$OUT/$label.err"
  if [ ! -s "$raw" ]; then
    echo "  FAIL $label  ($clip @ $pts)  -- no output"; rc=1; continue
  fi
  echo "  OK   $label  ($clip @ $pts)  $(sha256sum "$raw" | cut -c1-16)"
done <<< "$MATRIX"

[ $rc -ne 0 ] && exit $rc

if [ "$MODE" = "--baseline" ]; then
  echo "--- baseline written to $OUT ---"
  exit 0
fi

BASE="$HERE/baseline_vk"
if [ ! -d "$BASE" ] || [ -z "$(ls -A "$BASE" 2>/dev/null)" ]; then
  echo "--- no baseline yet; run: rapi_vk_run.sh --baseline ---"
  exit 0
fi

echo "=== VERIFY (cand_vk vs baseline_vk) ==="
while read -r label clip pts w h; do
  [ -z "${label:-}" ] && continue
  b="$BASE/$label.raw"; c="$OUT/$label.raw"
  if [ ! -f "$b" ]; then echo "  WARN $label : no baseline entry"; continue; fi
  if cmp -s "$b" "$c" && cmp -s "$b.txt" "$c.txt"; then
    echo "  PASS $label : pixels+meta identical"
  else
    echo "  FAIL $label : DIFFERS from baseline"; rc=1
  fi
done <<< "$MATRIX"

if [ $rc -eq 0 ]; then
  echo "--- ALL PASS (Vulkan render-API path byte-stable) ---"
else
  echo "--- REGRESSION ---"
fi
exit $rc
