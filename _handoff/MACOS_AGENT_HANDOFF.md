# macOS agent handoff — current release source and Mac validation state

## Start state

- Fork branch: `agent/rebase-upstream-20260714-handoff`
- Audited source commit: `4347588dea` on `agent/rebase-upstream-20260714`
- ARCA source: use the private ARCA release branch; its SwiftUI shell is still a
  stub and must be linked to the native core/libmpv on macOS.
- License target: build mpv/FFmpeg without GPL or nonfree inputs. ARCA remains
  proprietary; the dynamically loaded mpv/FFmpeg components retain LGPL terms.

### macOS worktrees after syncing the candidate

| Checkout | Branch | Purpose |
|---|---|---|
| `/Users/leo/depy/mpv` | `agent/rebase-upstream-20260714-handoff` | Mac libmpv build and validation harness worktree. |
| `/Users/leo/depy/arca` | `agent/arca-nongpl-release` | ARCA source to port next. Its macOS shell remains a SwiftUI stub. |

The harness source is tracked. Local build material remains under `_deps/` and
`build-macos-minimal/`; keep it unless a clean rebuild is intended.

## What already exists

The libmpv gpu-next render API supports `pl-vulkan`, imported host Vulkan
devices/images, explicit acquire/release semaphores and layouts, and target HDR
colorspace metadata. On macOS this means MoltenVK over Metal; libplacebo has no
separate native Metal backend.

The current Vulkan context creates an `ra_create_pl` wrapper over the imported
libplacebo GPU. That exposes the render device to mpv's hwdec registry.
`video/out/hwdec/hwdec_vt_pl.m` already maps VideoToolbox `CVPixelBuffer`
planes to `CVMetalTexture` and imports them with `PL_HANDLE_MTL_TEX`. This is the
intended zero-copy path.

It is now compiled and functionally validated on the project Apple M1 Mac
through the headless libmpv Vulkan render API gate. The test imports one
MoltenVK device into `MPV_RENDER_API_TYPE_PL_VULKAN`, enables the macOS
portability and Metal-object extensions, decodes a real mounted-Plex 4K HEVC
Main 10 HDR fixture with `hwdec=videotoolbox`, renders into a host `VkImage`,
and reads that target back. The resulting sidecar reports:

```text
pixelformat=videotoolbox
hwdec_current=videotoolbox
hw_downloading=0
decoder_frame_drop_count=0
non_uniform=1
```

The mpv log confirms `Using hardware decoding (videotoolbox)` and
`videotoolbox[p010]` with Dolby Vision / BT.2020 / PQ source metadata. This
proves the render-API decode/import path is live, not merely compiled. It does
not prove ARCA's future `CAMetalLayer` presentation, display EDR, Dolby Vision
output policy, resize, or display-change handling.

## Local Mac toolchain and artifacts

Host: Apple M1 (`arm64`), macOS 26.5.1, Command Line Tools SDK 26.2, Swift
6.2.4. Full Xcode is not required for the current terminal build or gate.

Installed Homebrew build dependencies:

- CMake 4.4.0, Ninja 1.13.2, Meson 1.11.2, pkgconf 3.0.1.
- libass 0.17.5, libplacebo 7.360.1, Vulkan loader/headers 1.4.350.1,
  MoltenVK 1.4.1.

Local, non-GPL build products in this worktree:

| Location | Contents |
|---|---|
| `_deps/src/ffmpeg-8.1.1.tar.xz` | Pinned FFmpeg source; SHA-256 matches ARCA's recorded `b6863a...c52edf3`. |
| `_deps/build/ffmpeg-8.1.1/` | ARM64 FFmpeg 8.1.1 build tree. |
| `_deps/prefix/` | Shared FFmpeg dylibs, headers, and pkg-config files. Configured with GPL, nonfree, version3, avdevice, programs, and docs disabled; VideoToolbox and AudioToolbox enabled. |
| `build-macos-minimal/` | Meson build directory for the fork. |
| `_deps/mpv-install/` | Installed local libmpv SDK: `lib/libmpv.2.dylib`, public headers including `render_vulkan.h`, and `mpv.pc`. |

The local libmpv build is `arm64`, `gpl=false`, `libmpv=true`, `cplayer=false`,
`vulkan=enabled`, `videotoolbox-pl=enabled`, Cocoa/Swift enabled, and OpenGL,
Lua, JavaScript, curl, libavdevice, and rubberband disabled. `mpv_initialize()`
returns success when dynamically loading the built dylib.

This is a development SDK, not an ARCA distributable package yet. Its FFmpeg
dylibs still carry local build paths and the Homebrew libass/libplacebo/Vulkan
closure is not vendored. The ARCA packaging phase must copy that closure,
rewrite install names/rpaths, include the MoltenVK ICD, and preserve third-party
notices before shipping.

### Rebuild commands

FFmpeg has already been configured in `_deps/build/ffmpeg-8.1.1`; rebuild and
install it with:

```sh
make -C _deps/build/ffmpeg-8.1.1 -j8
make -C _deps/build/ffmpeg-8.1.1 install
```

