# Plan: HDR over the libmpv render API (host-agnostic D3D11/Vulkan/Metal context-fns + TARGET_COLORSPACE param, post-W6)

> **First execution action (post-approval):** write this document verbatim
> to `c:\DEV\ai-dev\projects\mpv-src\plan-hdr-render-api.md`. It is the
> working plan for the attempt; this plan-file copy is the approved spec.
>
> **This plan is intentionally engine-agnostic.** The mpv-side work
> (Phases 0-4) is identical regardless of the chosen production host
> shell. Host options are catalogued in
> [C:\DEV\player-architecture-options.md](C:\DEV\player-architecture-options.md);
> Phase 5b consumes one of those options for the demonstration host.

## Context

The W5-6 + W6 refactor exposed gpu-next through the libmpv render API as
`MPV_RENDER_API_TYPE_PL_OPENGL`, validated bit-faithfully against the
windowed `--vo=gpu-next` path on real D3D11 NVIDIA HDR hardware
(`c8e9298`, both SW and HW decode; PNG `46CDAFFF…` + JSON `7712D507…`/
`E70ED9BE…`). But the GL render path is structurally HDR-incapable:

- The render backend builds the `pl_frame` target manually from the
  host's wrapped FBO. There is no `pl_swapchain` to read `target_csp`
  from and no `set_colorspace_hint` to negotiate against.
- `mpv_render_param` has `DEPTH` and `FLIP_Y` but nothing for "my
  surface is BT.2020/PQ with peak 1000 nits."
- The host's GL FBO is in practice `GL_RGBA8` (Qt's `QOpenGLWidget`,
  Avalonia's `OpenGLControl`, WinUI 3 / WPF GL interop, custom
  WGL/EGL hosts) — 8 bits per channel, no HDR luminance headroom —
  and no standard GL extension makes "HDR-capable target" observable
  to the user.

So any host using `mpv_render_context` today gets gpu-next quality
on the SDR side but tone-maps HDR back to sRGB at the FBO boundary.
Same ceiling `vo_gpu` had via the render API.

This plan closes that gap. It adds **two host-agnostic** things to mpv:

1. New per-API surface-wrap layers: `libmpv_pl_context_d3d11.c`
   (Windows), then `libmpv_pl_context_vulkan.c` (Linux + cross-
   platform Win), then `libmpv_pl_context_metal.c` (macOS) —
   parallel to `libmpv_pl_context_gl.c`. Each wraps a host-provided
   GPU texture (D3D11/Vulkan/Metal) as a `pl_tex` via libplacebo's
   matching backend.
2. A new public render-API param `MPV_RENDER_PARAM_TARGET_COLORSPACE`
   that lets the host tell mpv "my target surface is HDR with these
   characteristics" per frame — orthogonal to which graphics API.

No changes to `gpu_next_core`; no changes to the
`render_backend_gpu_next::render` hook structure (just one new param
read at the top); the maintainer-blessed shape (`render_backend_fns` +
per-API context-fns) is preserved end-to-end.

Endpoint of this plan: a **minimal verification host** (bare C++,
~200 LOC) demonstrates the end-to-end pipeline rendering a 4K HDR10
clip into a DXGI HDR swapchain, color-matched to the windowed-path
baseline. Optionally a second sample in the user's chosen production
host shell (Qt 6.7+ QQuickRhiItem / WinUI 3 SwapChainPanel / etc.)
proves the same API works there.

## Endpoint & guardrails (locked with user)

- **Endpoint:** a working HDR-over-render-API path on the user's
  D3D11 + NVIDIA HDR rig (Windows-first), integrable into any host
  shell. Mergeable-quality personal fork; upstream submission is
  bonus, **not** a success gate (same shape as the W5-6 endpoint).
- **No-regression bar (mandatory):**
  - W5-6+W6 paths — windowed `--vo=gpu-next` and render-API via
    `PL_OPENGL` — must stay bit-identical to the current sign-off
    (lavapipe 12-case golden + render-API harness v2 self-baseline +
    `run-hdrval.ps1` SW+HW on `c8e9298`).
  - SDR rendering via `PL_D3D11` (no TARGET_COLORSPACE) must match
    SDR via `PL_OPENGL` to within GPU noise on the same hardware
    (same libplacebo, different surface backend; expect identical).
