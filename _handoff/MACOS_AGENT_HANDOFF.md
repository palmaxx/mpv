# macOS agent handoff — current release source

## Start state

- Fork branch: `agent/macos-handoff`
- Audited source commit: `024e4c45f974e7dc418a4a32e1dbebd3aada588f`
- ARCA source: use the private ARCA release branch; its SwiftUI shell is still a
  stub and must be linked to the native core/libmpv on macOS.
- License target: build mpv/FFmpeg without GPL or nonfree inputs. ARCA remains
  proprietary; the dynamically loaded mpv/FFmpeg components retain LGPL terms.

## What already exists

The libmpv gpu-next render API supports `pl-vulkan`, imported host Vulkan
devices/images, explicit acquire/release semaphores and layouts, and target HDR
colorspace metadata. On macOS this means MoltenVK over Metal; libplacebo has no
separate native Metal backend.

The current Vulkan context creates an `ra_create_pl` wrapper over the imported
libplacebo GPU. That exposes the render device to mpv's hwdec registry.
`video/out/hwdec/hwdec_vt_pl.m` already maps VideoToolbox `CVPixelBuffer`
planes to `CVMetalTexture` and imports them with `PL_HANDLE_MTL_TEX`. This is the
intended zero-copy path. It is implemented but has never been built or tested
on this project’s Mac hardware.

## Minimum Mac work

1. Build the fork and an LGPL FFmpeg dependency set with Vulkan/MoltenVK,
   libplacebo, and VideoToolbox enabled; keep `gpl=false`.
2. Replace the SwiftUI player stub with the existing native ARCA core/libmpv
   boundary. Do not duplicate playback state in Swift.
3. Create one MoltenVK device for rendering and VideoToolbox interop, enabling
   the portability and Metal-object extensions required by the installed
   MoltenVK/libplacebo versions.
4. Render first in SDR, then configure `CAMetalLayer` HDR/EDR and pass the
   measured display headroom through `MPV_RENDER_PARAM_TARGET_COLORSPACE`.
5. Enable `hwdec=videotoolbox` and test the supplied HDR10, DV8.1, and genuine
   Profile 5 samples. Dolby Vision behavior is not claimed until these gates
   pass on the Mac.

## Acceptance evidence

- `hwdec-current=videotoolbox`.
- The VideoToolbox/libplacebo interop accepts `PL_HANDLE_MTL_TEX`; no
  `HW-downloading` or software-copy fallback appears in the log.
- The renderer retains hardware-backed frames through the render API.
- HDR/EDR output uses the requested BT.2020/PQ target and survives resize,
  pause/resume, seek, fullscreen, and display changes.
- Decoder and steady-state presentation drop counters remain flat.

The validated Windows D3D11VA path is a structural reference only. Its latest
Profile 5 run still reports `d3d11[p010]`, `hwdec-current=d3d11va`, Dolby
Vision/BT.2020/PQ, and output colorspace 12, but it does not prove the macOS
VideoToolbox/MoltenVK path.

## Rebase boundary

This release is based on upstream `33111f3212ee272ac4a79fe284a7b55c9b5be997`.
Upstream had advanced by 21 commits when this handoff branch was prepared. Do
not mix that rebase into the validated release commit; start the next rebase as
a separate planned branch and repeat the build/audit gates.
