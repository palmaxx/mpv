# Merge / audit handoff — gpu-next through the libmpv render API (Plan 1) + HDR over the render API (Plan 2)

> **Superseded status header (2026-06-20):** this document remains the detailed
> historical implementation handoff, but its old base SHA, WSL locations, and
> deferred-hwdec statements are no longer current. Use
> [`FINAL_PR_AUDIT.md`](FINAL_PR_AUDIT.md) for the frozen candidate
> `a8c7cfe7812e3d828eb603397860500a1569c512`, the final finding ledger,
> validation record, limitations, and review order. A post-freeze ARCA run found
> that D3D11VA initializes but is CPU-downloaded because `check_format()` omits
> hwdec formats; local additive commit `f728638bc1` fixes and validates the
> zero-copy path. Vulkan exposes `ra_create_pl` for
> compatible interops; VideoToolbox-over-MoltenVK remains unvalidated; true
> Vulkan Video remains deferred. The material below is intentionally preserved
> because its architecture, phase rationale, harness descriptions, and commit
> subject map remain useful.

**Audience:** a maintainer or agent performing a detailed code audit toward
merging this work into `mpv-player/mpv`. **Scope:** everything from the upstream
base `f239c2dd` to the Plan-2 tip. **Author's claim:** Plan 1 is
merge-complete; Plan 2's core (D3D11 + Vulkan backends + the `TARGET_COLORSPACE`
param) is implemented and validated, with a small, clearly-scoped tail deferred
(native Metal, render-API hwdec, on-screen production shell). This doc is the
map + the honest list of deviations, deferrals, and merge-prep decisions.

> All SHAs below are **post-rebase local** SHAs on branch
> `gpu-next-render-api-hdr` in the WSL fork `~/mpv-fork` (mirror: Windows
> worktree `C:\DEV\ai-dev\projects\mpv-wt-hdr`). They will change on the next
> upstream rebase — **identify commits by subject line**, which is stable.
> Working/validation docs live under `_refactor/` (git-excluded); the live
> entry point is `_refactor/INDEX.md`, per-phase detail in
> `_refactor/plan{1-gpu-next,2-hdr-render-api}/`.

---

## 0. TL;DR for the auditor

- **One renderer, two front-ends.** `vo_gpu_next.c` had its libplacebo core
  (~2160 lines) extracted into a new shared `video/out/gpu_next/core.{c,h}`,
  consumed by both the existing windowed VO **and** a new libmpv render backend
  `render_backend_gpu_next`. `vo_gpu_next.c` shrank from ~2725 to ~688 lines
  (**+127/−2164**); the extraction is pure code-motion, gated bit-identical at
  every step.
- **Per-GPU-API surface layer.** A small `libmpv_pl_context_fns` interface with
  three implementations — `libmpv_pl_context_{gl,d3d11,vulkan}` — wraps the
  host's GL FBO / `ID3D11Texture2D` / `VkImage` as a `pl_tex`. The render hook
  is API-agnostic.
- **One new render param** `MPV_RENDER_PARAM_TARGET_COLORSPACE` carries the
  host's target surface colorspace + HDR metadata (mpv-side enums, no leaked
  libplacebo types), enabling HDR over the render API on any backend.
- **Total diff vs `f239c2dd`:** 16 files, **+5141/−2166**. 9 new files, 5
  modified (vo_gpu_next.c is the extraction; the rest are 1-line registrations +
  meson + docs).
- **Validation:** deterministic software gates (lavapipe Vulkan windowed golden,
  llvmpipe/EGL render-API harness, WARP D3D11 harness, lavapipe Vulkan harness)
  byte-stable at every commit; **real NVIDIA D3D11 HDR hardware** confirms both
  the windowed path (byte-identical to pristine, SW+HW decode) and the
  render-API D3D11 HDR path (target negotiation bit-identical, on-display visual
  match).
- **No bugs found in this audit pass.** The open items (§9) are *deferrals* and
  *PR-strategy decisions*, not defects.

---

## 1. What the two plans deliver

