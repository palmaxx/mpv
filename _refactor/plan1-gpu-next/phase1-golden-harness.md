# Golden-frame regression harness (Phase 1)

Oracle for the `vo_gpu_next` -> render-API extraction: the windowed
`--vo=gpu-next` path must stay **bit-faithful** after every commit.

## Environment (this is part of the method -- record it)
- WSL2 Ubuntu 24.04, libplacebo **v7.360.1** built from source (Vulkan).
- GPU: **none**; Vulkan device = `llvmpipe` (Mesa lavapipe, CPU software
  rasterizer). A software rasterizer is bit-reproducible run-to-run on the
  same host -> ideal regression oracle. Proven: two independent runs of the
  full matrix (incl. 4K HDR10) are byte-identical (pixels + sidecar).
- Not representative of GPU perf, and not real-HDR-hardware fidelity. Those
  are the separate post-gate Windows steps the handoff scopes. For the
  extraction no-regression check, identical-pixels-on-same-renderer is the
  exact signal required.

## Method
`capture.sh <mpv> <outdir>` renders a fixed (clip,PTS) matrix through
`--vo=gpu-next --gpu-api=vulkan --gpu-sw=yes`, one frame held by `--pause
--start=PTS`, dumped by `golden.lua` on `playback-restart`:
- `<label>.png` -- `screenshot-to-file "video"` => gpu-next
  `video_screenshot` => `pl_render_image` (the extracted-core pixel path:
  scaling, tone-map, hooks, ICC, target colorspace).
- `<label>.png.json` -- canonical (key-sorted) `video-params`,
  `video-out-params` (colorspace / HDR mastering / sig-peak: the §3.1
  signal) and the ordered libplacebo render-pass desc list (structural
  render regression; nanosecond timings dropped => deterministic).

Pinned for determinism: `--no-config --interpolation=no`, fixed bilinear
scalers + `--dither-depth=8`; HDR adds `--tone-mapping=bt.2390
--hdr-compute-peak=no` (compute-peak is frame-history nondeterministic on a
single paused frame; the peak-detect hazard gets its own Phase-2 check).
Temporal interpolation (§3.2) is likewise a dedicated Phase-2 multi-frame
check, not this single-frame oracle.

## Matrix
| label | clip | PTS | profile |
|---|---|---|---|
| sdr_t0/t3/t7 | sdr_1080p25.mp4 (1080p25 bt709 h264) | 0.5 / 3.0 / 7.04 | SDR |
| hdr_t0/t2 | hdr10_4k.mp4 (4K24 PQ/bt2020 hevc 10-bit, mastering+CLL) | 0.5 / 2.0 | HDR |
| mot_t5 | motion_720p24.mp4 (720p24 bt709 h264) | 5.0 | SDR |

## Per-commit gate
```
bash _golden/capture.sh ./build/mpv _golden/cand
bash _golden/verify.sh  _golden/baseline _golden/cand   # exit!=0 => REGRESSION
```
`_golden/baseline/` = pristine mainline @ 35ae76d. `verify.sh` requires
every case to be pixel-identical (sha256) AND sidecar-identical.
