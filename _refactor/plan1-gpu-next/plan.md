# Plan: mpv `vo_gpu_next` → render-API refactor (maintainer-spec, meticulous, time-boxed)

> **First execution action (post-approval):** write this document verbatim to
> `c:\DEV\ai-dev\projects\mpv-src\plan.md`. It is the working plan for the
> attempt; this plan-file copy is the approved spec.

## Context

Goal: an embedded, composited, 4K-10bit-HDR player. mpv already uses libplacebo
(gpu-next) — the SOTA HDR ceiling — but its **render API does not expose
gpu-next** (issue #10810). PR **#16818** (author: sparky3387, same dev as the
`sparky3387/mpvqt` fork) adds it, but maintainers rejected the approach:

- `na-na-hi`: *"basically a reimplementation of `vo_gpu_next.c` with significant
  code duplication … reimplement `vo_gpu_next.c` with the RA like `vo_gpu.c`
  does while making sure it causes no regression."*
- `Dudemanguy` (changes-requested, marked draft): *"design is pretty much a
  mess … GL code shouldn't be all over `gpu_next/context.c` … I'm on record for
  not liking the render API."* Decisive reviewer `haasn` (cc'd) silent.

This was assessed in-session as a **scope-trap** (≈5k-LOC core-subsystem
refactor, skeptical gatekeeper). The user has made an **informed decision** to
attempt the maintainer-spec refactor anyway — but as a **mergeable-quality
personal fork (upstream merge optional, not a success gate)**, executed **slow,
steady, manual, meticulous**, and **time-boxed** behind an explicit AI
feasibility gate and a hard-stop reassessment. This plan encodes exactly that.
Intended outcome: either a clean, behavior-preserving gpu-next render backend
the user controls (and *could* be upstreamed), or an **early, cheap, decisive
abandonment** to the known fallbacks.

## Endpoint & guardrails (locked with user)

- **Endpoint:** mergeable-quality personal fork. Architecture must match the
  maintainer spec (so it *could* merge), but success = a working fork the user
  controls; upstream submission is a bonus, **not** gated on `haasn`/merge.
- **No-regression bar (mandatory):** the windowed `--vo=gpu-next` path must be
  **bit/behaviour-faithful** (≈ every mpv desktop user) + the Windows libmpv
  render path the user actually runs. Full all-platform matrix = only if/when
  pursuing the optional upstream submission.
- **Time-box (hard):** execute **Phase 0 + Phase 1 only**, then **STOP at the
  Reassessment Gate** and explicitly re-decide before any deep extraction.
- Fallbacks on kill: fork `plplay.c` (`PLANffmpeg.md`), or vendor
  `sparky3387/mpvqt` + prebuilt #16818 binaries as-is.

## Target architecture (the maintainer-sanctioned shape)

Mirror the *existing* render-API two-layer abstraction. (Correction per Phase 0:
`video/out/gpu_next/` is **existing mainline** — the windowed `gpu_ctx`
swapchain helper, *not* a #16818 artifact. #16818's actual error was injecting
render-API GL *into* that file **and** duplicating `vo_gpu_next.c`'s render
loop/map/options instead of extracting them. Do not repeat that; extract.):

- Generic backend `render_backend_fns` (`video/out/libmpv.h`), registered in
  `render_backends[]` (`video/out/vo_libmpv.c:114`) alongside
  `render_backend_gpu`/`render_backend_sw`. Add **`render_backend_gpu_next`**.
- Per-GPU-API surface-wrap layer analogous to `libmpv_gpu_context_fns`
  (`video/out/gpu/libmpv_gpu.{c,h}`; today only `libmpv_gpu_context_gl`).
  Add a libplacebo-flavoured context-fns: GL first, D3D11/Vulkan later.
- **Extract the libplacebo core out of `vo_gpu_next.c`** into a shared unit
  consumed by *both* the windowed `vo_gpu_next` VO **and**
  `render_backend_gpu_next`. No duplication. `vo_gpu_next.c` becomes a thin
  `vo_driver` shell that owns its `gpu_ctx`/swapchain and calls the shared core.

## Phases

### Phase 0 — AI feasibility check (the requested initial AI gate)
Distinct from all human checks already done this session. Dispatch an AI agent
(read-only, against `c:\DEV\ai-dev\projects\mpv-src` + `gh pr diff 16818`) to
deliver, in writing:
1. **Extraction blast-radius map**: which `struct priv` fields
   (`vo_gpu_next.c:107`) and functions (`draw_frame:1071`, `reconfig:1568`,
   `control:1883`, `get_image:274`, `hwdec_get_tex:592`, `update_*options`,
   OSD/ICC/hooks/peak) are entangled with the windowed-VO `vo`/`gpu_ctx`, vs.
   cleanly extractable into a shared core.
