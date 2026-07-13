# Plan 2 — Phase 5b assessment: first production-shell port (ARCA) — findings + the P5b.1 wrap-lifetime fix

**Date:** 2026-06-12 · **Status:** findings VALIDATED on hardware (via the
consumer); engine-side fix **IMPLEMENTED + GATED same day** — code
`008434c` (`render API: release wrapped host textures by end of render
(P5b.1)`, Option A incl. the §5 error-clear gate), public docs `dc7b021`,
tip `gpu-next-render-api-hdr` = `dc7b021`. Full gate record: §7.
Host-contract doc amendments included here and now also in
`include/mpv/render_d3d11.h`.

**Context.** Phase 5b ("production host shell, user app, out of tree" — see
`MERGE_AUDIT_HANDOFF.md` §9.6) now exists: **ARCA**
(`C:\DEV\ARCA`, C++ core + WinUI3 shell) ported the Phase-5a call sequence
into both a bare-Win32 host (`tools/hdr-verify`) and a WinUI3
`SwapChainPanel` composition swapchain. It is the first consumer driving
`pl-d3d11` through real product paths (window resize, hybrid-GPU laptop,
letterboxed targets, long-session seeks). Four findings; **one is an
engine-side defect in this fork** (§2), two are host-contract gaps the
Phase-5 notes must absorb (§3, §4), one is a libplacebo limitation we must
document and defend against on the error path (§5).

Test rig: Win11 hybrid laptop — RTX 4060 Laptop GPU + Intel UHD (panel owned
by the iGPU), internal HDR display (real peak 271 nits), fork tip
`gpu-next-render-api-hdr` = `304f611`.

---

## 1. TL;DR