- **HDR fidelity bar (the project's real go/no-go):** rendering a 4K
  HDR10 clip via the new render-API path into a D3D11 HDR swapchain
  must produce the same pixels (within base1==base2 noise floor) as
  the windowed `--vo=gpu-next` path on the same display + same target
  options. This is the Phase 4 gate. If it cannot be made faithful,
  abandon to the fallback (the user keeps using `--wid=HWND`).
- **Time-box (hard):** execute **Phase 0 + Phase 1 + Phase 2** then
  STOP at the Reassessment Gate. Phases 3-5 only after explicit
  re-decision (parallel to W5-6's discipline).
- **Slow, steady, manual, meticulous, golden-gated per commit.**

## Target architecture (the maintainer-blessed shape, extended)

The W5-6 + W6 architecture is already the right two-layer shape; this
plan only extends the per-API surface-wrap layer and adds one render
param:

```
public API (mpv/render.h, mpv/render_d3d11.h):
  MPV_RENDER_API_TYPE_PL_D3D11        = "pl-d3d11"            ; NEW
  MPV_RENDER_PARAM_D3D11_INIT_PARAMS  → mpv_d3d11_init_params* ; NEW
  MPV_RENDER_PARAM_D3D11_TEX          → mpv_d3d11_tex*         ; NEW
  MPV_RENDER_PARAM_TARGET_COLORSPACE  → orthogonal, host-side  ; NEW

video/out/libmpv.h:                          (unchanged)
  struct render_backend_fns

video/out/vo_libmpv.c::render_backends[]:    (unchanged — pl-opengl
                                              and pl-d3d11 share one
                                              render_backend_gpu_next)

video/out/gpu_next/libmpv_gpu_next.{c,h}:    (W5-6 — render hook unchanged
                                              apart from one new param
                                              read at the top of render())
  ├─ libmpv_pl_context_gl   ──→ pl_opengl_create / pl_opengl_wrap  (W5-6)
  └─ libmpv_pl_context_d3d11──→ pl_d3d11_create  / pl_d3d11_wrap   ; NEW

video/out/gpu_next/core.{c,h}:               (unchanged)
  gpu_next_core_apply_target_options + finalize_target_csp already
  honor whatever target.color the front-end pre-populated.
```

`render_backend_gpu_next::render` reads `TARGET_COLORSPACE` (if
present), maps it to the corresponding `pl_color_space`, and writes it
to `target.color` BEFORE `gpu_next_core_apply_target_options` runs.
The existing `--target-prim/--target-trc/--target-peak` opts logic
then refines or overrides it, just as it does for the swapchain-
negotiated colorspace in the windowed path.

## Phases

### Phase 0 — AI feasibility check

Dispatch an AI agent (read-only, against the WSL fork at HEAD
`c8e9298`, the libplacebo `d3d11.h` headers, mpv's render-API docs,
and Qt 6 / RHI documentation) to deliver, in writing:

1. **libplacebo D3D11 audit.** Confirm `pl_d3d11_create()` +
   `pl_d3d11_wrap()` actual signatures, required device feature level,
   driver requirements, and any state expectations on the wrapped
   `ID3D11Texture2D` (e.g., must it be `D3D11_USAGE_DEFAULT`?
   `D3D11_BIND_RENDER_TARGET`?). Identify version constraints vs the
   in-tree libplacebo 7.360.1.

2. **mpv render-API ABI hygiene.** Confirm extending the
   `MPV_RENDER_API_TYPE_*` enum and `MPV_RENDER_PARAM_*` enum is
   ABI-compatible (mpv's render API is C ABI; adding enumerators to
   the end is generally safe — verify by re-reading the public-API
   policy). Check how `PL_OPENGL` was added in W5-6 for the
   convention.

3. **Host HDR surface feasibility — side-by-side audit.** The
   mpv-side work is host-agnostic; confirm at least one viable
   integration path exists for each candidate host the user might
   pick (see player-architecture-options.md for the full
   catalogue). For each: how does the host hand mpv an
   HDR-capable `ID3D11Texture2D` (or Metal `MTLTexture` / Vulkan
   `VkImage`) per frame, and how does it expose / negotiate the
   target colorspace? Specifically audit:
   - **WinUI 3** via `SwapChainPanel` +
     `ISwapChainPanelNative::SetSwapChain` (D3D11 HDR swapchain
     hosted in XAML tree; per-visual color via Composition/DComp).
   - **Qt 6.7+** via `QQuickRhiItem` (RhiItem renders into a RHI
     texture, native handle exposed via `QRhi::nativeHandles()`).
   - **Qt 6.x** via custom `QWindow` + manual DXGI swapchain on
     `winId()` HWND.
   - **Avalonia** via `NativeControlHost` hosting an HWND/NSView
     with native D3D11/Metal swapchain.
   - **Bare Win32 / DComp** with composition swapchains
     (`CreateSwapChainForComposition`) for the verification host.
   - **Electron + native `--wid` (status quo)** noted as the
     "does not need this plan" fallback.
   Recommend the production-host path most likely to succeed for
   the user's stated goals and identify version constraints.

4. **TARGET_COLORSPACE design.** Propose the exact shape of
   `struct mpv_render_param_target_colorspace`. Mirror libplacebo's
   `pl_color_space` semantics (primaries enum, transfer enum, HDR
   metadata) but expose mpv-side enums — the render API must not
   leak libplacebo types. Match the enum values mpv already exposes
   via the `video-params` property (`primaries`/`gamma`). Decide
   whether `pl_color_repr` (system/levels/bits) belongs in the
   struct or is derived from the texture format.

5. **Top-5 no-regression hazards** for adding the D3D11 path:
   - HDR metadata signaling when the host reconfigures the swapchain
     mid-playback (HDR display toggled, resolution change).
   - Color management ordering: when the host supplies
     TARGET_COLORSPACE *and* user has `--target-trc=pq` set — which
     wins? (Spec: same precedence as the windowed swapchain-hint
     path; user opts override the front-end-supplied baseline only
     when explicitly set.)
   - Hwdec interop on the D3D11 backend (the SW path is simpler;
     d3d11va → libplacebo wrap should be cheap via shared D3D11
     device, but verify).
   - DPI scaling and resize: the host's DXGI swapchain may resize
     while mpv has a wrapped `pl_tex` from a stale backbuffer.
   - Threading: `mpv_render_context_render` must be called on the
     same thread as the host's D3D11 immediate context (or with
     explicit synchronization). Verify mpv's render-API threading
     model intersects cleanly.

6. **Go/No-Go + confidence (low/med/high)** with the single biggest
   unknown. Same gate shape as `plan.md`'s Phase 0.

**Gate:** confidence ≥ medium AND no kill-points → proceed to Phase
1. If the host-side answer is "no robust path exists in any
candidate host," **stop and take the fallback** (keep using
`--wid=HWND` on the windowed path — HDR works there today, proven).