2. **Salvage audit of PR #16818**: % reusable vs must-rewrite; concretely why it
   violated the `render_backend`/`libmpv_gpu_context_fns` model.
3. **Top-5 no-regression hazards** in making the windowed path call the
   extracted core (interpolation, HDR/peak-detect, ICC, user hooks, hwdec).
4. **Go/No-Go + confidence (low/med/high)** with the single biggest unknown.
**Gate:** if confidence is *low*, or it reports the core cannot be extracted
behavior-preserving without touching the windowed `draw_frame` semantics →
**stop, take a fallback.** (Time-boxed: advisory-strong, decision made at the
Phase-1 gate.)

### Phase 1 — Baseline harness + first behavior-preserving extraction
1. Build mainline mpv unchanged (meson) from the clone; establish a **golden
   baseline**: `--vo=gpu-next` on a fixed SDR clip, a 4K HDR10 clip, and a
   seek/interpolation clip → capture golden frames (`--screenshot`/`--vo-image`
   at fixed PTS), `--frame-drop` stats, and a perf trace. Scripted, repeatable.
2. Add the libmpv render-API test harness: build `sparky3387/mpvqt` against this
   tree (its README states the expected libmpv branch) as the integration rig.
3. **One** small, provably behavior-preserving extraction commit — Phase 0's
   identified lowest-risk unit: the **DR buffer pool + `get_image`**
   (`get_dr_buf`@241, `free_dr_buf`@257, `get_image`@274; fields
   `dr_lock`/`dr_buffers`/`num_dr_buffers`@121–123). Zero swapchain/`ra_ctx`/
   `vo` coupling, off the pixel path, maps 1:1 onto both
   `render_backend_fns.get_image` and `vo_driver.get_image_ts` — validates the
   "shared core, two front-ends" mechanic at near-zero risk. Extract into the
   shared core; the VO calls it. Rebuild; **golden frames + perf must match
   baseline exactly.**
4. `git diff --stat` must be minimal and comment-noise-free (Dudemanguy bar).

### ⛔ REASSESSMENT GATE (mandatory hard stop)
Do not proceed without a written go/no-go answering: did Phase 0 return ≥ medium
confidence? Did the Phase-1 extraction reproduce golden frames + perf *exactly*
with a clean minimal diff? Estimated effort for full extraction vs. the
fallbacks' cost? **Default = stop and choose a fallback unless all green.**
Everything below is *post-gate* and not part of the time-box.

### Phase 2 — Full core extraction (post-gate, incremental)
Repeat Phase-1 discipline: many small behavior-preserving commits, golden-frame
+ perf verified after **each**, until the libplacebo core
(`pl_renderer/pl_queue/pl_swapchain` use, `draw_frame`, hwdec mapping, OSD, ICC,
hooks, peak/perf) lives in the shared unit and `vo_gpu_next.c` is a thin shell
with **zero** windowed-path regression. **Per Phase 0: the HDR colorspace-hint
round-trip (`draw_frame` ~1162 `target_csp` → 1282 `set_colorspace_hint` →
1310 `pl_frame_from_swapchain`) is the project's true go/no-go** — carve it into
its *own dedicated, early* commit as a third pure helper the shell drives
between its swapchain calls; it must be proven **bit-identical on real HDR
hardware**. If that one block can't be made faithful, abandon to a fallback.

### Phase 3 — `render_backend_gpu_next` + GL context-fns
Add the new generic backend (mirror `render_backend_gpu`) + a libplacebo GL
context-fns (mirror `libmpv_gpu_context_gl`: `init`/`wrap_fbo`→pl-importable
surface/`done_frame`/`destroy`), driving the shared core. Add the public API
type (`include/mpv/render.h`). GL code stays *only* in the context-fns.

### Phase 4 — Integration + HDR validation
`sparky3387/mpvqt` (or mpv-examples) renders via the new backend: SDR correct,
then **4K HDR10 not washed-out**, seek/interpolation correct, clean teardown,
ASan clean.

### Phase 5 — Optional (bonus only)
D3D11/Vulkan context-fns; upstream-submission hygiene (exact style, minimal
diff, regression matrix, drop AI comments) — only if pursuing the optional PR.

## Critical files (verified in clone)

- `video/out/vo_gpu_next.c` — extraction source: `struct priv`@107,
  `preinit`@2228 (`gpu_ctx_create`), `draw_frame`@1071, `reconfig`@1568,
  `control`@1883, `uninit`@2170, `get_image`@274, `hwdec_get_tex`@592.
- `video/out/libmpv.h` — `render_backend_fns` contract (mirror it).
- `video/out/vo_libmpv.c` — `render_backends[]`@114; dispatch in
  `mpv_render_context_create`.
