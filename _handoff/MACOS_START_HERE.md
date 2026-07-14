# mpv render-API on macOS — START HERE (premise corrected 2026-06-13)

**The original plan for this work-copy rested on a false premise and is
superseded.** It assumed libplacebo has a native Metal backend
(`<libplacebo/metal.h>`, `pl_metal_create`, `pl_metal_wrap`, `pl_has_metal`)
that merely needed a Mac to build. **It does not exist.** libplacebo has no
Metal `pl_gpu` at all — its graphics backends are Vulkan, OpenGL, D3D11 and a
dummy; its "Metal support" *is* the Vulkan backend running on **MoltenVK**.
A Mac does not unblock a native libplacebo Metal backend because there is
nothing of that shape to build. It is still required to build and validate the
MoltenVK host, HDR/EDR output, and VideoToolbox interop. (Verified against
libplacebo `master`: no `metal.h`, no `pl_has_metal`. mpv agrees: there is no
`features['metal']` — `metal_layer.swift` is a `CAMetalLayer` compiled only
under `features['vulkan']`.)

## The macOS path

Use the existing **`pl-vulkan`** backend on **MoltenVK** —
`include/mpv/render_vulkan.h` + `video/out/vulkan/libmpv_pl_vulkan.c`, already
written and proven byte-stable on lavapipe. It already does HDR/EDR
(`MPV_RENDER_PARAM_TARGET_COLORSPACE`). Your host shell talks Vulkan to mpv via
MoltenVK and bridges to a `CAMetalLayer` / `MTLTexture` at the surface
(`VK_EXT_metal_surface` or `VK_EXT_metal_objects`).

- **Rendering: READY now — net new mpv code zero.** Start embedding this.
- **VideoToolbox hwdec: wired in source; Mac validation pending.** The Vulkan
  backend creates an `ra_create_pl` context over the imported MoltenVK GPU, and
  `hwdec_vt_pl.m` imports VideoToolbox `CVMetalTexture` planes through
  `PL_HANDLE_MTL_TEX`. A real Mac must still prove the required MoltenVK Metal
  import capability and confirm `hwdec-current=videotoolbox` with no download.
  See `MACOS_AGENT_HANDOFF.md` for the current gate.

## Read next

→ **`_refactor/plan2-hdr-render-api/metal-backend-plan.md`**: the
macOS / MoltenVK integration guide for your player (init params, the two
CAMetalLayer/MTLTexture patterns, EDR, hwdec, caveats), plus the **honest scope
of what a real native Metal backend would require** — a libplacebo Metal
`pl_gpu`, a large upstream project, not an mpv backend. Its old §2.5 todo is
superseded by the current source and `MACOS_AGENT_HANDOFF.md`.

The Windows-side context (`CLAUDE-windows-reference.md`) and the Plan-2 docs
under `_refactor/` are unchanged except where they still describe the old
"native Metal" deferral; treat `metal-backend-plan.md` as the source of truth.