### Phase 1 — Public API + scaffolding (additive, no behavior change)

1. **Public-API additions** (`include/mpv/render.h` +
   `include/mpv/render_d3d11.h`):
   - `MPV_RENDER_API_TYPE_PL_D3D11` (= `"pl-d3d11"`) in `render.h`.
   - `MPV_RENDER_PARAM_TARGET_COLORSPACE` enumerator in `render.h`
     plus the `struct mpv_render_param_target_colorspace` definition
     (final fields per Phase 0).
   - New header `mpv/render_d3d11.h` parallel to `render_gl.h`:
     `MPV_RENDER_PARAM_D3D11_INIT_PARAMS` + `_D3D11_TEX`, plus the
     `mpv_d3d11_init_params` / `mpv_d3d11_tex` structs.
   - Documentation comments matching `render_gl.h`'s tone.

2. **Empty `libmpv_pl_context_d3d11.c` scaffolding** (W1-style):
   declares the `libmpv_pl_context_fns` const struct with all
   functions stubbed (init returns `MPV_ERROR_NOT_IMPLEMENTED`,
   wrap_fbo returns same, destroy is no-op). Not yet registered in
   `context_backends[]`. Builds clean behind `#if HAVE_D3D11 &&
   defined(PL_HAVE_D3D11)`.

