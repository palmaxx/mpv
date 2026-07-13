# Phase 5a — Minimal verification host (bare Win32 HDR swapchain), VISUALLY CONFIRMED

**Status:** ✅ DONE / confirmed on the NVIDIA HDR display (2026-06-04). A
framework-free ~270-LOC Win32 host (`_golden/render_api/rapi_hdr_present.c`,
git-excluded) plays an HDR10 clip through `MPV_RENDER_API_TYPE_PL_D3D11` into a
real DXGI HDR10 swapchain and, side-by-side with windowed `mpv --vo=gpu-next
--gpu-api=d3d11`, **looks the same on the HDR display.** No mpv source change —
this is the render-API exercised as a production host would, and the deliverable
the plan's Phase 5a calls for.

## §1. What the host does (the reference call sequence for 5b)

- `D3D11CreateDevice(HARDWARE)` (NVIDIA) + `CreateSwapChainForHwnd` with
  `DXGI_FORMAT_R10G10B10A2_UNORM`, flip-discard, 2 buffers.
- `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)`
  (gated on `CheckColorSpaceSupport`) + `IDXGISwapChain4::SetHDRMetaData` (HDR10
  mastering display metadata).
- `mpv_render_context_create(PL_D3D11, {device})`; per frame on the
  immediate-context thread: `GetBuffer(0)` → `mpv_render_context_render({
  D3D11_TEX, TARGET_COLORSPACE })` → `Present(1, 0)`.
- Single-threaded UI+render loop; mpv's wakeup + render-update callbacks set
  atomics, the loop pumps events and renders. Live `ResizeBuffers` on `WM_SIZE`
  (no backbuffer ref held between frames).

This is the exact sequence a WinUI3 `SwapChainPanel` / Qt host ports (5b).

## §2. The load-bearing finding: the host must query the display peak

First run looked **wrong vs windowed**: the render-API output was strongly
saturated (pure BT.2020 red) where windowed showed a tone-mapped orange. Root
cause was **host-side, not mpv-side**: the host hardcoded
`TARGET_COLORSPACE.hdr.max_luma = 1000` (the *content's* mastering peak), so the
tone-mapper compressed 1000→1000 (≈ passthrough). The windowed path
(`--target-colorspace-hint=yes`) instead targets the **display's real peak** —
its passes showed `bt2390 tone map (1000 -> 353)`, i.e. this monitor's measured
peak is ~353 nits.

**Fix (host):** query `IDXGIOutput6::GetDesc1()` on the swapchain's containing
output and pass `MaxLuminance` / `MinLuminance` as `TARGET_COLORSPACE.hdr.
max_luma` / `min_luma`. After that, the render-API playback **matches windowed**
on the display. `max_cll`/`max_fall` are content-side (mpv reads them from the
clip) and are left unset in the target.

**This is the key 5b integration requirement:** any HDR render-API host
(WinUI3/Qt/Avalonia/…) must query the display's HDR caps and feed the real peak
into `TARGET_COLORSPACE` — the API is correct (it tone-maps to whatever the host
declares), but the host owns the display knowledge. It validates the API design:
`TARGET_COLORSPACE` is the right place for this, host-controlled.

## §3. Screenshots of HDR are not a valid comparison

The remaining "red vs orange" the user saw only appears in **Snipping Tool**
captures — Windows' OS-level HDR→SDR screenshot tone-maps poorly (blown
highlights, wrong colors). This is the same class of artifact as the Phase-4b
`ffprobe` finding (windowed screenshot written as sRGB, render-API as native
PQ): **HDR output only compares meaningfully in-pipeline** (mpv's own
`screenshot`, or a real ≥10-bit HDR readback), never through an OS SDR screen
grab. On the actual HDR display the two paths match.

A small residual difference may remain (windowed `hdr-compute-peak` /
swapchain-negotiation nuance — a few nits of tone-map target vs the host's
queried `MaxLuminance`); it is perceptually negligible and chase-able to
pixel-exact only if desired.

## §4. Status / next

- **D3D11 render-API HDR path: end-to-end confirmed** — objective (Phase 4b:
  `video-out-params` bit-identical, genuine PQ render, deterministic, windowed
  no-regression) **and** visual (Phase 5a: matches windowed on the HDR display).
- **5b (production shell):** port the §1 sequence into the chosen host (WinUI3
  `SwapChainPanel` via `ISwapChainPanelNative`, or Qt `QWindow`+manual-DXGI on
  `winId()` / `QQuickRhiItem`). Carry over the §2 display-peak query verbatim.
- Vulkan host equivalent (for Linux/macOS-via-MoltenVK) would mirror this with
  a `VkSwapchainKHR` + the V2.1 acquire/hold sync; same display-peak principle
  via the platform's HDR query.
