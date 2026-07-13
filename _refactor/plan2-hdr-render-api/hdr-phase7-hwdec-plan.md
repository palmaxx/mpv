# Plan 2 — Phase 7: hardware decoding on the render API (D3D11 first, Vulkan next)

> **STATUS UPDATE 2026-06-13 — 7a (D3D11) IMPLEMENTED + FUNCTIONALLY SIGNED OFF
> on the NVIDIA rig.** The plan validated against the code exactly: the change
> is a single tracked file, `libmpv_pl_d3d11.c::init`/`destroy` (no
> `libmpv_gpu_next.c` edit — the `if (p->context->ra_ctx)` gate opened itself).
> The §3.3 frame-bridge risk was **resolved by inspection then proven on
> hardware**: the `ra_tex`→`pl_tex` D3D11 bridge already exists
> (`gpu_next/core.c` `hwdec_plane_tex`, `ra_is_d3d11` → `ra_d3d11_get_raw_tex`
> → `pl_d3d11_wrap` onto `core->gpu`, zero-copy since one device). Functional
> probe (new `rapi_harness_d3d11 --hwdec`, real NVIDIA GPU, `hdr10_4k.mp4`):
> `hwdec-current=d3d11va`, `decoder-frame-drop-count=0`, non-uniform readback
> → **PASS**. Gates green: WARP SW path byte-identical to baseline (6/6 +
> refcount), P5b.1 wrap-lifetime contract intact under hwdec
> (`--hwdec --refcount` → host sole owner post-render, no leak post-free).
> One deliberate deviation from §3.2 step 3: ra_ctx build is **best-effort** —
> on SPIR-V/ra_d3d11 failure it tears down the partial ra_ctx and continues
> software-decode only, rather than bailing `MPV_ERROR_UNSUPPORTED` (which
> would turn a previously-working SW config into a hard init failure). Pending:
> commit (held for user), WSL gate (a formality — d3d11 isn't compiled there,
> so this d3d11-only TU cannot touch the windowed/pl-opengl paths), ARCA
> re-vendor + ADR-003 retirement. 7b (Vulkan) remains deferred (§4).

**Date:** 2026-06-13 · **Status:** ~~PLAN (not started)~~ **7a DONE (see banner)**. Supersedes the
"Phase 4" placeholder in the P2.2 code comment
(`libmpv_gpu_next.c`: "d3d11va interop on the render API is Phase 4" — that
number was reused by the HDR-validation phase; render-API hwdec is Phase 7).

**One-line thesis.** The render hook's hwdec machinery is already complete
and backend-agnostic — it is gated only on whether the surface backend
exposes a `ra_ctx` (P2.2). The GL backend proves the whole core path works
end to end. So **render-API hwdec for D3D11 is, to first approximation, the
same delta the F-phase was for GL: have the backend's `init()` build a
`ra_ctx` from the host's device.** No render-hook changes are anticipated.

> Audience: an implementer in a session on this clone / `mpv-wt-hdr`.
> Cites are `file:line` at fork tip `dc7b021` (`gpu-next-render-api-hdr`).
> Companion: `MERGE_AUDIT_HANDOFF.md` §9.1 (the deferral this closes),
> `hdr-phase5b-assessment.md` (the consumer, ARCA, that needs it).

---

## 1. Why this is wanted now

The Day-0 consumer (ARCA) ships **software decode only** (its ADR-003), a
deliberate deferral pending this phase. Measured on its rig (NVIDIA RTX 4060,
`C:\DEV\ARCA\docs\verification\day0.md`): SW decode holds 4K24 and even 4K60
with **zero decoder drops**, but 4K60 carries a ~29-frame VO warmup transient
and burns CPU that hardware decode would return. d3d11va hwdec is the
remaining box to tick for "true 4K HDR/DV native playback" parity with the
windowed path, and it is the last item blocking ADR-003's retirement.

## 2. How hwdec already works on the render API (the GL precedent)

The render backend stands up the **existing** `ra_hwdec_ctx` registry and
drives it from the core's `hwdec_get` hook — all of it generic:

- `libmpv_gpu_next.c:176-223` — builds `hwdec_devs`, forwards host native
  resources, calls `ra_hwdec_ctx_init(&p->hwdec_ctx, …)`, wires
  `gpu_next_core_frontend{ .hwdec_get = core_hwdec_get, .hwdec_ctx = … }`.
  **The entire block is gated `if (p->context->ra_ctx)`** (`:186`). A backend
  with no `ra_ctx` gets a zero-initialised `hwdec_ctx`, so the core's
  `hwdec_get` finds no interop → software decode (P2.2).
- Teardown is likewise generic: `ra_hwdec_ctx_uninit` (`:662`) +
  `hwdec_devices_destroy` (`:663-665`).