3. **Empty handling for `MPV_RENDER_PARAM_TARGET_COLORSPACE`** in
   `render_backend_gpu_next::render`: reads it into a local but
   doesn't yet apply it (the data path is wired in Phase 3). This
   is purely a code-organization commit; behavior unchanged.

4. **Meson:** gate `libmpv_pl_d3d11.c` on `features['d3d11']` AND
   libplacebo's `pl_has_d3d11`.

**Gate:** all existing gates green (lavapipe 12-case golden +
render-API harness v2 self-baseline + `--probe`). Pure-additive diff;
no semantic change. `git diff --stat` minimal, no comment pollution
(Dudemanguy bar).

### Phase 2 — D3D11 context-fns: init / wrap_fbo

Flesh out `libmpv_pl_context_d3d11.c`:

1. `init(ctx, params)`:
   - Read `MPV_RENDER_PARAM_D3D11_INIT_PARAMS` for `ID3D11Device *`.
   - Build a `pl_d3d11_params` from the host's device and call
     `pl_d3d11_create(...)`. On failure log + return
     `MPV_ERROR_UNSUPPORTED`.
   - Store the `pl_d3d11` in `ctx->priv`, populate `ctx->pllog` +
     `ctx->gpu`.

2. `wrap_fbo(ctx, params, out_tex)`:
   - Read `MPV_RENDER_PARAM_D3D11_TEX` for `ID3D11Texture2D *`.
   - Destroy any previously wrapped texture (`pl_tex_destroy`).
   - Wrap via `pl_d3d11_wrap(ctx->gpu, &(struct pl_d3d11_wrap_params)
     { .tex = host_tex, … })`.

3. `done_frame`: no-op (host owns presentation).

4. `destroy`: tear down wrap_fbo state, `pl_d3d11_destroy(&priv->
   pl_d3d11)`, `pl_log_destroy(&ctx->pllog)`.

5. Register `&libmpv_pl_context_d3d11` in `libmpv_gpu_next.c::
   context_backends[]` behind the `#if HAVE_D3D11 &&
   defined(PL_HAVE_D3D11)` guard. The render backend now matches
   `"pl-d3d11"` as well as `"pl-opengl"`.

6. Extend the render-API harness with a D3D11 mode
   (`_golden/render_api/rapi_harness_d3d11.c` or a `--d3d11` flag
   on the existing harness): the host creates an SDR D3D11 device
   + swapchain (`R8G8B8A8_UNORM`, sRGB color space), drives mpv
   through `MPV_RENDER_API_TYPE_PL_D3D11`, reads the backbuffer
   back, sidecar with format + colorspace tag.

**Gate:**
- `rapi_run.sh --d3d11 --baseline` then `rapi_run.sh --d3d11`
  re-verifies byte-identical (self-baselined). Software D3D11 (WARP)
  is deterministic run-to-run on one host.
- The existing harness modes (`--probe`, render via PL_OPENGL) +
  lavapipe windowed golden stay byte-identical (no regression).

### ⛔ REASSESSMENT GATE (mandatory hard stop)

Do not proceed without a written go/no-go answering: did Phase 0
return ≥ medium confidence? Did Phase 1 land additively with no
regression? Did the Phase-2 D3D11 SDR path produce visually correct
output (manual eyeball) AND self-baseline byte-stably? Is the
target-colorspace design clear enough to lock in Phase 3?
Estimated effort for Phase 3 + Phase 4 + Phase 5 (verification
host) vs the fallback cost? **Default = stop and take a fallback
unless all green.**

Everything below is *post-gate* and not part of the time-box.

### Phase 3 — Wire TARGET_COLORSPACE into the render hook