| # | Finding | Layer at fault | Disposition |
|---|---|---|---|
| 1 | The d3d11 backend holds its `pl_tex` wrap of the host texture **across render calls** → one live COM reference on the host's texture between frames → a host that wraps the swapchain backbuffer directly can never `ResizeBuffers` (`DXGI_ERROR_INVALID_CALL`). The Phase-5a host's "No outstanding backbuffer reference is held between frames" comment is **false**. | **this fork** (`libmpv_pl_d3d11.c`) | **Fix at source: P5b.1** (§2) — **LANDED `008434c` + docs `dc7b021`, all gates green (§7)** |
| 2 | `D3D11CreateDevice(NULL, …)` picks adapter 0 = iGPU on hybrid laptops; Intel UHD cannot tone-map 4K60 (measured ~46 VO drops/s, unbounded; RTX 4060: 0 after warmup). | host | host-contract note (§3) + reference-host edit |
| 3 | `IDXGISwapChain::GetContainingOutput` **fails silently cross-adapter** (device on dGPU, panel owned by iGPU) → the Phase-5 display-peak rule silently degrades to defaults (1000 nits vs real 271) → wrong tone-map target. | host (but our reference host demonstrates the broken pattern) | host-contract amendment (§4) + reference-host edit |
| 4 | `pl_tex_clear` (border clear; also the render hook's purple error clear) requires `params.blit_dst`; wrapped `R10G10B10A2` gets `blit_dst` on Intel but **not NVIDIA** → per-frame validation failure + no border clear on letterboxed targets. | libplacebo (cap model) | document host contract (`--border-background=none` + host clears); gate the error-path clear (§5); candidate upstream libplacebo issue |

None of these invalidate Phase 4/5a results: 4b/5a rendered full-window
(no borders, no resize) on a desktop-style single-adapter setup, which is
exactly why they couldn't surface there. HDR negotiation, DV RPU
reshaping (profile 8.1, real + synthetic content), seeks, and SW-decode
behavior all validated clean through the render API on the consumer side.

---

## 2. P5b.1 — wrap lifetime: release the host texture by end-of-render

### 2.1 The defect

`video/out/d3d11/libmpv_pl_d3d11.c` caches the wrap in backend state and
frees it **at the start of the next call**:

- `struct priv { pl_d3d11 pl_d3d11; pl_tex wrapped_fbo; }`
- `wrap_fbo()`: `if (p->wrapped_fbo) pl_tex_destroy(...)` → `pl_d3d11_wrap()`
  (which takes one COM reference, released only on `pl_tex_destroy`).
- `done_frame()`: no-op.

So between `mpv_render_context_render()` returning and the next render (or
context destroy), the engine holds one `ID3D11Texture2D` reference to the
*host's* texture. DXGI requires **zero** outstanding app references on all
backbuffers for `ResizeBuffers`; a host following the documented Phase-5a
sequence (`GetBuffer(0) → render → Present`, wrapping the backbuffer
directly) therefore fails every resize. Found empirically by ARCA's
automated resize test; ARCA works around it host-side (renders into an
intermediate texture + `CopyResource`, see `C:\DEV\ARCA\DECISIONS.md`
ADR-004) — the workaround costs one trivial GPU copy/frame and should not
be *required* by the API.

Same cross-call caching exists in `libmpv_pl_gl.c` (harmless: a GL FBO wrap
holds no host-lifetime-relevant resource) and `libmpv_pl_vulkan.c`
(latent: a stale wrapped `VkImage` handle across frames is a use-after-free
hazard if the host recreates its swapchain between frames — currently saved
only by the destroy-before-use ordering at the next `wrap_fbo`).

Additionally, `get_target_size()` (`libmpv_gpu_next.c`) calls `wrap_fbo`
for sizing only — that wrap also lingers in backend state until the next
wrap, extending the same reference-holding window.

### 2.2 The fix (preferred shape — Option A: caller-owned wraps) — IMPLEMENTED as specced, `008434c`

Change the `wrap_fbo` contract in `libmpv_gpu_next.h` from
"backend-cached, valid until next call" to **"caller owns the returned
`pl_tex`; destroy when done"**, and drop the `wrapped_fbo` cache from all
three backends:

- `render()` (`libmpv_gpu_next.c`): `pl_tex_destroy(gpu, &fbo)` **after**
  `done_frame()` returns (after `pl_gpu_flush`; D3D11 drivers keep their own
  in-flight references, so releasing the app reference post-flush is safe
  and is precisely what `ResizeBuffers` wants).
- `get_target_size()`: destroy immediately after reading `params.w/h`.
- Error paths between `wrap_fbo` and `done_frame` destroy the wrap on exit
  (incl. the `acquire_target`-failed early return, which deliberately skips
  `done_frame` — the destroy must not).
- Backends: delete `priv.wrapped_fbo` + its destroy-on-next-wrap and
  destroy-in-`destroy()` logic.

**Vulkan pairing caution (the one subtle bit):** the vulkan backend's
`acquire_target`/`done_frame` handshake and the `ensure_held()` audit fix
(`304f611`) currently key off its cached wrap. Under Option A it may keep a
**non-owning** pointer for the handshake (valid through the render call —
the caller destroys only after `done_frame` returns), and `ensure_held()`
logic moves naturally: caller destroys per-frame, so `destroy()` no longer
owns any wrap and the aborted-mid-handshake case is handled by the render
hook's error-path destroy *after* an explicit hold (mirror of the existing
rule: never destroy a wrap libplacebo still holds — i.e. on abort between
acquire and done, call the hold half first, then destroy). Spell this out
in the commit; it is the part a careless port gets wrong.

**Option B (minimal, d3d11-only)** if Option A's Vulkan surgery is deemed
out of scope: keep the cache but destroy it in d3d11's `done_frame()` and
at the end of the sizing path. Smaller diff, but leaves the Vulkan staleness
latency and the asymmetric contract; Option A is the merge-worthy shape.

Suggested commit subject:
`render API: release wrapped host textures by end of render (P5b.1)`
(+ a separate docs commit per §2.4).

### 2.3 Validation gates (per repo convention — every one, per commit)

- **WSL lavapipe windowed golden + teardown (mandatory):** byte-identical
  (no windowed-path code touched; run anyway).
- **`_golden/render_api/rapi_run.sh`** (pl-opengl, llvmpipe): byte-stable.
- **WARP d3d11 harness (`rapi_d3d11_run.sh`):** byte-stable, **plus a new
  refcount case**: `AddRef/Release` probe on the host texture after
  `mpv_render_context_render` returns must show the host as sole owner
  (this is the regression test for the actual defect; it fails on `304f611`).
- **lavapipe vulkan harness (`rapi_vk_run.sh`):** byte-stable; hold/release
  pairing clean (no validation-layer complaints where layers are present).
- **NVIDIA rig:** `rapi_hdr_present.c` with a **programmatic mid-playback
  resize** (port the trigger from ARCA `tools/hdr-verify/main.cpp`,
  `--seconds` path): `ResizeBuffers` must succeed while wrapping the
  backbuffer directly. This is the acceptance test that Phase 5a lacked.
- `run-hdrval.ps1` SW+HW unchanged (HDR-touching rule).

### 2.4 Doc/comment corrections riding along