- The GL backend populates `ra_ctx` by building a **blank but real** `ra` via
  `ra_gl_ctx_init` (`libmpv_pl_gl.c:57-72`) — its swapchain is a dummy (we
  render through `pl_opengl_wrap`, not the ra swapchain); the `ra` exists
  **only** so the ra-typed hwdec interops accept it. `ctx->ra =
  ctx->ra_ctx->ra` (`:72`).

The hwdec interops gate on the `ra` *type*: `ra_hwdec_d3d11va` reads the
device with `ra_d3d11_get_device(hw->ra_ctx->ra)` (`hwdec_d3d11va.c:78`), as
does `ra_hwdec_dxva2dxgi` (`hwdec_dxva2dxgi.c:79`). Give them a `ra` of type
`ra_d3d11` and they light up.

**Consequence:** the D3D11 surface backend currently sets `ctx->gpu` but
leaves `ctx->ra_ctx == NULL` (`libmpv_pl_d3d11.c` `init()`), which is exactly
and only why it is SW-only. Filling that field is the phase.

## 3. Phase 7a — D3D11 (`d3d11va`): the primary deliverable

### 3.1 The building block already exists

`ra_d3d11_create(ID3D11Device *dev, struct mp_log *log, struct spirv_compiler
*spirv)` wraps an **externally-supplied** device (`ra_d3d11.c:2389`,
`ra_d3d11.h:20`); the windowed path calls it on its own device
(`d3d11/context.c:526`). For the render API we call it on the **host's**
device — the same `ID3D11Device` already handed to `pl_d3d11_create` via
`MPV_RENDER_PARAM_D3D11_INIT_PARAMS`. Two abstractions (libplacebo's
`pl_d3d11` for rendering, mpv's `ra_d3d11` for hwdec interop) over one device,
each taking its own COM reference. **The render device and the d3d11va decode
device are therefore the same device by construction** — the "thread the
host's decode device through" worry in `MERGE_AUDIT_HANDOFF.md` §9.1 dissolves
for D3D11 (it is real for Vulkan, §4).

### 3.2 The change (mirror the F-phase, in `libmpv_pl_d3d11.c::init`)

After `pl_d3d11_create` succeeds and `ctx->gpu` is set, additionally:

1. Create a `spirv_compiler` (the `ra_d3d11_create` 3rd arg). Mirror the
   windowed setup in `d3d11/context.c` (it builds `ctx->spirv` via
   `spirv_compiler_create` before `ra_d3d11_create`). Own it in `struct priv`.
2. `ctx->ra_ctx = talloc_zero(p, struct ra_ctx);` set `log`/`global`/`opts`
   (`allow_sw` is irrelevant for d3d11; set debug from `ra_ctx_conf` as the GL
   backend does at `libmpv_pl_gl.c:66-69`).
3. `ctx->ra_ctx->ra = ra_d3d11_create(host_device, ctx->log, spirv);` bail
   `MPV_ERROR_UNSUPPORTED` on NULL. No swapchain is created (like GL's dummy —
   we present through the host, render through `pl_d3d11_wrap`).
4. `ctx->ra = ctx->ra_ctx->ra;`
5. `destroy()`: tear down in the right order — `ra` is freed via
   `ctx->ra->destroy(ctx->ra)` (or the ra_d3d11-appropriate teardown; check
   `ra_d3d11.c`), then the `spirv_compiler`, then `pl_d3d11_destroy`, then
   `pl_log_destroy`. Note the render hook already calls
   `ra_hwdec_ctx_uninit` **before** the context `destroy()`
   (`libmpv_gpu_next.c:662` runs in the backend uninit path ahead of the
   context teardown), so hwdec is gone before `ra` dies — preserve that
   ordering.

That is the whole expected surface. **No edit to `libmpv_gpu_next.c` is
anticipated** — the `if (p->context->ra_ctx)` gate opens itself.

### 3.3 The one real risk: the decoded-frame → `pl_tex` bridge

The core maps an hwdec source frame through `ra_hwdec_mapper` (producing
ra-side textures) and must hand libplacebo a `pl_tex` to render. The GL path
already does this for `ra_gl`; the question is whether the d3d11 mapper's
output reaches `pl_d3d11`'s `gpu` cleanly. Two outcomes, both tractable:

- **Likely:** the core's hwdec frame mapping is ra-typed and produces a
  `pl_tex` via the shared `gpu_next/core` path regardless of backend (the GL
  backend is the existence proof that core-level mapping is backend-agnostic).
  Then 3.2 is genuinely the whole change.
- **If a d3d11-specific bridge is missing:** the decoded `ID3D11Texture2D`
  (from `ra_hwdec_d3d11va`) may need wrapping via `pl_d3d11_wrap` into the
  same `pl_gpu` the renderer uses (both sit on the host device, so this is a
  zero-copy wrap, not a copy). Localise any such bridge in the core's hwdec
  mapping, not the backend.

**Verify this first**, before polishing — it is the only part not already
proven by the GL backend. Smallest probe: `hwdec=d3d11va` on a short clip,
watch `hwdec-current` flip to `d3d11va` and confirm a frame renders (not
purple/black).

### 3.4 Validation gates (repo convention — every commit)

- **WSL lavapipe windowed golden + teardown (mandatory):** byte-identical.
  (d3d11 isn't built on WSL, so this only proves no windowed/`pl-opengl`
  regression — run it anyway.)
- **`_golden/render_api/rapi_run.sh`** (pl-opengl): byte-stable.
- **WARP d3d11 harness:** still byte-stable for the **SW** path. WARP has **no
  hardware video decoder**, so it cannot exercise d3d11va — do not expect it
  to; it guards the SW path against regression while ra_ctx now exists.
- **Functional sign-off (NVIDIA rig — the real gate):** this is the proof the
  software rigs structurally cannot give (the open item from
  `MERGE_AUDIT_HANDOFF.md` §9.1 / Plan-1 hwdec). Two equivalent routes:
  1. Extend `rapi_harness_d3d11` with a `--hwdec` mode that loads a real DV/HEVC
     clip and asserts `hwdec-current == d3d11va` + a non-purple readback.
  2. **Or use the consumer:** ARCA's `hdr-verify` already prints
     `hwdec-current` and `decoder-frame-drop-count`. Flip ARCA's engine option
     off its pinned `hwdec=no` (ADR-003) to `d3d11va`, re-run the 4K60 case,
     and confirm hwdec engages with no decoder drops and reduced VO warmup.
     This is the cheapest real-hardware harness and closes the loop with the
     consumer that motivated the phase.
- `run-hdrval.ps1` SW+HW unchanged (HDR-touching rule).

### 3.5 Definition of done (7a)

`hwdec=d3d11va` (and `auto`) decodes on the GPU through `pl-d3d11`, HDR + DV
RPU passthrough intact (Phase 4b/5 behaviour unchanged), no regression on the
SW path, ADR-003 retractable on the consumer side.

## 4. Phase 7b — Vulkan (`vaapi`/Vulkan-video): deferred follow-up

Structurally the same shape (build a `ra_ctx` of type `ra_vk` from the
imported `VkDevice` so `ra_hwdec` vulkan interops accept it), but materially
harder, hence a separate, later phase:

- The render API imports the host's `VkDevice` (`pl_vulkan_import`,
  `libmpv_pl_vulkan.c`). For hwdec the **decode** queues/device must be the
  same device — so the host's import params (queues, extensions) must satisfy
  the decoder's needs; this is the genuine "thread the decode device through"
  work §9.1 flagged, and it is real for Vulkan (unlike D3D11 §3.1).