1. In `render_backend_gpu_next::render`:
   - Read `MPV_RENDER_PARAM_TARGET_COLORSPACE`. If present, map the
     mpv-side enums to libplacebo equivalents and write the
     resulting `pl_color_space` into `target.color` BEFORE
     `gpu_next_core_apply_target_options` runs.
   - When absent, the existing `pl_color_space_srgb` default stands
     → `PL_OPENGL` SDR path remains backwards-compatible.

2. Decide the precedence semantics for `gpu_next_core_apply_target_
   options`' `hint` parameter when TARGET_COLORSPACE is supplied.
   Mirror the windowed-path behavior: host-supplied is the
   swapchain-equivalent baseline, user `--target-*` opts override
   only when explicitly set. Pass `hint = true` to
   `apply_target_options` when TARGET_COLORSPACE was supplied (and
   `hint = false` when not — preserves PL_OPENGL behavior).

3. Plumb `target_csp` + `target_unknown` to `finalize_target_csp`
   from the host-supplied data when present (drives the Windows
   `target_pq` logic in particular). When not, keep current
   behavior (`target_csp = {0}`, `target_unknown = true`).

**Gate:**
- `PL_OPENGL` SDR path: byte-identical to pre-Phase-3 (no
  TARGET_COLORSPACE = original sRGB default).
- `PL_D3D11` SDR with TARGET_COLORSPACE=sRGB: byte-identical to
  Phase-2 self-baseline (no-op default).
- `PL_D3D11` SDR with TARGET_COLORSPACE=BT.2020/HLG: rendered
  output reflects the requested target (manual visual check;
  HDR-on-SDR-display will look washed/clamped, that's expected).

### Phase 4 — HDR validation (the project's true go/no-go)

The real kill-point. If this fails, the whole render-API HDR path
is rejected and the fallback (`--wid=HWND` windowed) is the
permanent answer.

1. Extend the D3D11 harness for HDR mode:
   - `DXGI_FORMAT_R10G10B10A2_UNORM` swapchain, `IDXGISwapChain3::
     SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)`.
   - Pass `MPV_RENDER_PARAM_TARGET_COLORSPACE = {BT.2020, PQ,
     max_luma=1000, max_cll=1000, max_fall=400, ...}` per frame
     (mirroring the windowed swapchain-negotiated values from the
     existing HDR validation).
   - Render the same 4K HDR10 clip used by `run-hdrval.ps1`;
     capture the backbuffer.

2. Build a `run-hdrval-rapi.ps1` script (parallel to
   `run-hdrval.ps1`) that does the 3-run base1/base2/ref dance.
   Reference = the existing windowed `vo_gpu_next` rendering on the
   SAME display.

3. Compare:
   - PNG hashes: render-API path vs windowed should match within
     GPU noise (same NVIDIA, same libplacebo, same target options
     → same pixels).
   - Negotiated HDR metadata: tone-map range, primaries, peak
     should match the windowed JSON sidecar.

4. If divergence: root-cause whether it is in the pl_options
   resolution (target.color setup), the libplacebo render path
   (pl_d3d11 vs pl_d3d11-via-swapchain), or the host's swapchain
   colorspace negotiation. Fix in-place (Phase 4a sub-commit) and
   re-validate.

**Gate:** HDR clip renders bit-identical to the windowed-path
baseline on real D3D11 HDR hardware. **If not faithful, abandon to
the fallback.**

### Phase 5 — Sample hosts (host-agnostic verification + production)

#### Phase 5a — Minimal verification host (bare C++, deliverable)

A small standalone C++ sample (`samples/d3d11-hdr-rapi/`,
~200 LOC), deliberately framework-free so it proves the mpv API
in isolation. This is the *deliverable* part of Phase 5; the
production host (5b) is the user's separate application choice.

1. `main.cpp`: Win32 `WinMain`, raw `HWND` from `CreateWindow`,
   HDR detection (`IDXGIOutput6::GetDesc1()` →
   `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` support).
2. Manual `ID3D11Device` + `IDXGISwapChain1` on the HWND with
   `DXGI_FORMAT_R10G10B10A2_UNORM` and
   `IDXGISwapChain3::SetColorSpace1` to BT.2020/PQ.
3. `mpv_render_context_create` with
   `MPV_RENDER_API_TYPE_PL_D3D11` +
   `MPV_RENDER_PARAM_D3D11_INIT_PARAMS = { device }`.
4. Event loop: per `MPV_RENDER_UPDATE_FRAME` from the wakeup
   callback, `GetBuffer(0)` → `mpv_render_context_render` with
   `MPV_RENDER_PARAM_D3D11_TEX = { backbuffer }` +
   `MPV_RENDER_PARAM_TARGET_COLORSPACE = { BT.2020, PQ, ... }`
   → `Present(1, 0)`.
5. Subtitles via mpv's built-in OSD (HDR-aware via libplacebo
   `pl_overlay`). No host-side UI.

