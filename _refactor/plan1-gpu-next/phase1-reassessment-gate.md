# ⛔ PHASE 1 — REASSESSMENT GATE (mandatory hard stop)

Per `plan.md`: execute Phase 0 + Phase 1 only, then STOP here and
explicitly re-decide before any deep extraction. This is that document.
**Verdict: all four gate criteria are GREEN. Decision is the user's.**

## What was done this session (Phase 1, complete)

- **Build env stood up** (the deferred §4.1 decision → WSL/Ubuntu, user
  approved). WSL2 Ubuntu 24.04; **libplacebo v7.360.1 built from source**
  (Vulkan+shaderc+lcms+opengl) because apt ships only 6.338 < mpv's
  required >=7.360.1; mpv built at pinned `35ae76d` (lua enabled solely so
  the golden harness can script `screenshot-to-file`; source unchanged).
- **Deviation (forced, benign):** the C: drive is 100% full (7.3 GB free),
  so building in `c:\DEV\...\mpv-src` is unsafe and slow. Work tree is a
  local clone in WSL-native fs: `~/mpv-fork` (948 GB free), branch
  **`gpu-next-render-api`**, HEAD `5ad4900`. The Windows host is untouched
  (aligns with the handoff's "don't touch the Windows host" intent). The
  Windows clone remains the docs home and can `git fetch` the WSL branch.
- **GPU reality:** no GPU passthrough; Vulkan device = `llvmpipe`
  (lavapipe, CPU software). This is the *ideal regression oracle*
  (bit-reproducible run-to-run — proven). It is NOT GPU-perf- nor
  real-HDR-hardware-representative; those are the post-gate Windows steps
  the handoff already scopes.
- **Golden baseline** established, byte-deterministic, documented
  (`_golden/README.md`): matrix = SDR 1080p, **4K HDR10** (PQ/BT.2020 +
  mastering+CLL metadata), motion 720p24; per case a PNG (gpu-next
  `video_screenshot`→`pl_render_image`) + a canonical colorspace/HDR +
  render-pass sidecar. Two independent full-matrix runs => identical.
- **First extraction commit `5ad4900`** — DR buffer pool + get_image →
  new `video/out/gpu_next/core.{c,h}` (opaque `struct gpu_next_core`); VO
  keeps a 1-line shim, mirroring gl_video shared by vo_gpu.c/libmpv_gpu.c.

## Gate criteria

| # | Question | Answer |
|---|---|---|
| 1 | Phase 0 confidence ≥ medium? | **YES** — GO / MEDIUM (`phase0-feasibility.md`). |
| 2 | First extraction reproduced golden frames + perf EXACTLY, clean minimal diff? | **YES** — all 6 cases (incl. 4K HDR10) pixel-AND-sidecar byte-identical to pristine baseline (same sha256). Diff: vo_gpu_next.c `+7/-76`, meson `+1`, 2 new files, **no comment noise** (Dudemanguy bar met); VO net-shrinks, logic extracted not duplicated. Clean play-to-EOF teardown, no leak/assert. |
| 3 | Effort: full extraction vs. fallback? | See below. |
| 4 | Default = stop & take a fallback unless all green. | Criteria 1–2 GREEN; 3 assessed. |

## Criterion 3 — effort & residual risk

- The DR commit **validated the entire "shared core, two front-ends"
  mechanic end-to-end at near-zero risk**. The maintainer-blessed
  architecture (extract, don't duplicate; mirror gl_video) is proven to
  work in this tree. The harness can detect a 1-bit regression incl. on
  4K HDR10. This de-risks the *method*.
- **Residual true-risk is unchanged from Phase 0 §3.1/§5:** the HDR
  colorspace-hint round-trip in `draw_frame` (target_csp → hint math →
  set_colorspace_hint → read-back). It must be carved into a third pure
  shell-driven helper and proven **bit-identical on REAL HDR hardware**.
  lavapipe proves *regression* (same renderer, before vs after) but
  **cannot prove HDR-hardware fidelity** of the reordered hint sequence —
  that needs the Windows HDR path, which is outside this time-box.
- **Rough effort (meticulous, golden-gated per commit):** Phase 2 = the
  bulk (~1700 substantive lines of class-A/C logic; many small commits;
  the HDR-hint as its own early dedicated commit = the real go/no-go).
  Phase 3 (`render_backend_gpu_next` + libplacebo GL context-fns) is
  well-templated by `render_backend_gpu`/`libmpv_gpu_context_gl`. Phase 4
  integration + HDR validation. Realistically multi-week.
- **Fallbacks remain cheap** and reach the same libplacebo HDR ceiling:
  fork `plplay.c`, or vendor `sparky3387/mpvqt` + prebuilt #16818 binaries.

## Deferred (transparent)

- **mpvqt render-API harness** (plan Phase 1.2): NOT built. Not
  gate-critical — the gate hinges on extraction exactness, verified via
  the windowed gpu-next golden path; Phase 0/handoff place render-API
  integration value *post-gate* (it exercises the Phase-3 backend that
  does not exist yet). It is at most an input to the Phase-3 effort line.
- DR path not dynamically exercised: lavapipe lacks the libplacebo limits
  that enable DR, so the pool stayed inactive (identically before/after).
  The move is verbatim; real-GPU DR exercise = post-gate Windows step.

## Recommendation (decision is the user's — this is the time-box end)

All green: Phase 0 medium, extraction exact + clean, mechanic proven. The
plan's "default = stop unless all green" is satisfied to *continue* — but
the time-box deliberately ends here so the user re-decides with eyes open,
because the genuinely hard part (HDR-hint, needing real HDR hardware to
truly validate) is Phase 2+ and out of this box. Options:

- **A. Proceed to Phase 2** — next commit = the HDR colorspace-hint helper
  (the true kill-point), golden-gated; then plan a Windows/real-HDR
  validation pass. Highest value, highest effort, real residual risk.
- **B. Bank the proven mechanic, take a fallback now** — successful
  time-box outcome per the plan; cheapest path to the HDR ceiling.
- **C. Continue extracting low-risk class-A units** (options schema,
  cache, OSD/ICC helpers) to shrink the VO further *before* committing to
  the HDR-hint risk — incremental, defers the kill-point decision.
