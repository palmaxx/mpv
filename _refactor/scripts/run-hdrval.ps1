# Real-HDR validation capture for any Phase 2 extraction commit.
#
# Compares the given refactor commit vs pristine 35ae76d on the real
# NVIDIA GPU + D3D11 HDR swapchain. Originally validated the kill-point
# a12365c (target_csp / set_colorspace_hint round-trip lavapipe could
# not exercise); now reused to revalidate later renderer-core sub-steps
# that touch the HDR pixel path. Pass -Ref <short-sha> for a build at
# C:\DEV\ai-dev\projects\mpv-wt-<short-sha>\bld\mpv.exe; outputs go to
# _golden\hdrval\<short-sha>\<mode>\ (mode = sw | hw) so neither earlier
# refs nor the other decode mode get clobbered.
#
# Pass -Hwdec to add --hwdec=auto -- exercises the hwdec acquire/release
# path (lavapipe cannot honestly verify hwdec; this is the rig for it).
# Without -Hwdec the run uses software decode, the same path the
# kill-point originally validated.
#
# RUN THIS WITH THE DISPLAY IN WINDOWS HDR MODE (Settings > System >
# Display > HDR = On, on the screen mpv will open on). It launches a brief
# fullscreen mpv three times, captures a frame + colorspace/HDR sidecar
# each, then exits. Tell Claude when done; Claude diffs the results.

param(
    # Short SHA of the candidate commit; expects a built worktree at
    # C:\DEV\ai-dev\projects\mpv-wt-<Ref>\bld\mpv.exe (see runbook §3).
    [string]$Ref = "97146f2",
    # Add --hwdec=auto so the hwdec acquire/release path is exercised
    # (the SW path is the default since it is what the original kill-point
    # validated and is deterministic on a wider range of HW).
    [switch]$Hwdec
)

$ErrorActionPreference = "Stop"
$mode   = if ($Hwdec) { 'hw' } else { 'sw' }
$repo   = "C:\DEV\ai-dev\projects\mpv-src"
$lua    = "$repo\_golden\golden.lua"
$clip   = "$repo\_golden\clips\hdr10_4k.mp4"
$outdir = "$repo\_golden\hdrval\$Ref\$mode"
$mpvBase = "C:\DEV\ai-dev\projects\mpv-wt-35ae76d\bld\mpv.exe"  # pristine
$mpvRef  = "C:\DEV\ai-dev\projects\mpv-wt-$Ref\bld\mpv.exe"     # refactor

# mpv.exe needs its MSYS2 UCRT64 DLLs (libplacebo, ffmpeg, lua, ...).
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
New-Item -ItemType Directory -Force -Path $outdir | Out-Null

$mpvArgs = @(
  "--no-config","--no-audio","--ao=null","--fullscreen",
  "--vo=gpu-next","--gpu-api=d3d11","--gpu-context=d3d11",
  "--pause","--osc=no","--interpolation=no",
  "--tone-mapping=bt.2390","--hdr-compute-peak=no",
  "--target-colorspace-hint=yes",
  "--start=2.0","--script=$lua",$clip
)
if ($Hwdec) {
    $mpvArgs += "--hwdec=auto"
    # Verbose vd/vo log lines reveal whether hwdec actually engaged (vs
    # silently falling back to sw), which is the whole point of -Hwdec.
    $mpvArgs += "--msg-level=vd=v,vo=v"
}
Write-Host "Mode: $mode$(if ($Hwdec) {' (--hwdec=auto)'} else {' (sw decode)'})" -ForegroundColor Yellow

# 3 runs: pristine x2 (= the GPU run-to-run noise floor) + refactor x1.
$runs = @(
  @{ name="base1"; exe=$mpvBase },
  @{ name="base2"; exe=$mpvBase },
  @{ name="ref";   exe=$mpvRef  }
)
foreach ($r in $runs) {
  $png = Join-Path $outdir "$($r.name).png"
  $log = Join-Path $outdir "$($r.name).log"
  Remove-Item -Force -ErrorAction SilentlyContinue $png, "$png.json", $log
  $env:GOLDEN_OUT = $png
  Write-Host "=== $($r.name): $($r.exe) ===" -ForegroundColor Cyan
  & $r.exe @mpvArgs 2>&1 | Out-File -FilePath $log -Encoding utf8
  if (Test-Path $png) {
    $h = (Get-FileHash $png -Algorithm SHA256).Hash.Substring(0,16)
    Write-Host "    captured $png  sha256:$h"
  } else {
    Write-Host "    FAILED: no output (HDR mode off? wrong screen?)" -ForegroundColor Red
  }
}
Write-Host ""
Write-Host "Done. Tell Claude 'hdr runs done' -- it will diff base1/base2/ref." -ForegroundColor Green
