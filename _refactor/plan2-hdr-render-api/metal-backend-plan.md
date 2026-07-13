# macOS on the libmpv render API — pl-vulkan via MoltenVK

> **2026-07-13 update:** the §2.5 `ra_create_pl` change described below has
> landed in the current release branch (`7f6cc15929`). The Vulkan render backend
> now exposes the imported libplacebo GPU to mpv's hwdec registry, and
> `hwdec_vt_pl.m` already contains the `PL_HANDLE_MTL_TEX` VideoToolbox bridge.
> Treat the implementation steps in §2.5/§3 as historical; only the real-Mac
> build and functional zero-copy/HDR validation remain.

**Date:** 2026-06-13 (rewritten) · **Status:** the macOS path is **`pl-vulkan`
on MoltenVK — no new mpv code**. A *native* Metal backend is **not buildable**
and is blocked upstream of mpv (see §4). This supersedes the earlier
"Native Metal backend (Phase 8)" plan, which was authored on Windows against a
libplacebo Metal API **that does not exist**.

> **What changed, and why.** The earlier plan assumed libplacebo ships a native
> Metal backend — `<libplacebo/metal.h>`, `pl_metal_create`, `pl_metal_wrap`,
> `pl_has_metal` — that merely wasn't packaged off-Mac, so "a Mac to build
> metal.h" was treated as the only blocker. **None of that exists.** Verified
> 2026-06-13 against libplacebo `master`:
> - public headers (`src/include/libplacebo/`) are `vulkan.h`, `opengl.h`,
>   `d3d11.h`, `dummy.h` — **there is no `metal.h`**;
> - `meson.build` defines graphics components vulkan / opengl / d3d11 / dummy —
>   **no `pl_has_metal`, no Metal component**;
> - libplacebo's own docs: *"no native Metal backend… on macOS, `gpu-api=vulkan`
>   via MoltenVK is required."* Its advertised "Metal support" **is** the Vulkan
>   backend on MoltenVK.
>
> mpv confirms the same shape: there is **no `features['metal']`** in
> `meson.build`; `video/out/mac/metal_layer.swift` is a `CAMetalLayer` compiled
> *only* under `features['vulkan']`, paired with `video/out/vulkan/context_mac.m`.
> mpv's macOS GPU path already **is** MoltenVK. There was never a native-Metal
> `pl_gpu` to wrap a texture into; the old plan inverted the effort (§4).

---

## 1. The macOS path (what to actually do)

gpu-next renders through a libplacebo `pl_gpu`. On macOS the only `pl_gpu` that
exists *and* is HDR-capable is **Vulkan on MoltenVK**. So the macOS render-API
backend is the one already written and proven byte-stable on lavapipe:
**`MPV_RENDER_API_TYPE_PL_VULKAN`** — `include/mpv/render_vulkan.h` +
`video/out/vulkan/libmpv_pl_vulkan.c`. No Metal-specific mpv code is needed, and
none is possible. The host's macOS shell talks Vulkan (via MoltenVK) to mpv and
bridges to Metal at the surface, exactly as mpv's own windowed macOS VO does.

Build mpv on macOS with `features['vulkan']` ON. That single feature pulls in
libplacebo Vulkan, the `CAMetalLayer` surface path (`context_mac.m` +
`metal_layer.swift`), and the VideoToolbox-on-libplacebo hwdec
(`video/out/hwdec/hwdec_vt_pl.m`, gated on `vulkan` + CoreVideo). Confirm the
configure summary shows `vulkan` ON and that the MoltenVK ICD is found.

## 2. Host integration (your C++ core + macOS native shell)