- Vulkan hwdec interop maturity is lower and platform-split (VAAPI-over-Vulkan
  on Linux; nascent Vulkan-video). Cross-platform value is high (it is also
  the macOS/MoltenVK rendering path) but the decode story there is the least
  settled.
- Build/test only where `pl_has_vulkan` (lavapipe has no HW decoder, so
  functional sign-off needs a real Vulkan GPU — same shape as 7a's NVIDIA gate
  but on a Vulkan rig).

Recommend landing 7a first and standalone; 7b after 7a's frame-bridge
learnings (§3.3) are banked.

## 5. Sequencing & non-goals

1. **7a-probe:** prove the frame bridge (§3.3) on the NVIDIA rig with a minimal
   `init()` that builds the d3d11 `ra_ctx`. Gate: a hwdec frame renders.
2. **7a-impl:** finish `init()`/`destroy()` ordering, spirv ownership, debug
   plumbing; full gate set (§3.4); commit
   `render API: d3d11va hwdec via host-device ra_ctx (P7.1)`.
3. **Consumer:** re-vendor into ARCA, flip `hwdec`, re-run M2 4K60, retire
   ADR-003.
4. **7b:** Vulkan, later.

**Non-goals:** no change to the HDR/DV render path (Phase 3-5 frozen); no new
public API (hwdec is engine-internal — the host already just passes its
device); no D3D11 screenshot/`get_image` changes.

## 6. Cross-refs

- Generic hwdec wiring + the P2.2 gate: `libmpv_gpu_next.c:176-223`, `:662`.
- GL precedent (the template): `libmpv_pl_gl.c:53-72`.
- D3D11 backend to extend: `libmpv_pl_d3d11.c::init`.
- ra-from-existing-device: `ra_d3d11.c:2389`, `ra_d3d11.h:20`; consumers
  `hwdec_d3d11va.c:78`, `hwdec_dxva2dxgi.c:79`; windowed setup
  `d3d11/context.c:526`.
- Deferral being closed: `MERGE_AUDIT_HANDOFF.md` §9.1.
- Consumer + motivation: `hdr-phase5b-assessment.md`,
  `C:\DEV\ARCA\docs\verification\day0.md` (SW-decode headroom table),
  `C:\DEV\ARCA\DECISIONS.md` ADR-003.