Configure or refresh libmpv with:

```sh
PKG_CONFIG_PATH="$PWD/_deps/prefix/lib/pkgconfig" \
meson setup build-macos-minimal --reconfigure \
  --prefix="$PWD/_deps/mpv-install" \
  -Dgpl=false -Dcplayer=false -Dlibmpv=true \
  -Dcocoa=enabled -Dswift-build=enabled -Dgl=disabled \
  -Dvulkan=enabled -Dvideotoolbox-pl=enabled \
  -Dlibavdevice=disabled -Drubberband=disabled \
  -Dlua=disabled -Djavascript=disabled -Dlibcurl=disabled \
  -Dtests=false -Dbuild-date=false
ninja -C build-macos-minimal osdep/mac/swift.h
meson compile -C build-macos-minimal
meson install -C build-macos-minimal
```

The explicit `swift.h` target is a useful workaround after reconfiguration:
otherwise this branch can schedule the Objective-C clipboard source before the
generated Swift header. The normal interactive developer environment can write
Swift's module cache; the tool sandbox needed elevated permission for that
cache during the initial build.

## macOS render regression gate

The reusable source is `_golden/render_api/rapi_harness_vulkan.c`. It now:

- detects and enables `VK_KHR_portability_enumeration` /
  `VK_KHR_portability_subset` on macOS;
- enables `VK_EXT_metal_objects` for `--hwdec` cases;
- renders through the fork's `pl-vulkan` backend into a host `VkImage`;
- makes `--hwdec` fail unless VideoToolbox engages, no `HW-downloading`
  fallback appears, and the result contains real pixels.

Build and probe the current host:

```sh
./_golden/render_api/build_vulkan.sh
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  ./_golden/render_api/rapi_harness_vulkan --hwdec --probe
```

For real-video regression checks, the local ignored manifest
`_golden/render_api/macos_cases.tsv` describes tab-separated cases. It maps SMB
paths such as `smb://192.168.1.28/PLEX/<file>` to the already-mounted path
`/Volumes/PLEX/<file>`. The current local manifest contains the private Plex
HDR fixture used for the result above. Capture a baseline after intentionally
changing rendering behavior, otherwise verify after every render-path change:

```sh
./_golden/render_api/rapi_macos_run.sh \
  --manifest _golden/render_api/macos_cases.tsv --baseline
./_golden/render_api/rapi_macos_run.sh \
  --manifest _golden/render_api/macos_cases.tsv
```

The harness is deliberately headless, so no player window appears. It is a
pixel/decode-interoperability regression test, not an HDR display test. Raw
captures, logs, the local manifest, and baselines are ignored; the template is
`_golden/render_api/macos_cases.tsv.example`. The next ARCA host must add a
separate visible `CAMetalLayer` EDR validation gate.

## Minimum Mac work

1. **Complete locally.** Build the fork and LGPL FFmpeg dependency set with
   Vulkan/MoltenVK, libplacebo, and VideoToolbox enabled; keep `gpl=false`.
2. Replace the SwiftUI player stub with the existing native ARCA core/libmpv
   boundary. Do not duplicate playback state in Swift.
3. **Proven in the headless gate.** Create one MoltenVK device for rendering
   and VideoToolbox interop, enabling the portability and Metal-object
   extensions required by the installed MoltenVK/libplacebo versions. Reuse the
   same single-device rule in ARCA.
4. Render first in SDR, then configure `CAMetalLayer` HDR/EDR and pass the
   measured display headroom through `MPV_RENDER_PARAM_TARGET_COLORSPACE`.
5. Enable `hwdec=videotoolbox` and test the supplied HDR10, DV8.1, and genuine
   Profile 5 samples. Dolby Vision behavior is not claimed until these gates
   pass on the Mac.

## Acceptance evidence

- **Passed on the current Mac gate:** `hwdec-current=videotoolbox`.
- **Passed on the current Mac gate:** VideoToolbox/libplacebo interop retains
  `videotoolbox[p010]`; no `HW-downloading` or software-copy fallback appears
  in the log.
- **Passed on the current Mac gate:** the renderer retains hardware-backed
  frames through the render API and produces non-uniform readback pixels.
- HDR/EDR output uses the requested BT.2020/PQ target and survives resize,
  pause/resume, seek, fullscreen, and display changes. **Still pending the
  visible ARCA `CAMetalLayer` host.**
- Decoder drop count was `0` for the one-frame current gate. Steady-state
  presentation drop counters remain pending the ARCA render loop.

The validated Windows D3D11VA path is a structural reference only. Its latest
Profile 5 run still reports `d3d11[p010]`, `hwdec-current=d3d11va`, Dolby
Vision/BT.2020/PQ, and output colorspace 12, but it does not prove the macOS
VideoToolbox/MoltenVK path.

## Rebase boundary

The candidate is based on upstream `e5486b96d7d06dd148337899bfdc46bf25101663`,
including the 21 commits that followed the previous validated release base.
Windows builds and source audits pass; repeat the macOS build and MoltenVK /
VideoToolbox gates before promoting the candidate to `main`.