### 2.1 Prerequisites (host side)
- Link **MoltenVK** (Apple's Vulkan ICD) + the Vulkan loader. The LunarG Vulkan
  SDK for macOS bundles both; or link MoltenVK directly as the ICD.
- Create the `VkInstance` with `apiVersion >= VK_API_VERSION_1_2` and the
  portability bits: instance extension `VK_KHR_portability_enumeration` + the
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` flag, plus
  `VK_EXT_metal_surface` (for pattern A) and/or `VK_EXT_metal_objects`
  (pattern B).
- Create the `VkDevice` with `VK_KHR_portability_subset` **and the features
  libplacebo requires** (`pl_vulkan_required_features`) — the same host
  obligation `render_vulkan.h` already documents. libplacebo handles the
  portability-subset limitations; the host just has to enable the extension.

### 2.2 Render-API calls (identical to the Vulkan backend you wrote)
- `MPV_RENDER_PARAM_API_TYPE = "pl-vulkan"`.
- `MPV_RENDER_PARAM_VULKAN_INIT_PARAMS` (`mpv_vulkan_init_params`): your
  `VkInstance` / `VkPhysicalDevice` / `VkDevice`, queue family indices, and
  `get_proc_addr` = MoltenVK's `vkGetInstanceProcAddr`.
- Per frame: `MPV_RENDER_PARAM_VULKAN_TEX` (`mpv_vulkan_tex`): the target
  `VkImage` + acquire/release semaphores + layouts — the exact handshake
  `libmpv_pl_vulkan.c` implements. Under MoltenVK a timeline `VkSemaphore` is a
  wrapped `MTLSharedEvent`, so the old plan's "use an `MTLSharedEvent`"
  instinct is right — it's just expressed in Vulkan and already coded.

### 2.3 Getting pixels onto a CAMetalLayer / MTLTexture — two patterns
- **(A) MoltenVK swapchain on a `CAMetalLayer`.** Host makes a surface with
  `vkCreateMetalSurfaceEXT(CAMetalLayer*)` (`VK_EXT_metal_surface`), a
  `VkSwapchainKHR`, and hands mpv each acquired swapchain `VkImage` as the
  `VULKAN_TEX`. MoltenVK presents to the layer. **Simplest**, and it's exactly
  what mpv's own `video/out/vulkan/context_mac.m` does — read it as the
  reference.
- **(B) `MTLTexture` ↔ `VkImage` interop (`VK_EXT_metal_objects`).** If the host
  owns the `MTLTexture` (its own Metal compositor/renderer), import it as a
  `VkImage` via `VkImportMetalTextureInfoEXT` (legacy: `vkSetMTLTextureMVK`),
  hand that `VkImage` to mpv, and once `release_sem` fires the pixels are in the
  host's `MTLTexture`. **This is the "render straight into my native
  `MTLTexture`" outcome the old plan wanted — delivered through the existing
  Vulkan backend, zero new mpv code.**

### 2.4 HDR / EDR on macOS
Pass `MPV_RENDER_PARAM_TARGET_COLORSPACE` per frame, same as D3D11/Vulkan. The
macOS specifics (host side):
- `CAMetalLayer.wantsExtendedDynamicRangeContent = YES`, an HDR
  `pixelFormat` (`rgba16Float`, or `bgr10a2`/`bgra10_xr`), the layer `colorspace`
  set, and `CAEDRMetadata` for HDR10/PQ.
- Query the display headroom for the colorspace peak —
  `NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue` (and the
  current `maximum…Value`). This is the macOS analogue of the phase-5b D3D11
  finding (`IDXGIOutput6::GetDesc1().MaxLuminance`): the host must measure the
  display peak and pass it in `TARGET_COLORSPACE`; an OS screenshot is not a
  valid HDR comparison.

### 2.5 VideoToolbox hwdec — NOT yet wired on the render API (one small mpv change)
**Correction (verified 2026-06-14 against the code):** VideoToolbox hwdec works
on mpv's *windowed* macOS VO, but **does not engage through the libmpv render
API yet.** The interop `video/out/hwdec/hwdec_vt_pl.m` resolves its target with
`ra_pl_get(hw->ra_ctx->ra)` (`hwdec_vt_pl.m:45`) — it needs the surface backend
to expose an **`ra_ctx` whose `ra` is an `ra_pl`**. The render-API Vulkan
backend currently sets **only `ctx->gpu`, never `ctx->ra_ctx`**
(`libmpv_pl_vulkan.c:85`), and the render hook gates *all* hwdec on
`if (p->context->ra_ctx)` (`libmpv_gpu_next.c`). So on the render API today:
**software decode only** — the exact state D3D11 was in before Phase 7a.

This is the Vulkan/MoltenVK analogue of Phase 7a, and it is **small** — smaller
than 7a, even, because no SPIR-V compiler or device-specific `ra` is involved.
In `libmpv_pl_vulkan.c::init`, after `ctx->gpu` is set, additionally:

```c
ctx->ra_ctx = talloc_zero(p, struct ra_ctx);
ctx->ra_ctx->log = ctx->log;
ctx->ra_ctx->global = ctx->global;
ctx->ra_ctx->ra = ra_create_pl(ctx->gpu, ctx->log);   // video/out/placebo/ra_pl.h
if (ctx->ra_ctx->ra)
    ctx->ra = ctx->ra_ctx->ra;
else { /* best-effort: drop ra_ctx, continue SW-only (mirror 7a) */ }
```

`ra_create_pl` just wraps the `pl_gpu` the backend already has. The decoded
frame → `pl_tex` bridge **already exists** for `ra_pl` in `gpu_next/core.c`
`hwdec_plane_tex` (`if (ra_pl_get(ra)) return (pl_tex) ratex->priv;`), so no
core change is anticipated — same shape as 7a.

**Two caveats before calling it done:**
- **Functional sign-off needs the Mac.** The mpv change is platform-agnostic
  and can be build-verified + lavapipe-gated on Windows/Linux, but whether
  VideoToolbox actually engages (and the `CVPixelBuffer`/`IOSurface` → `VkImage`
  import succeeds under MoltenVK) can only be proven on a real Mac — ARCA's
  `hdr-verify` flipping `hwdec=videotoolbox` is the cheapest harness.
- **The host may need extra device extensions.** The IOSurface→VkImage import
  path under MoltenVK can require Metal-interop / external-memory device
  extensions enabled at `VkDevice` creation. This is a *host-contract* addition
  to §2.1, not a decode-queue negotiation — distinct from, and far lighter
  than, the generic "Vulkan-video decode" device threading (which VideoToolbox
  does **not** need, since CoreVideo does the decode and mpv only imports the
  surface). Verify the exact extension list on the Mac.

So: **this supersedes the old §6 ("Step 8b") mechanism but not the work** — it
relocates it from "native Metal hwdec" to "one `ra_create_pl` line in the
Vulkan backend + a Mac functional gate." Rendering does not depend on it.

### 2.6 Caveats (MoltenVK portability)
- Enable `VK_KHR_portability_enumeration` (instance) + `VK_KHR_portability_subset`
  (device) or device enumeration/creation fails.
- Timeline semaphores: MoltenVK supports `VK_KHR_timeline_semaphore` (Vulkan
  1.2); the acquire/hold handshake works. Binary-semaphore fallback is fine too.
- One device for everything (render + VideoToolbox interop) — create the
  `VkDevice` once via MoltenVK.

## 3. Validation
**Rendering** needs no new mpv code, so the **existing Vulkan gate covers it**:
the lavapipe byte-stable result from phase 6 is the deterministic oracle (a
stronger one than macOS has — there is no software-Metal oracle, and MoltenVK is
not deterministic across machines). On the Mac, the gate is the consumer (your
player) plus a visual EDR check against a windowed
`--vo=gpu-next --gpu-api=vulkan` reference — the macOS analogue of the phase-5b
on-display D3D11 check. There is no native-Metal harness because there is no
native-Metal backend.

**hwdec** (§2.5) does need the one-line mpv change, which carries its own gate:
build-verify + lavapipe Vulkan no-regression on Windows/Linux (proves the
`ra_create_pl` ra_ctx doesn't perturb the rendering path), then the *functional*
sign-off on the Mac via ARCA `hdr-verify` with `hwdec=videotoolbox`
(`hwdec-current == videotoolbox`, no decoder drops) — mirroring the D3D11 7a
`--hwdec` harness gate.

## 4. "Native Metal" — the real scope, if it is ever wanted

The old plan described "one new translation unit + one header + a few wiring
lines." That scoping was only valid *if* libplacebo already had a Metal
`pl_gpu` to wrap a texture into. It does not. The actual prerequisite is large
and lives **in libplacebo, not mpv**:

1. A native Metal `pl_gpu` in libplacebo: a new `<libplacebo/metal.h>` +
   `src/metal/` implementing the entire `pl_gpu` abstraction on `MTLDevice`
   (buffers, textures, render/compute passes, sync), **plus GLSL→MSL shader
   translation** for all of libplacebo's shader output (via SPIRV-Cross or
   naga). This does not exist and is not on libplacebo's public roadmap.
2. *Only then* does the thin mpv-side `libmpv_pl_context_metal.c` +
   `render_metal.h` (wrap a host `MTLTexture` as a `pl_tex`, à la the D3D11
   backend) become the small task the old plan imagined.

**Recommendation: don't.** Ship on `pl-vulkan`/MoltenVK (§1–§2); it already
carries HDR/EDR and VideoToolbox hwdec. Revisit a native backend only if/when
libplacebo lands a Metal `pl_gpu` upstream — at which point step 2 above is a
straight mirror of the D3D11 backend and this repo's render-hook architecture
absorbs it with no core changes.

## 5. Cross-refs
- The macOS backend + its public API: `video/out/vulkan/libmpv_pl_vulkan.c`,
  `include/mpv/render_vulkan.h`.
- mpv's own MoltenVK usage (host reference): `video/out/vulkan/context_mac.m`,
  `video/out/mac/metal_layer.swift`.
- VideoToolbox hwdec on the libplacebo path: `video/out/hwdec/hwdec_vt_pl.m`.
- Render hook + backend table (unchanged): `video/out/gpu_next/libmpv_gpu_next.{c,h}`.
- Phase-6 Vulkan result + the original MoltenVK-for-macOS decision:
  `hdr-phase6-assessment.md`. Doc map: `../INDEX.md`.
