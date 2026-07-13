#!/usr/bin/env bash
# macOS MoltenVK + VideoToolbox render-API regression gate.
#
# The manifest is local because its media paths belong to the user's mounted
# library. See macos_cases.tsv.example. This gate renders a held frame into a
# host-owned VkImage, reads it back, self-baselines raw pixels, and—where a
# case requests hwdec—requires hwdec-current=videotoolbox with no HW-downloading
# log fallback. It deliberately does not claim to validate CAMetalLayer EDR;
# that is an ARCA-host integration gate.
#
#   ./build_vulkan.sh
#   ./rapi_macos_run.sh --manifest ~/mpv-macos-cases.tsv --baseline
#   ./rapi_macos_run.sh --manifest ~/mpv-macos-cases.tsv
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
HARNESS="$HERE/rapi_harness_vulkan"
BASE="$HERE/baseline_macos_vk"
CAND="$HERE/cand_macos_vk"
MANIFEST=""
BASELINE=0

usage() {
  cat >&2 <<EOF
usage: $(basename "$0") --manifest <cases.tsv> [--baseline]

Build first with: $HERE/build_vulkan.sh
EOF
  exit 2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --manifest) [ "$#" -ge 2 ] || usage; MANIFEST="$2"; shift 2 ;;
    --baseline) BASELINE=1; shift ;;
    *) usage ;;
  esac
done

[ -n "$MANIFEST" ] && [ -f "$MANIFEST" ] || usage
[ -x "$HARNESS" ] || { echo "missing $HARNESS; run build_vulkan.sh first" >&2; exit 2; }

# Homebrew's MoltenVK formula installs this standard loader ICD. Allow callers
# to override it for an SDK-installed or app-bundled MoltenVK instead.
DEFAULT_ICD="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"
if [ -z "${VK_DRIVER_FILES:-}" ] && [ -z "${VK_ICD_FILENAMES:-}" ]; then
  [ -f "$DEFAULT_ICD" ] || { echo "MoltenVK ICD not found: $DEFAULT_ICD" >&2; exit 2; }
  export VK_DRIVER_FILES="$DEFAULT_ICD"
fi

OUT="$CAND"
if [ "$BASELINE" -eq 1 ]; then
  OUT="$BASE"
fi
mkdir -p "$OUT"

rc=0
while IFS=$'\t' read -r label clip pts width height csp fmt hwdec extra; do
  case "$label" in ''|'#'*) continue ;; esac
  if [ -n "${extra:-}" ] || [ -z "${hwdec:-}" ]; then
    echo "FAIL $label: expected eight tab-separated fields" >&2
    rc=1
    continue
  fi
  if [ ! -f "$clip" ]; then
    echo "FAIL $label: clip is not readable: $clip" >&2
    rc=1
    continue
  fi
  case "$hwdec" in yes|no) ;; *) echo "FAIL $label: hwdec must be yes or no" >&2; rc=1; continue ;; esac

  raw="$OUT/$label.raw"
  rm -f "$raw" "$raw.txt" "$OUT/$label.log"
  args=()
  [ "$hwdec" = yes ] && args+=(--hwdec)
  if "$HARNESS" "${args[@]}" "$clip" "$pts" "$width" "$height" "$raw" "$csp" "$fmt" \
       >"$OUT/$label.log" 2>&1; then
    echo "OK   $label  $(shasum -a 256 "$raw" | cut -c1-16)"
  else
    echo "FAIL $label: harness failed; see $OUT/$label.log" >&2
    rc=1
    continue
  fi
  if [ "$hwdec" = yes ]; then
    if ! grep -Eq '^hwdec_current=.*videotoolbox' "$raw.txt" || \
       ! grep -qx 'hw_downloading=0' "$raw.txt"; then
      echo "FAIL $label: VideoToolbox zero-copy proof missing; see $raw.txt" >&2
      rc=1
    fi
  fi
  if [ "$BASELINE" -eq 0 ]; then
    baseline="$BASE/$label.raw"
    if [ ! -f "$baseline" ]; then
      echo "FAIL $label: no baseline; rerun with --baseline" >&2
      rc=1
    elif ! cmp -s "$baseline" "$raw"; then
      echo "FAIL $label: rendered pixels differ from baseline" >&2
      rc=1
    fi
  fi
done < "$MANIFEST"

if [ "$BASELINE" -eq 1 ] && [ "$rc" -eq 0 ]; then
  echo "Baseline written to $BASE"
elif [ "$rc" -eq 0 ]; then
  echo "PASS: all macOS Vulkan render cases match their baseline"
fi
exit "$rc"
