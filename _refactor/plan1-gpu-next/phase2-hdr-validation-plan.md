# Real-HDR-hardware validation plan (the caveat lavapipe cannot retire)

## Why this is needed (precisely)
The golden oracle runs on lavapipe (software Vulkan). A software swapchain
has **no HDR colorspace**: `sw->fns->target_csp` is absent and
`set_colorspace_hint` -> `pl_swapchain_colorspace_hint` is effectively
inert. So the golden runs proved the **pure math relocation** of the
hint/`apply_target_contrast` code is byte-identical (commits 0d612ff,
a12365c) -- but they did **not** exercise the swapchain-coupled branch
(target_csp read-back actually changing the result; set_color negotiation;
`external_params` true). That branch -- Phase 0 §3.1, the highest hazard --
only runs on a real HDR swapchain. This pass tests exactly that.

## What is being compared
Pristine mainline **`35ae76d`** vs **`a12365c`** (DR + full HDR-hint
extraction), windowed `--vo=gpu-next`, on a real GPU + a display in **HDR
mode**. Pass = bit-identical between the two builds for:
1. The deterministic golden PNG (reuse `_golden/golden.lua` /
   `screenshot-to-file "video"`).
2. The colorspace/HDR sidecar: `video-params` + `video-out-params`
   (sig-peak, primaries, transfer, **max/min-luma, max-cll, max-fall**) and
   the libplacebo render-pass list -- i.e. the negotiated `target.color` /
   `vo->params->color.hdr` that Phase 0 §3.1 names as the detection signal.
This is the *same* harness already proven on lavapipe; only the binary and
the machine change. The metric is **A==B** (refactor vs mainline on the
SAME real HW), not absolute correctness -- so it is robust to GPU/driver.

## Hard environment facts (verified this session)
- C:\ has ~7 GB free of 953 GB (full). A native Windows MSYS2 build of
  mpv + libplacebo(>=7.360.1, from source) + ffmpeg does not fit without
  the user first freeing several GB.
- WSL has 947 GB free and the mingw-w64 cross toolchain is apt-available.
- WSL has **no** real-GPU Vulkan (lavapipe only) and cannot present HDR
  (WSLg is software). => real-HDR can only be **a native Windows binary run
  on the Windows desktop with the HDR display**. WSL can only *build* it
  (cross-compile) or the user builds natively.
- The agent cannot observe an HDR display or drive an interactive HDR
  window. Validation MUST be the scripted, diffable artifact above, and
  **the user runs the two binaries** on the HDR-configured machine; the
  agent diffs the results.

## Candidate build approaches
- **A. Cross-compile from WSL (mingw-w64).** Build mpv + libplacebo +
  ffmpeg for Windows in WSL (roomy fs, zero C: impact); copy the small
  `mpv.exe`/`libmpv-2.dll` (x2: 35ae76d, a12365c) to Windows; user runs
  the harness on the HDR display. Heaviest build (libplacebo+ffmpeg mingw
  is multi-hour, failure-prone) but no Windows disk problem.
- **B. Native Windows MSYS2.** Matches exactly what Windows users run
  (most representative). Requires the user to free several GB on C:\ first;
  multi-step per HANDOFF §4.1.
- **C. Defer.** Accept the lavapipe result + a written code-review argument
  for the swapchain branch now; do real-HDR later on suitable HW. Lowest
  effort; leaves the one true caveat open (explicitly a non-pass).

## Gating prerequisite (no build is worth starting until confirmed)
A display that can actually enter **HDR mode** on this Windows machine,
plus a GPU/driver whose gpu-next swapchain reports HDR
(`sw->fns->target_csp`). Without it this validation is physically
impossible regardless of build approach.

---

## VERDICT (executed) — PASS, kill-point retired on real hardware

Native MSYS2 UCRT64 builds (libplacebo 7.360.1, d3d11): pristine
`35ae76d` vs refactor `a12365c`, NVIDIA GPU, display in Windows HDR mode,
`--vo=gpu-next --gpu-api=d3d11 --target-colorspace-hint=yes`, 4K HDR10
clip @ 2.0s.

- Rendered PNG: base1 == base2 == ref, all sha256 `46cdafff48cfffa4…`.
  base1==base2 => the real-GPU run-to-run noise floor is 0, so ref==base1
  is unambiguous (not noise-masked).
- Colorspace/HDR sidecar (canonical): ref bit-identical to base1.
  Negotiated target.color: primaries bt.2020, gamma pq, sig-peak 4.926,
  max-luma 1000, min-luma 0.005, max-cll 1000, max-fall 400; pass chain
  incl. "bt2390 tone map (1000 -> 271), gamut map (perceptual)".
- This differs from the lavapipe path (which gave 1000 -> 203 and a
  trivial pass list), confirming the D3D11 HDR swapchain
  target_csp/set_colorspace_hint round-trip was genuinely engaged — the
  exact branch Phase 0 §3.1 flagged and software could not test.

Conclusion: commits 0d612ff + a12365c are byte-identical to mainline on
real HDR hardware in both rendered output and negotiated HDR metadata.
The project's single biggest risk is retired. Phase 2 long tail unblocked.