- `video/out/gpu/libmpv_gpu.{c,h}` — `render_backend_gpu`,
  `libmpv_gpu_context_fns`, `context_backends[]`@8 (the pattern to mirror).
- `include/mpv/render.h` — `MPV_RENDER_API_TYPE_*` (add the new type).
- PR #16818 (via `gh pr diff 16818 --repo mpv-player/mpv`) — salvage + anti-pattern reference.

## Reuse (don't reinvent)

- Mirror `render_backend_gpu` + `libmpv_gpu_context_gl` structurally — they are
  the maintainer-blessed shape.
- Reuse `vo_gpu_next.c`'s existing libplacebo logic *by extraction*, not rewrite.
- `sparky3387/mpvqt` = ready integration harness (don't build one).
- Existing clone `c:\DEV\ai-dev\projects\mpv-src`; `mpv-src` build = meson.

## Verification

- **Golden baseline (Phase 1, before any change):** scripted frame dumps at
  fixed PTS + frame-drop/perf trace for `--vo=gpu-next` on SDR, 4K-HDR10, and a
  seek/interpolation clip.
- **Per-commit regression:** rebuild; new golden frames must be
  pixel-identical (or within documented tolerance) and perf within noise vs.
  baseline; `git diff --stat` minimal, no comment pollution.
- **Integration:** `sparky3387/mpvqt` plays SDR then 4K HDR10 via the new
  backend; visual correctness vs. windowed `vo=gpu-next`; ASan clean; clean
  shutdown/resize/EOF.
- **Kill criteria are verification too:** failing the Reassessment Gate is a
  *successful* outcome of the time-box — record findings, take a fallback.

## Fallbacks (explicit, no shame)

`PLANffmpeg.md` (fork `plplay.c`), or vendor `sparky3387/mpvqt` + prebuilt
#16818 CI binaries unmodified. Both reach the same libplacebo HDR ceiling.

---

## Completeness assessment (added 2026-06-01, post-F-phase)

**This plan (Plan 1: expose gpu-next through the libmpv render API, GL
context-fns only) is CODE-COMPLETE and validated to its mandatory bar.**
The HDR-over-render-API work (D3D11/Vulkan/Metal backends + per-backend
SW/HW harnesses + `TARGET_COLORSPACE`) is a *separate* effort —
`plan-hdr-render-api.md` — and is NOT part of this plan's completeness.

### Phase status

| Phase | State |
|---|---|
| 0 — AI feasibility | ✅ GO / MEDIUM |
| 1 — baseline + first extraction | ✅ |
| ⛔ Reassessment Gate | ✅ PASSED |
| 2 — full core extraction | ✅ 17 commits; `vo_gpu_next.c` **+127/−2164** (2725→688 lines); libplacebo core lives in shared `gpu_next/core` |
| 3 — `render_backend_gpu_next` + GL context-fns | ✅ W1–W6b: backend registered in `render_backends[]`, public `MPV_RENDER_API_TYPE_PL_OPENGL`, **every `render_backend_fns` hook real** (no stubs; `reconfig` no-op + `AMBIENT_LIGHT` NOT_IMPLEMENTED are intentional/documented), GL confined to `opengl/libmpv_pl_gl.c`, `gpu_next/context.c` untouched |
| **hwdec (UNDOCUMENTED FLAW in the original spec)** | ✅ **FIXED — F-phase F1.1–F1.3 (`286b788`).** The Phase-2/3 extraction wired the *SW-decode* path through the core but never stood up the hwdec registry on the render backend → a libmpv host on PL_OPENGL got software decode only. F1 adds the registry (mirrors `libmpv_gpu.c`: `hwdec_devices_create` + `ra_hwdec_ctx_init(load_all_by_default=true)`), has the GL context-fns expose `ra`/`ra_ctx` for hwdec to bind, and feeds the core's existing `hwdec_get` hook. **No core changes** — the render-API hwdec map path IS the windowed VO's map path. |
| 4 — integration + HDR validation | ✅ in substance via `_golden/render_api/` (a self-baselined GL render-API harness replaced the suggested mpvqt rig); SDR/HDR-to-FBO byte-stable. mpvqt itself was a *suggested rig*, not a deliverable — not built. |
| 5 — D3D11/Vulkan context-fns + upstream hygiene | Explicitly *optional bonus* → **deferred to `plan-hdr-render-api.md` (Plan 2).** |

### Validation completeness

| Path | State |
|---|---|
| Windowed `--vo=gpu-next` no-regression **(the mandatory bar)** | ✅ bit-faithful: lavapipe 12-case golden + teardown, and real-HDR D3D11 SW+HW (d3d11va) through F1.3 (`run-hdrval` base1==base2==ref, 2026-06-01) |
| Render-API SDR via PL_OPENGL | ✅ self-baselined harness (4 cases: SDR/HDR/motion) byte-stable |
| Render-API **hwdec** (the F1.3 feature) | ⚠️ compile/link-validated on Windows MSYS2 + **verbatim `libmpv_gpu` pattern** + shares the core map path already HW-proven on the windowed side — **but not yet functionally exercised on real GPU hardware** |
| Render-API subs / ICC | ⚠️ shared-core code (windowed-validated via `sdr_subs`) but the render-API harness doesn't pass subs/ICC |

### The one thing the current rig cannot prove

Functional proof that **render-API hwdec engages on real hardware.** The
WSL harness is surfaceless-EGL/**llvmpipe (software)** — no GPU decoder
interop — and no real-GPU render-API host exists. **This does not require
building Plan 2's D3D11/Vulkan/Metal backends; it requires *a real-GPU
render-API host*.** Two ways to get one:

- **(Recommended) Fold it into Plan 2's first backend.** Plan 2's D3D11
  backend + its HW harness drives the render API on the real NVIDIA rig
  with d3d11va — which proves render-API hwdec *as a byproduct*, on the
  exact path the user targets (D3D11/HDR). Zero throwaway work.
- (Alternative) Port the GL harness to a Windows native-GL host
  (WGL/ANGLE) + enable `d3d11egl`/`cuda` hwdec, proving the GL-path
  hwdec before Plan 2. More thorough, but partly duplicates Plan 2's
  harness effort, and the GL render-API path is not the production target.

### Known issues before *upstream* merge (see `GPU_NEXT_VALIDATION_ISSUES.md`)

A prior independent review (`GPU_NEXT_VALIDATION_ISSUES.md`, this folder,
audited at F1.3) found items beyond the hwdec-functional gap. **None block
the user's Windows/D3D11 personal-fork endpoint;** they matter for full /
Linux / upstream merge-worthiness. **Update 2026-06-01: #5 and #2 are FIXED
(`1dc4802`, `82a1428`); #1/#3/#4 remain open/deferred** (see the review
doc's resolution banner):

1. **Native-resource forwarding missing (Linux GL hwdec).** `libmpv_gpu.c`
   copies `MPV_RENDER_PARAM_{X11_DISPLAY,WL_DISPLAY,DRM_DRAW_SURFACE_SIZE,
   DRM_DISPLAY_V2}` into the RA via `ra_add_native_resource()` before hwdec
   init; `render_backend_gpu_next::init` does not mirror that loop. → VAAPI /
   VDPAU / DRM-PRIME GL hwdec parity likely incomplete. **Windows D3D11/EGL +
   CUDA unaffected** (they don't use those native resources). *Fix when/if
   Linux GL render-API hwdec is targeted.*
2. **`VO_CAP_FILM_GRAIN` advertised but ineffective.** The backend sets
   `driver_caps |= VO_CAP_FILM_GRAIN`, but `vo_libmpv.c` hardcodes `.caps`
   and decode-side film-grain checks `vo->driver->caps`, not the backend's
   `driver_caps`. → AV1 film grain likely disabled on the PL_OPENGL/PL_D3D11
   path. **Affects the render API generally incl. Windows** — small fix,
   worth doing.
3. **render-API hwdec not functionally proven on real HW** (the gap above) —
   rides Plan 2's D3D11 HW harness.
4. **Upstream review hygiene** — `Co-Authored-By` trailers + explanatory
   comments + harness notes in commit messages; squash/prune for a PR
   (#16818 objected to AI-looking commentary). N/A for the personal fork.
5. **Dead code:** `gpu_next_core_renderer()` declared/defined but unused —
   remove or use before a maintainer review.

### Verdict

**Plan 1 is functionally complete for the Windows/D3D11 personal-fork
endpoint** (code-complete incl. hwdec; mandatory windowed no-regression bar
fully met incl. real-HDR SW+HW; render-API SDR self-baselined; hwdec the
blessed `libmpv_gpu` pattern over an already-HW-proven core,
Windows-compile-validated). Two classes of deferred work remain, **neither
blocking the Windows path**: (a) the render-API-hwdec *functional-on-HW*
proof, most efficiently earned as Plan 2's first D3D11 HW-harness milestone;
(b) the upstream-merge gaps in `GPU_NEXT_VALIDATION_ISSUES.md` (#1 Linux
native-resources, #2 film-grain caps, #5 dead code, #4 hygiene). For the
user's endpoint the only one worth fixing soon is **#2 (film-grain caps)** —
small, affects the render API on Windows too. **Plan 1 does not need to be
held open to start Plan 2**, and Plan 2 needs no new backend written "to
validate Plan 1."
