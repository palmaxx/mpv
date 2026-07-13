# Development harness sources

This branch keeps the reusable D3D11/Vulkan render-API harness sources and
scripts. Generated captures, raw frame dumps, compiled executables, candidate
directories, and media samples are deliberately excluded; they are release
evidence or rebuildable output, not source.

The authoritative Windows release evidence remains in the local
`_release/nongpl-win64-024e4c45/validation-logs` archive.

## macOS MoltenVK / VideoToolbox gate

`render_api/rapi_harness_vulkan.c` is also the macOS host harness. It creates a
real MoltenVK device, imports it into `MPV_RENDER_API_TYPE_PL_VULKAN`, renders
into a host-owned `VkImage`, and reads the pixels back. On macOS it enables the
required portability extensions; with `--hwdec`, it additionally requires the
Metal-object extension needed by the VideoToolbox texture interop path.

Build and probe it after a local `build-macos-minimal` libmpv build:

```sh
./_golden/render_api/build_vulkan.sh
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  ./_golden/render_api/rapi_harness_vulkan --hwdec --probe
```

For real media, copy `render_api/macos_cases.tsv.example` to a local file and
replace its fixture paths with clips from the mounted Plex share. Capture a
per-Mac baseline once, then run the same matrix after render-path changes:

```sh
./_golden/render_api/rapi_macos_run.sh --manifest ~/mpv-macos-cases.tsv --baseline
./_golden/render_api/rapi_macos_run.sh --manifest ~/mpv-macos-cases.tsv
```

The VideoToolbox cases fail unless `hwdec-current` contains `videotoolbox`, no
`HW-downloading` fallback was logged, and the readback is non-uniform. Raw
captures and local baselines are git-ignored. This headless gate validates the
libmpv render API and decode interop; ARCA must separately validate its
`CAMetalLayer` HDR/EDR presentation path.