**Gate:** sample plays an HDR clip with visually correct color
reproduction on the user's HDR display. Tested at both SDR
(display HDR off → OS maps back) and HDR (display HDR on). Clean
exit; no leaks; resize works. **This is the mpv-side
deliverable** — it proves the API works without depending on any
specific UI framework.

#### Phase 5b — Production host integration (user's choice, separate effort)

Outside the time-box of *this* plan but listed for completeness.
Once 5a proves the API, integrate into the user's chosen
production host per
[player-architecture-options.md](C:\DEV\player-architecture-options.md):

- **WinUI 3 / Fluss path**: add `SwapChainPanel + mpv` backend
  alongside the existing `MediaPlayerElement` path.
- **Qt 6.7+ + QQuickRhiItem path**: render mpv into the
  RhiItem's RHI texture via the native-handles bridge.
- **Avalonia path**: `NativeControlHost`-based native control
  with the same D3D11 setup as 5a.
- **Hybrid path**: each platform-native shell consumes the same
  D3D11 (Windows) / Metal (macOS — needs Phase 6) / Vulkan
  (Linux — needs Phase 6) render-API integration.

5b is the user's application work; the mpv-side plan finishes at
5a.

### Phase 6 — Cross-platform backends (post-time-box; required for non-Windows)

These elevate from "optional polish" to "first-class" if the
production host targets macOS or Linux (host options 1, 3, 4):

- **Vulkan context-fns** (`libmpv_pl_context_vulkan.c`,
  `MPV_RENDER_API_TYPE_PL_VULKAN`) — required for Linux/Wayland
  HDR and for cross-platform Vulkan hosts on Windows. Same
  structural shape as the D3D11 implementation (Phase 2), but
  consuming the host's `VkInstance`/`VkDevice`/`VkQueue` and
  wrapping host-provided `VkImage` textures via `pl_vulkan_*`.
- **Metal context-fns** (`libmpv_pl_context_metal.c`,
  `MPV_RENDER_API_TYPE_PL_METAL`) — required for macOS HDR
  (Metal + EDR). Same shape, consuming `MTLDevice` and wrapping
  `id<MTLTexture>` via `pl_metal_*`.