**Plan 1 — expose gpu-next through the libmpv render API (GL).** Lets a libmpv
host drive mpv's libplacebo (gpu-next) renderer via
`mpv_render_context_*` with `MPV_RENDER_API_TYPE_PL_OPENGL`, identical init/FBO
params to the legacy `MPV_RENDER_API_TYPE_OPENGL`. The prerequisite was
separating the renderer core out of the VO so a second front-end could exist
without duplication. Includes hwdec on the render API (the F-phase). **The GL
render path is structurally SDR** (8-bit host FBO, no swapchain to negotiate a
target colorspace).

**Plan 2 — HDR over the render API.** Adds the pieces the GL path structurally
can't carry HDR with:
1. Per-API surface backends that wrap an HDR-capable host texture:
   `libmpv_pl_context_d3d11` (`MPV_RENDER_API_TYPE_PL_D3D11`) and
   `libmpv_pl_context_vulkan` (`MPV_RENDER_API_TYPE_PL_VULKAN`).
2. `MPV_RENDER_PARAM_TARGET_COLORSPACE` — the host tells mpv "my surface is
   BT.2020/PQ, peak N nits" per frame, orthogonal to the graphics API.

Plan 2 is built **on top of** Plan 1 (its branch contains Plan 1's commits).
`gpu_next/core` is unchanged by Plan 2 except that the render hook reads one new
param; the maintainer-blessed shape is preserved end to end.

---

## 2. Branch & commit map (oldest → newest)

`gpu-next-render-api-hdr` on upstream `f239c2dd`. Plan-1 deliverable also exists
standalone as `gpu-next-render-api` (tip = the `…film grain` commit below).

### Plan 1 — extraction (E/W phases), wiring, hwdec (F), review fixes

| Group | Commit subjects (in order) | What |
|---|---|---|
| Core extraction | `extract DR buffer pool` → … → `move required-frames formula into gpu_next/core` (≈30 commits: pl_options/renderer/queue ownership, the render call, SW-upload mapping, hwdec interop, caches, ICC, render-option resolution, overlays, target-CSP, screenshot) | Lift `vo_gpu_next.c`'s libplacebo core into `gpu_next/core.{c,h}`, each commit windowed-golden bit-identical |
| Render-backend scaffolding | `add render_backend_gpu_next scaffolding`, `define libmpv_pl_context_fns interface`, `add libmpv_pl_context_gl`, `register render_backend_gpu_next in the libmpv render API` | The new front-end + the GL surface layer + registration in `render_backends[]` |
| The render hook | `implement the libmpv render backend (W5-6d)`, `move video_screenshot into gpu_next/core (W6a)`, `wire optional render-backend hooks (W6b)` | The actual `render()` + screenshot/get_image/perfdata/set_parameter hooks |
| hwdec on render API (F) | `add ra fields to libmpv_pl_context (F1.1)`, `build ra_ctx alongside pl_opengl … (F1.2)`, `wire hwdec on the libmpv render API (F1.3)` | GL backend builds a blank `ra_ctx` so the existing `ra_hwdec_ctx` registry works |
| Review fixes | `drop unused gpu_next_core_renderer()`, `stop advertising VO_CAP_FILM_GRAIN on the render backend` | Issues #5, #2 from `GPU_NEXT_VALIDATION_ISSUES.md` |

### Plan 2 — HDR (P / V phases)

| Commit subject | What | Diff |
|---|---|---|
| `libmpv: add PL_D3D11 + TARGET_COLORSPACE public C-API surface (P1.1)` | `render.h` enums + `render_d3d11.h` + structs; **API 2.5→2.6** + rst (retro-documents PL_OPENGL too) | +334/−2 |
| `add libmpv_pl_context_d3d11 stub (P1.2)` | stub file + meson gate, unregistered | +74 |
| `read MPV_RENDER_PARAM_TARGET_COLORSPACE into a local (P1.3)` | inert param read (scaffold) | +3 |
| `implement libmpv_pl_context_d3d11 (P2.1)` | `pl_d3d11_create`/`pl_d3d11_wrap` + register | (full body) |
| `gate render-backend hwdec registry on a surface ra_ctx (P2.2)` | the D3D11 backend has no `ra_ctx`; gate the hwdec registry so it runs SW-decode-only instead of asserting | +20/−13 |
| `wire MPV_RENDER_PARAM_TARGET_COLORSPACE into the render hook (P3.1)` | mpv-enum→`pl_color_space` translation + precedence into `target.color` | +107/−9 |
| `add PL_VULKAN public C-API surface + stub (V1.1)` | `render_vulkan.h` + enums + stub + meson gate; **API 2.6→2.7** + rst | +289/−2 |
| `implement libmpv_pl_context_vulkan (V2.1)` | `pl_vulkan_import`/`pl_vulkan_wrap` + the per-frame acquire/hold sync + a new optional `acquire_target` hook + register | +163/−7 |

---

## 3. Architecture (the maintainer-blessed two-layer shape)

```
public API:
  include/mpv/render.h        MPV_RENDER_API_TYPE_PL_OPENGL / _PL_D3D11 / _PL_VULKAN
                              MPV_RENDER_PARAM_TARGET_COLORSPACE (+ mpv color enums)
  include/mpv/render_gl.h      (existing) OPENGL_INIT_PARAMS / OPENGL_FBO
  include/mpv/render_d3d11.h   D3D11_INIT_PARAMS / D3D11_TEX
  include/mpv/render_vulkan.h  VULKAN_INIT_PARAMS / VULKAN_TEX

video/out/vo_libmpv.c::render_backends[]      (+1 line: register render_backend_gpu_next)
  └─ video/out/gpu_next/libmpv_gpu_next.{c,h}  render_backend_fns "render_backend_gpu_next"
        dispatches over context_backends[]:
          ├─ libmpv_pl_context_gl     (video/out/opengl/libmpv_pl_gl.c)   pl_opengl_*  [+ra_ctx]
          ├─ libmpv_pl_context_d3d11  (video/out/d3d11/libmpv_pl_d3d11.c) pl_d3d11_*
          └─ libmpv_pl_context_vulkan (video/out/vulkan/libmpv_pl_vulkan.c) pl_vulkan_*
        and calls into:
  └─ video/out/gpu_next/core.{c,h}     the shared libplacebo renderer (NEW; ex-vo_gpu_next.c)
        consumed unchanged by BOTH render_backend_gpu_next AND vo_gpu_next.c
```

`struct libmpv_pl_context_fns` (in `libmpv_gpu_next.h`) is the per-API contract:
`api_name`, `init`, `wrap_fbo`, **`acquire_target` (optional, new in V2.1)**,
`done_frame`, `destroy`. A context exposes `pl_log`/`pl_gpu` (consumed by
`gpu_next_core_create`) and optionally `ra_ctx`/`ra` (for hwdec — only the GL
backend provides these).

---

## 4. File-by-file walkthrough

**New:**
- `video/out/gpu_next/core.{c,h}` — the extracted libplacebo renderer: pl_options
  / pl_renderer / pl_queue ownership, the render call (`gpu_next_core_render_mix`
  / `_render_image`), SW-upload + hwdec frame mapping, shader/ICC caches, render-
  option resolution, overlays, target-colorspace finalization, screenshot. API-
  and front-end-agnostic via a `gpu_next_core_frontend` struct (opts, ra,
  optional timer/hwdec hooks).
- `video/out/gpu_next/libmpv_gpu_next.{c,h}` — `render_backend_gpu_next` (the
  render-API front-end) + the `libmpv_pl_context_fns` interface + the
  context_backends[] table + the P3.1 enum→`pl_color_space` translation.
- `video/out/opengl/libmpv_pl_gl.c` — GL surface backend: `pl_opengl_create` /
  `pl_opengl_wrap`, **plus** a blank `ra_ctx` via `ra_gl_ctx_init` (F1.2) so the
  hwdec registry accepts it.
- `video/out/d3d11/libmpv_pl_d3d11.c` — D3D11 surface backend (P2.1).
- `video/out/vulkan/libmpv_pl_vulkan.c` — Vulkan surface backend (V2.1).
- `include/mpv/render_d3d11.h`, `include/mpv/render_vulkan.h` — public headers.

**Modified:**
- `video/out/vo_gpu_next.c` — **+127/−2164**, pure extraction; now a thin
  front-end over `gpu_next/core`.
- `include/mpv/render.h` — enum + API-type-string additions (all appended at the
  end; ABI-safe).
- `include/mpv/client.h` — `MPV_CLIENT_API_VERSION` 2.5 → 2.7.
- `DOCS/client-api-changes.rst` — 2.6 + 2.7 entries.
- `meson.build` — gate the three context files (see §6).
- `video/out/vo_libmpv.c` (+1: register `&render_backend_gpu_next` first in
  `render_backends[]`), `video/out/libmpv.h` (+1: `extern` decl).

---

## 5. Public API surface (ABI notes)

- New `MPV_RENDER_API_TYPE_*` **string** macros: `"pl-opengl"`, `"pl-d3d11"`,
  `"pl-vulkan"`. (pl-opengl shares `render_gl.h`'s init/FBO params with the
  legacy `"opengl"`.)
- New `mpv_render_param_type` enumerators **appended** after `SW_POINTER=20`:
  `TARGET_COLORSPACE=21`, `D3D11_INIT_PARAMS=22`, `D3D11_TEX=23`,
  `VULKAN_INIT_PARAMS=24`, `VULKAN_TEX=25`. Appending is ABI-safe (only the `0`
  terminator's value is guaranteed).
- `MPV_RENDER_PARAM_TARGET_COLORSPACE` → `mpv_render_param_target_colorspace`
  (`mpv_color_primaries`, `mpv_color_transfer`, `mpv_hdr_metadata`). **mpv-side
  enums, deliberately a complete superset of libplacebo's** with *independent
  numeric values*; P3.1 translates via explicit `switch`, so a libplacebo enum
  reshuffle cannot change the mpv ABI. `pl_color_repr` (system/levels/bits) is
  intentionally NOT in the struct — derived from `MPV_RENDER_PARAM_DEPTH` + the
  texture format.
- `render_d3d11.h`: `mpv_d3d11_init_params{device}` (void*), `mpv_d3d11_tex{tex,
  w,h}` (void*) — void* for header portability, mirroring `render_gl.h`.
- `render_vulkan.h`: `mpv_vulkan_init_params` (mirrors `pl_vulkan_import_params`:
  instance/get_proc_addr/phys_device/device/queues/extensions/features) and
  `mpv_vulkan_tex` (image/w/h/format/usage + **the external-sync contract**:
  acquire/release semaphores + current/final layout). Uses real Vulkan types
  (`<vulkan/vulkan_core.h>`), as a Vulkan header must.
- **Version:** combined work ships **API 2.7** (soname `libmpv.so.2.7.0`).

---

## 6. Per-backend detail + sync models

**Common render flow** (`render_backend_gpu_next::render`): `wrap_fbo` →
`acquire_target` (if set) → read `DEPTH`/`FLIP_Y`/`TARGET_COLORSPACE` → build a
swapchain-free `pl_frame` target → `gpu_next_core_apply_target_options` /
`_finalize_target_csp` / overlays / crop → `gpu_next_core_render_mix` →
`pl_gpu_flush` → `done_frame`. `get_target_size` calls `wrap_fbo` separately
(sizing only) — which is why the Vulkan acquire/hold is bracketed by
`acquire_target`+`done_frame` (render-only), **not** by `wrap_fbo`.

**GL** (`libmpv_pl_gl.c`): `pl_opengl_create(allow_software=true)` +
`pl_opengl_wrap` of the host FBO (`iformat = fbo->internal_format`). `done_frame`
no-op (host presents). Also builds a blank `ra_ctx` (`ra_gl_ctx_init`) → **hwdec
works** here (subject to §9 #1).

**D3D11** (`libmpv_pl_d3d11.c`): `init` reads `D3D11_INIT_PARAMS`, checks
feature level ≥ 11_0, `pl_d3d11_create(device)`. `wrap_fbo` reads `D3D11_TEX`,
destroys the prior wrap, `pl_d3d11_wrap`. `done_frame` no-op. `destroy`: wrap →
`pl_d3d11_destroy` → `pl_log_destroy`. No `ra_ctx` → **SW decode only** (P2.2).
Host texture must be `USAGE_DEFAULT`, single-sample, single-mip,
`BIND_RENDER_TARGET` (+`UNORDERED_ACCESS` for compute) — documented in the
header.

**Vulkan** (`libmpv_pl_vulkan.c`): `init` does `pl_vulkan_import` of the host's
existing device (never creates one). A wrapped `VkImage` is externally
synchronized, so the per-frame handshake is the one architectural addition to
the shared interface — the optional `acquire_target` hook (NULL for GL/D3D11, so
their paths are byte-unchanged):
- `wrap_fbo` → `pl_vulkan_wrap` (image starts "held by user").
- `acquire_target` → `pl_vulkan_release_ex` (hand to libplacebo; wait
  `acquire_sem`; treat as `current_layout`); stash `final_layout`/`release_sem`.
- `done_frame` → `pl_vulkan_hold_ex` (hand back in `final_layout`; fire
  `release_sem`). `destroy` also holds if a render aborted mid-handshake.
- `qf = VK_QUEUE_FAMILY_IGNORED` (concurrent on the shared imported device). No
  `ra_ctx` → **SW decode only**.

**meson gating** (matches the existing `libmpv_pl_gl.c` / `pl_has_opengl`
pattern):
- d3d11: inside `features['d3d11']`, `if pl_has_d3d11 == '1'`.
- vulkan: inside `features['vulkan']`, `if pl_has_vulkan == '1'`.
The C guards in `libmpv_gpu_next.c` mirror these (`#if HAVE_X &&
defined(PL_HAVE_X)`). **No single CI host builds all three** (d3d11 = Windows,
vulkan = Linux/cross); the context_backends[] table compiles whichever are
present and is `{NULL}`-safe when none are.

---

## 7. Validation — what was tested, how, results

Per-commit, every commit:
- **Windowed no-regression (mandatory):** lavapipe (software Vulkan) 12-case
  golden — PNG + a curated colorspace/HDR/render-pass JSON sidecar — byte-
  identical to pristine `35ae76d`. Plus a 9-case teardown/lifecycle check.
- **Render-API `pl-opengl` no-regression:** a surfaceless-EGL/llvmpipe harness
  renders into a host FBO + reads back; self-baselined byte-stable (the render
  path is pl-over-GL while the windowed golden is pl-over-Vulkan → not
  cross-comparable, so self-baseline).

Backend functional proofs (deterministic software oracles, self-baselined):
- **D3D11:** WARP (`D3D_DRIVER_TYPE_WARP`, FL11_1) harness — renders + reads
  back R8G8B8A8 (SDR) and R10G10B10A2 (HDR10/PQ). SDR byte-stable; HDR genuine
  PQ (decoded peak in HDR range, 0.92 luma-correlation with the SDR render =
  same image), `TARGET_COLORSPACE` sRGB no-op + BT.2020/PQ effect proven.
- **Vulkan:** lavapipe harness (self-contained device w/
  `pl_vulkan_required_features` + recommended extensions; image→buffer readback
  through the release semaphore) — 4 cases byte-stable. **Stronger oracle than
  WARP** (same software Vulkan the windowed golden uses).

Real-hardware (NVIDIA D3D11 HDR display, HDR mode on) — **Plan 2 Phase 4b/5a:**
- Windowed real-HDR no-regression byte-identical to pristine at the Plan-2 tip,
  **both SW and HW (d3d11va) decode** (PNG `46CDAFFF…`).
- Render-API D3D11 HDR: `video-out-params` **bit-identical** to the windowed
  swapchain (bt.2020/pq, max-luma 1000, sig-peak 4.926108, min-luma 0.005);
  actual render genuine PQ + deterministic; and a bare-Win32 HDR-swapchain host
  (`rapi_hdr_present.c`) **visually matches windowed** on the display.

Validation harnesses/hosts live in the git-excluded `_golden/render_api/`
(WARP/lavapipe/EGL) and the Windows worktree; they are per-developer infra, not
proposed for upstream (mirrors how mpv keeps such rigs out of tree).

---

## 8. Deviations from the written plans

1. **Plan 1's "W5 is mechanical wiring" assumption was wrong** → replaced by the
   **E-phase** (extract `draw_frame`'s orchestration into the core rather than
   duplicate ~150 lines). Result is cleaner (zero duplication); documented in
   `plan1-gpu-next/HANDOFF.md`.
2. **hwdec was an undocumented gap in Plan 1's original spec** → filled by the
   **F-phase** (the GL backend builds a blank `ra_ctx`; the core's existing
   hwdec mapper is reused). Plan-1 completeness statement in
   `plan1-gpu-next/plan.md`.
3. **Plan 2 P2.2 (the hwdec-registry gate) was not in the written plan** — it is
   a runtime fix the WARP harness surfaced: the render backend unconditionally
   called `ra_hwdec_ctx_init` (which `mp_assert`s `ra_ctx`), aborting the first
   real D3D11 context. The fix gates the registry on `ra_ctx`, which is the
   correct shape regardless (hwdec interops are ra-typed). Compile-only validation
   couldn't catch it.
4. **Plan 2 V2.1 added one shared-interface hook (`acquire_target`)** not
   anticipated by the plan, because a wrapped `VkImage` needs an explicit
   acquire that D3D11 doesn't. It is optional/NULL for GL+D3D11 → no behavior
   change for them.
5. **Phase 4b's "bit-exact PNG vs windowed" gate was reframed.** The windowed
   path never exposes its HDR swapchain backbuffer for readback (its screenshot
   tone-maps to sRGB), so a backbuffer-vs-backbuffer compare is unavailable;
   `video-out-params` identity + the on-display visual are the equivalent proof.
   See `plan2-hdr-render-api/hdr-phase4-assessment.md` §2.
6. **macOS = MoltenVK via the existing `pl-vulkan` backend** (the plan offered
   "metal or MoltenVK"); native Metal is deferred (§9).

---

## 9. Deferred / known limitations — the audit must-knows

1. **render-API hwdec is GL-only.** The D3D11 and Vulkan backends expose only a
   `pl_gpu` (no `ra_ctx`) and therefore run **software-decode only** (P2.2). The
   plan defers d3d11va/vulkan render-API hwdec to a later phase; it needs the
   host's decode device threaded through and a real-GPU harness. **Not done.**
2. **GL render-API hwdec lacks Linux native-resource forwarding** (issue #1 in
   `plan1-gpu-next/GPU_NEXT_VALIDATION_ISSUES.md`):
   `render_backend_gpu_next` does not mirror `libmpv_gpu.c`'s
   `ra_add_native_resource()` loop (X11/Wayland/DRM display handles), so Linux GL
   hwdec interops (VAAPI/VDPAU/DRM-PRIME) are likely incomplete. **Windows
   D3D11/EGL+CUDA unaffected.** A bounded fix; not started.
3. **`MPV_CLIENT_API_VERSION` / PR-split gap.** The combined branch is coherent
   at 2.7. But the **Plan-1-only** branch adds `MPV_RENDER_API_TYPE_PL_OPENGL`
   while still at **2.5 with no rst entry** (the 2.6 bump lives in Plan-2's
   P1.1). **If Plan 1 is submitted as a separate PR, add a 2.6 bump + rst entry
   for PL_OPENGL to it**, then renumber Plan-2's bumps. For a combined PR, no
   action. (Left as-is — a maintainer PR-strategy decision, §10.)
4. **render-API screenshot differs from windowed** (minor, intentional,
   commented). The backend's screenshot hook passes `fallback_depth=0,
   want_alpha=false` (no swapchain, no ra_ctx) vs the windowed VO's swapchain
   depth + `want_alpha` option. Empirically, `screenshot-to-file "video"` of HDR
   content comes out **native BT.2020/PQ (rgb48, no alpha)** on the render API vs
   **tone-mapped sRGB (rgba64)** windowed. This is a screenshot-feature
   difference, **orthogonal to the display render** (which matches). If upstream
   wants parity, the render-API screenshot should accept a host-supplied depth +
   alpha intent — small, but out of the HDR-render scope.
5. **Native Metal deferred.** libplacebo's `metal.h` ships **only on macOS**
   (absent on the Windows MSYS2 + WSL rigs; `pl_has_metal` unset), so a
   `pl_metal_*` backend cannot be authored/compiled/gated here. macOS is covered
   today via MoltenVK + `pl-vulkan` (no new code). A native backend would reuse
   the `acquire_target` hook (via `MTLSharedEvent`); Mac-resident task.
6. **Production host shell (Plan 2 Phase 5b)** is the user's application, out of
   tree. The bare-Win32 host `rapi_hdr_present.c` is the reference call sequence;
   the load-bearing host requirement it surfaced: **the host must query the
   display peak (`IDXGIOutput6::GetDesc1().MaxLuminance`) and pass it in
   `TARGET_COLORSPACE`** — not the content mastering peak — or HDR is
   over-saturated. (Host-side; validates the API design.) See
   `hdr-phase5-assessment.md`.
7. **Commit hygiene for upstream** (issue #4): commits carry `Co-Authored-By`
   trailers; squash/relabel per upstream policy. No `Change-Id`/noise otherwise.

---

## 10. Merge-strategy guidance

- **Two PRs vs one.** Cleanest is **Plan 1 first** (GL render API — a complete,
  self-contained feature: the core extraction + `render_backend_gpu_next` +
  `pl-opengl` + F-phase hwdec), **then Plan 2** (D3D11 + Vulkan + TARGET_COLORSPACE
  on top). If split, fix §9 #3 (version bump on the Plan-1 PR). A single combined
  PR is also coherent at 2.7.
- **The core extraction (≈30 commits) is the reviewer-heavy part** but is pure
  code-motion, each commit windowed-golden bit-identical — reviewable as "did
  this commit change any pixel?" (answer: no, by construction).
- **Rebase onto `upstream/master` before submission.** The conflict surface is
  dominated by `vo_gpu_next.c`, which mainline rarely touches; prior rebases were
  near-free. `DOCS/client-api-changes.rst` is the one file that reliably
  conflicts (everyone appends to the top).
- **What's genuinely new C the auditor must read** (everything else is motion or
  trivial): `gpu_next/core.{c,h}` (the extracted renderer — verify nothing
  changed semantically vs the old `vo_gpu_next.c`), `libmpv_gpu_next.{c,h}` (the
  render hook + interface + P3.1 translation), and the three `libmpv_pl_context_*`
  files (gl 156 / d3d11 135 / vulkan 187 LOC). The headers are declarative.
  (`core.c` 2605, `core.h` 669, `libmpv_gpu_next.c` 589 / `.h` 94, `vo_gpu_next.c`
  now 688.)

---

## 11. Auditor checklist

- [ ] **Core extraction is semantics-preserving.** Diff `gpu_next/core.c`
  against the pre-extraction `vo_gpu_next.c` bodies; confirm pure motion.
- [ ] **Enum additions are append-only** in `render.h` (ABI). ✔ verified here.
- [ ] **mpv color enums are a superset of libplacebo's** and P3.1's `switch`
  translation is exhaustive (compile-warns on a missing case via the enum
  `switch` with no `default`). Cross-check `render.h` vs `libplacebo/colorspace.h`.
- [ ] **`acquire_target`/`done_frame` pairing** in the Vulkan backend: every
  `release_ex` is matched by a `hold_ex` (incl. the error path and `destroy`).
- [ ] **Destroy ordering** in each backend: core (which owns objects on
  `ctx->gpu`) torn down before the context that owns the gpu; `pl_*_destroy`
  before `pl_log_destroy`.
- [ ] **meson gates** compile-out cleanly where `pl_has_{d3d11,vulkan}=0` (the
  C `#if` guards match the meson `if`).
- [ ] **No new compiler warnings** on the three backends + the render hook
  (builds were clean on UCRT64 + lavapipe Linux).
- [ ] **Decide §9 #1–#7** disposition (fix-now vs document-as-known) per the
  chosen merge scope.

---

*Per-phase deep-dives:* `_refactor/plan1-gpu-next/{plan.md,HANDOFF.md,
GPU_NEXT_VALIDATION_ISSUES.md}` (Plan 1), `_refactor/plan2-hdr-render-api/
{plan-hdr-render-api.md,hdr-phase{0,1,2,3,4,5,6}-assessment.md}` (Plan 2).
Live state: `_refactor/INDEX.md`.