- `rapi_hdr_present.c`: delete the false comment ("No outstanding
  backbuffer reference is held between frames…") — replace with the new
  contract statement once P5b.1 lands; until then the host comment must say
  the opposite (resize requires an intermediate-texture indirection).
  **DONE** (comment replaced; §3 adapter + §4 peak-query + `--resize-test`
  acceptance also added to the host, see §7).
- `include/mpv/render_d3d11.h`: document the reference-lifetime contract
  ("the renderer holds no reference to the texture after
  `mpv_render_context_render` returns") — this becomes public API wording.
  **DONE** in `dc7b021`.

---

## 3. Host contract amendment — adapter selection (hybrid GPUs)

Measured on the rig (ARCA `docs/verification/day0.md`): identical content,
identical engine, only the `D3D11_INIT_PARAMS` device differs —

| Device | 4K24 75 Mbps DV8.1 | 4K60 DV8.1 |
|---|---|---|
| Intel UHD (adapter 0 default) | 0 drops after ~12s warmup | **~46 VO drops/s, unbounded** |
| RTX 4060 (`EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`) | **0 drops from t=2s, incl. through resize** | 0 after ~4s warmup |

Decoder drops were 0 in *all* cases — CPU SW decode is not the limiter;
gpu-next tone-mapping on the iGPU is. **Host rule:** create the D3D11
device on the high-performance adapter (`IDXGIFactory6::
EnumAdapterByGpuPreference`), not via `D3D11CreateDevice(NULL, …)`;
cross-adapter present to an iGPU-owned panel is handled by the OS. The
engine renders on whatever device it is given and cannot fix this side.
(Windowed mpv exposes `--d3d11-adapter`; the render API host owns the
equivalent choice.) Add to the Phase-5 host-requirements list; update the
reference host to demonstrate it.

---

## 4. Host contract amendment — display-peak query must be HMONITOR-based

Phase 5a's load-bearing rule stands (pass the **display's** peak in
`TARGET_COLORSPACE`), but the reference host's *mechanism*
(`IDXGISwapChain3::GetContainingOutput` → `IDXGIOutput6::GetDesc1`) breaks
exactly when §3 is applied on a hybrid machine: the swapchain's device (dGPU)
is not the adapter that owns the panel, `GetContainingOutput` fails, and the
host silently keeps defaults — observed as tone-mapping targeting 1000 nits
on a 271-nit panel (visibly wrong highlights, no error anywhere).

**Robust pattern** (proven in ARCA `core/src/engine/render_d3d11.cpp::
query_display_hdr`): `MonitorFromWindow(hwnd)` → enumerate **all** DXGI
adapters → outputs → match `DXGI_OUTPUT_DESC.Monitor` → `IDXGIOutput6::
GetDesc1().MaxLuminance/MinLuminance`. Re-query on resize (monitor moves).
Update `rapi_hdr_present.c` and fold into the Phase-5 host requirements
(this supersedes the 5a mechanism, not the 5a rule).

---

## 5. Known limitation — `pl_tex_clear` needs `blit_dst`; NVIDIA R10G10B10A2 lacks it

With a letterboxed target (any windowed player), gpu-next's default
`--border-background=color` clears the bars via `pl_frame_clear_rgba` →
`pl_tex_clear`, which validates `dst->params.blit_dst`. The wrapped
`R10G10B10A2` host texture gets `blit_dst` on Intel but **not** on NVIDIA
(observed: per-frame `Validation failed: dst->params.blit_dst`
(`gpu.c:318`) + unclear borders; libplacebo v7.360.1). 4b/5a never hit it:
full-window video, no borders.

- **Host contract (now):** run `--border-background=none` and clear the
  target host-side before each render (ARCA does
  `ClearRenderTargetView` on its intermediate texture; ~free).
- **This fork (small hardening, can ride with P5b.1):** the render hook's
  *error-path* clear (`libmpv_gpu_next.c` `render()`: purple
  `pl_tex_clear` on `!valid`) has the same requirement → gate it on
  `fbo->params.blit_dst` so a render failure on NVIDIA doesn't cascade into
  clear-validation spam. **DONE — rode along in `008434c`.**
- **Upstream candidate (libplacebo, out of tree):** d3d11 `pl_tex_clear`
  could fall back to `ClearRenderTargetView` for renderable textures
  instead of requiring blittable caps. Worth filing; not ours to patch here.

---

## 6. Why this doc instead of the edit

Per `CLAUDE.md` conventions every fork commit must pass the full gate set
(WSL lavapipe golden + teardown + render-API harness; HDR-touching →
`run-hdrval` SW+HW on the rig). This session ran on the consumer repo
(ARCA) without the WSL loop; landing P5b.1 without those gates would break
the repo's own rules. Implement in a session opened on this clone /
`mpv-wt-hdr`, follow §2.2–§2.4, then refresh ARCA's vendored DLL
(`C:\DEV\ARCA\third_party\mpv\refresh.ps1`) — after which ARCA's
intermediate-texture indirection (its ADR-004) can optionally be retired to
the direct-backbuffer path the API documents.

**Consumer cross-refs:** ARCA findings + measurements
`C:\DEV\ARCA\docs\verification\day0.md`; host workaround + rationale
`C:\DEV\ARCA\DECISIONS.md` (ADR-004); resize/seek test driver
`C:\DEV\ARCA\tools\hdr-verify\main.cpp`.

---

## 7. Implementation record (2026-06-12, same-day close-out)

**Commits** (tip `gpu-next-render-api-hdr` = `dc7b021`, on `304f611`):

- `008434c` `render API: release wrapped host textures by end of render
  (P5b.1)` — Option A exactly as §2.2: caller-owned wraps, cache dropped
  from all three backends, hook destroys at end-of-render / sizing /
  acquire-failure; Vulkan handshake on a non-owning `cur_fbo`, failed-hold
  recovery via an owning `orphaned_fbo` (preserves `304f611`'s "never
  destroy a wrap libplacebo still holds", incl. the leak-at-teardown
  fallback). §5's `blit_dst` gate on the purple error clear rode along.
- `dc7b021` `render API docs: wrapped-target reference lifetime, D3D11
  host guidance` — render_d3d11.h reference-lifetime contract (§2.4) +
  §3/§4/§5 host guidance now in the public header; render_vulkan.h wrap
  lifetime + failed-hand-back exception; client-api-changes.rst 2.7 note.

**Plan-1 branch disposition:** not needed. The defect is reachable only
through the D3D11/Vulkan backends (hdr branch only); on Plan 1's GL-only
surface the cached wrap held no host-lifetime-relevant resource and GL has
no ResizeBuffers analogue. The contract change rides with Plan 2 rather
than forcing a 10-commit rebase over the same hunks.

**Gates (all green, 2026-06-12):**

- WARP d3d11 (`rapi_d3d11_run.sh`): 6/6 byte-identical to baseline +
  srgb==none / pq!=none / hdr10!=sdr invariants + **new permanent
  `--refcount` case** — host sole owner post-render (post-render=2/FAIL on
  `304f611`, 1/1 PASS on `008434c`; teardown leak check included).
- NVIDIA rig resize acceptance (`rapi_hdr_present --resize-test`, new):
  **6/6 mid-playback `ResizeBuffers` OK wrapping the backbuffer directly**
  (361 frames, HDR mode, no ClearState needed — libplacebo leaves no bound
  state). Host updated per §2.4/§3/§4: false comment replaced,
  high-performance adapter (RTX 4060 selected), HMONITOR-based peak query
  (271 nits found with device on the dGPU — §4's exact failure case).
- NVIDIA Vulkan harness: sdr/srgb/pq10/pq16f **byte-identical to the
  pre-fix 2026-06-04 run** (pts 0.5) and deterministic (two runs).
- WSL lavapipe: windowed golden **12/12** byte-identical, teardown ALL
  CLEAN, render-API pl-opengl **4/4**, lavapipe Vulkan **4/4**.
- `run-hdrval.ps1 -Ref hdr` SW **and** HW: base1/base2/ref all
  `46CDAFFF48CFFFA4` (the documented golden), JSON sidecars identical
  per mode.

**Consumer re-vendored:** ARCA `refresh.ps1` run → provenance at
`dc7b021` (110 runtime DLLs, 54 exports). `hdr-verify dv81_4k24.mp4
--seconds 20` exit 0 on the refreshed engine (DV8.1 path + programmatic
resize, display peak 271 picked up). Found+fixed a latent refresh.ps1 bug:
the ldd closure ran against the vendored DLL, so on any *re*-refresh every
dep resolved from the already-vendored closure (module dir precedes PATH)
and the >=20-deps sanity check threw; it now ldd's the source DLL in the
worktree. **ARCA's ADR-004 intermediate-texture workaround can now be
retired** (direct backbuffer is legal per the new contract) — that is
ARCA-side work for an ARCA session.