- Gating: each new backend gets its own SDR self-baselined
  harness mode (parallel to Phase 2's D3D11 harness) before
  any HDR validation.

### Phase 7 — Optional polish / upstreaming

- Documentation: `DOCS/client-api-changes.rst` update for the
  new API types and the TARGET_COLORSPACE param.
- Upstreaming: prepare a clean diff for `mpv-player/mpv` if the
  user pursues the optional PR. Each context-fns backend can be
  upstreamed independently.
- API hygiene: deprecation path for `MPV_RENDER_PARAM_FLIP_Y` in
  HDR contexts (the new param's crop-based flip handling may
  supersede it).

## Critical files (verified in clone)

mpv (modify):
- `include/mpv/render.h` — add `API_TYPE_PL_D3D11` +
  `PARAM_TARGET_COLORSPACE` enumerators + the param struct.
- `meson.build` — gate the new file.
- `video/out/gpu_next/libmpv_gpu_next.c` — `context_backends[]` +
  `render()` reads the new param.

mpv (new):
- `include/mpv/render_d3d11.h` — public header, parallel to
  `render_gl.h`.
- `video/out/d3d11/libmpv_pl_d3d11.c` — the new context-fns
  implementation (~150 LOC, parallel to `libmpv_pl_gl.c`).

mpv (existing references):
- `video/out/opengl/libmpv_pl_gl.c` — the structural template.
- `video/out/gpu_next/libmpv_gpu_next.h` — the
  `libmpv_pl_context_fns` interface.
- `video/out/gpu_next/core.{c,h}` — unchanged; honors host-supplied
  target.color via the existing `apply_target_options` +
  `finalize_target_csp` path.

libplacebo:
- `libplacebo/d3d11.h` — `pl_d3d11_create` + `pl_d3d11_wrap` (Phase 0
  audits exact signatures).
- `libplacebo/vulkan.h` — Vulkan backend (Phase 6).
- `libplacebo/metal.h` — Metal backend (Phase 6).
- `libplacebo/colorspace.h` — colorspace enums the new mpv enums
  must map to.

Host (separate from this plan):
- See [player-architecture-options.md](C:\DEV\player-architecture-options.md)
  for the catalogue of candidate host shells. The mpv-side plan
  is the same regardless of the chosen host.

## Reuse (don't reinvent)

- `libmpv_pl_context_gl.c` is the structural template; copy and
  adapt for D3D11 / Vulkan / Metal.
- `mpv/render_gl.h` is the structural template for the new public
  headers (`render_d3d11.h`, `render_vulkan.h`, `render_metal.h`).
- libplacebo's existing `pl_<api>_*` functions do the heavy
  lifting on the backend side.
- libplacebo's own `plplay` sample is the canonical reference for
  each backend's DXGI / Vulkan / Metal HDR setup — study but
  don't copy.

## Verification

- **Per-commit golden gate** (mirroring W5-6 discipline): each
  commit golden-gated.
  - Lavapipe windowed 12-case golden for the SW path.
  - Render-API harness v2 self-baseline for `PL_OPENGL` no-regression.
  - New D3D11 SDR harness for the new path (Phase 2 onwards).
- **HDR fidelity gate (Phase 4):** `run-hdrval-rapi.ps1`-style
  three-run dance comparing render-API HDR vs windowed HDR on the
  same display.
- **Manual functional eyeball (Phase 5):** Qt sample, real clip
  playback on the user's HDR display, both display HDR on and off.
- **Reassessment Gate (after Phase 2):** mandatory hard stop, same
  shape as `plan.md`'s. Default = take a fallback unless all green.
- **Kill criteria are verification too:** failing Phase 4 is a
  *successful* outcome of the time-box — record findings, take a
  fallback (`--wid=HWND`).

## Fallbacks (explicit, no shame)

- **`--wid=HWND` on the windowed `vo_gpu_next` path.** The status
  quo: mpv owns the swapchain on an HWND the host provides. HDR
  works (validated through `c8e9298` on the user's rig); already
  in production for the Electron + DComp path (option 6) and
  trivially usable from any host that can give mpv an HWND. The
  cost vs the render-API path: mpv owns surface presentation
  (no fine-grained host control over compositing, no host-
  rendered overlays inside the HDR swapchain).
- **Vulkan instead of D3D11 on Windows.** Higher portability;
  unlocks Linux + macOS later. Same plan, swap `pl_d3d11_*` for
  `pl_vulkan_*` (Phase 6). Defer until D3D11 proves successful.
- **`--wid=HWND` + separate host-UI overlay window.** A hybrid:
  mpv owns the HDR video region (via the windowed path), the host
  owns a transparent child window above it for UI. Skips this
  entire plan; ships today; matches options 5 + 6 in
  player-architecture-options.md.

## References

- mpv `render_backend_gpu_next` (W5-6+W6 result, post-`c8e9298`).
- `plan.md` (the parent W5-6 spec, this same folder).
- `HANDOFF.md` (W5-6 status, this same folder).
- libplacebo D3D11 backend: `libplacebo/d3d11.h` + `plplay.c`'s
  D3D11 path.
- Qt 6 RHI: https://doc.qt.io/qt-6/qrhi.html.
- DXGI HDR (the Microsoft documentation):
  https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
- The HDR validation rig and runbooks in this folder
  (`run-hdrval.ps1`, `phase2-msys2-hdr-runbook.md`).
