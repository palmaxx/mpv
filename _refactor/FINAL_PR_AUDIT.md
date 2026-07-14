# Final PR audit — gpu-next through the libmpv render API

Audit date: 2026-06-19/20 (Europe/Rome)  
Fork PR: https://github.com/palmaxx/mpv/pull/1  
Branch: `gpu-next-render-api-hdr`  
Candidate: `a8c7cfe7812e3d828eb603397860500a1569c512`  
Local follow-up: `f728638bc1 gpu_next: advertise render-api hwdec formats`  
Upstream base: `2d5dfb343aeac72e4ee7303dbf34347a0ff4425d`  
Range: 71 linear commits, 16 files

## Decision

The candidate remains the frozen GitHub review snapshot. The shared-core
extraction remains equivalent to the upstream windowed gpu-next path, while the
new render backend adds private libplacebo OpenGL, D3D11, and Vulkan target
wrappers without exposing libplacebo types in the public API.

Post-freeze ARCA validation found one additional D3D11VA integration defect:
the render backend initialized the hwdec registry but `check_format()` did not
advertise its hardware formats. mpv therefore reported `hwdec-current=d3d11va`
while inserting `HW-downloading from d3d11`, causing sustained 4K60 VO drops.
Local follow-up `f728638bc1` mirrors the windowed frontend's `ra_hwdec_get()`
format check. It restores zero-copy `d3d11[p010]` through `video-out-params`;
the fix is validated locally but is not pushed into the frozen GitHub snapshot.

Three fixes were added during this audit:

1. `2009f693d6 render_vulkan: clarify current image layout`
2. `8b1475a111 gpu_next: clean up partial hwdec acquisition`
3. `a8c7cfe781 render_vulkan: document queue-family sharing`

No other code defect was found. The remaining limitations are validation or
platform scope, not hidden implementation claims: no WSL installation was
available for the Linux/lavapipe rerun, macOS VideoToolbox-over-MoltenVK is not
validated, and true FFmpeg Vulkan Video decoding is not implemented.

## Scope and upstream state

- The audit and all tracked mpv changes use only `_upload/mpv`.
- Planning, captures, harness output, and this document remain outside that
  worktree.
- `upstream/master` was fetched on 2026-06-19 and again before freezing on
  2026-06-20. It still resolved to the exact
  branch base, `2d5dfb3`; the branch was 68 ahead and 0 behind before the three
  audit commits. No rebase was warranted.
- The recent upstream fixes moved out of `vo_gpu_next.c` are present in the
  shared core/frontends: paused-frame caching, interpolation-preserve cache
  behavior, upload callback synchronization, render-cache flushing, still-frame
  underrun logging, and scRGB dithering.
- Monitored upstream PRs #18073, #18123, and #17394 remained open/unmerged and
  were not ported.
- Competing PRs #16818, #17764, and #17828 remained open.

## CodeRabbit finding ledger

| Finding | Audit result | Disposition |
|---|---|---|
| Vulkan `current_layout` called a release layout | Valid documentation defect | Fixed by `2009f693d6`; it is the host's current/acquire-side layout passed to `pl_vulkan_release_ex`. |
| Partial hwdec plane-wrap leak | Valid major defect | Fixed by `8b1475a111`; owned non-`ra_pl` wrappers are destroyed, every plane texture pointer is cleared, the mapper is unmapped, and the timer is stopped. |
| `vo->params->w/h` are window dimensions | False positive | `vo.h` defines `vo->params` as configured video parameters; `vo.c` copies `img->params` into it during reconfiguration. The exact source-dimension use predates the extraction and is preserved. |
| LUT `.path` versus `.opt` comparison | Intentional | The old applied/resolved path is captured before options update and compared with the newly requested option to decide whether queued frames must be reset. The core then updates `.path` from `.opt`. |
| `!bstr_equals0(...) != 0` | Real cosmetic oddity, unrelated | Introduced upstream in 2022 (`33136c276c`) and moved byte-for-byte. It is not mixed into this extraction. |
| Docstring coverage | Inapplicable generic check | No C docstring/comment pollution added. Public API contracts are documented where required. |

After the audit commits were pushed, CodeRabbit accepted both fixes, withdrew
both false positives, recorded the two deliberate non-changes, and completed a
fresh incremental review without new findings.

## Extraction-equivalence map

The extraction is a staged move from upstream `vo_gpu_next.c` into
`gpu_next/core.{c,h}`. Each item below retains the upstream ordering and
behavioral invariants; frontend-only swapchain, VO, stats, and option-cache
state remains in `vo_gpu_next.c`.

| Original responsibility/state | Shared-core destination | Preserved invariant |
|---|---|---|
| DR buffer array/lock and `get_image` | `gpu_next_core_get_dr_buf`, `_get_image`, `free_dr_buf` | Queue destruction returns frame refs before the zero-buffer teardown assertion. |
| libplacebo options, renderer, queue | `gpu_next_core_create/destroy`, `_options`, `_queue` | Cache is installed before renderer creation; queue dies before mapper/renderer/GPU. |
| render call and HDR metadata query | `_render_mix`, `_render_image`, `_get_hdr_metadata` | `pl_frames_infer_mix` still runs only after successful rendering and retains its metadata side effects. |
| software plane upload | `_plane_data_from_imgfmt`, `_upload_sw_planes` | Async callbacks retain each image; DR buffers are polled when callbacks are unavailable. |
| hwdec mapper and plane wrapping | `_hwdec_reconfig/acquire/release` | `ra_pl` textures are borrowed; GL/D3D11 wrappers are owned and destroyed; mapper map/unmap remains paired. |
| shader/LUT/ICC caches | `_load_hook`, `_load_lut`, `_update_lut`, `_update_icc*` | Applied path tracking and renderer cache invalidation remain unchanged. |
| OSD texture pool/overlay generation | `_sub_tex_push/pop`, `_update_overlays` | Per-frame subtitle textures return to the core pool on unmap; target OSD cache remains frontend-owned. |
| source crop, rotation, blended subtitles | `_apply_crop`, `_update_frames` | Unrotated source dimensions remain the configured video dimensions; target crop uses target texture dimensions. |
| render-option resolution | `_update_render_options`, `_update_options` | Paused/interpolation-preserve cache rules and user hook parameter behavior are retained. |
| target color/ICC/dither/hint logic | `_apply_target_options`, `_finalize_target_csp`, `_target_hint` | Windowed swapchain precedence is unchanged; the render backend supplies an equivalent host baseline. |
| screenshot and perf data | `_screenshot`, `_get_perf_data` | Windowed fallback depth/alpha behavior is retained; render API intentionally lacks swapchain depth/alpha intent. |

### Intentional behavior differences

- The render frontend wraps a host target instead of starting a swapchain
  frame, and the host presents it.
- A host-supplied `TARGET_COLORSPACE` replaces swapchain target negotiation.
  Omitted/all-zero values preserve the prior render-API sRGB behavior.
- The render frontend has no VO stats context, so frontend timing hooks are
  null; libplacebo pass data remains available.
- Screenshot fallback depth is 0 and alpha is disabled because no swapchain
  contract provides them.
- The D3D11/Vulkan frontends provide an RA only for hwdec interop; rendering
  remains entirely through the imported libplacebo GPU.

No unexplained windowed behavior difference remains.

## Ownership and failure-path audit

| Resource | Acquire/create | Release/destroy | Failure-path result |
|---|---|---|---|
| Per-call host `pl_tex` wrap | `wrap_fbo` | render hook after flush + successful `done_frame` | Wrap failure returns directly. Acquire failure destroys a still-host-held wrap. Vulkan hand-back failure transfers ownership to `orphaned_fbo`. |
| Vulkan target control | `pl_vulkan_release_ex` | `pl_vulkan_hold_ex` | `released` remains true on failed hold; next render or teardown retries. A permanently released wrapper is deliberately leaked rather than destroying an image in undefined state. |
| D3D11 target COM reference | `pl_d3d11_wrap` | `pl_tex_destroy` before render returns | Dimension mismatch destroys immediately. Refcount harness confirms the host is sole owner between calls, allowing `ResizeBuffers`. |
| Imported libplacebo GPU | `pl_opengl_create`, `pl_d3d11_create`, `pl_vulkan_import` | corresponding backend destroy | Core and OSD textures are destroyed first. Host device/instance ownership is retained by the host. |
| `ra_ctx`/RA/SPIR-V | GL context init, D3D11 RA + compiler, Vulkan `ra_create_pl` | hwdec registry, then RA/compiler/backend teardown | Partial D3D11 RA setup falls back to software decode without destroying the libplacebo device. Backend RA destructor is called once. |
| hwdec registry/devices | render-backend init | after core/queue, before RA | Queue unmap callbacks run while mapper, registry, and RA are alive. |
| hwdec mapper | `_hwdec_reconfig` | format change or core destroy | Map failure stops timer. Plane-wrap failure now destroys owned partial wraps, clears pointers, unmaps, and stops timer. |
| software upload refs | `mp_image_new_ref` per callback | libplacebo callback or failure cleanup | Failed current plane drops its callback ref; prior successful async planes retain theirs until completion. |
| frame queue | `pl_queue_create/push` | reset or core destroy | Queue dies first and invokes unmap/discard callbacks before mapper/DR teardown. |
| renderer/options/caches | core create | renderer/options, then cache objects | GPU is alive throughout. Existing upstream cache-pointer teardown window is unchanged and no dispatch occurs in it. |
| DR buffers | `pl_buf_create` + mp_image callback | `free_dr_buf` | Protected by `dr_lock`; teardown asserts all image refs were released by queue destruction. |
| OSD textures | recreate/pop pool | per-frame pool return; frontend target OSD destroy; core pool destroy | Upload failure frees the retained packed-image ref. Existing texture remains valid for later reuse. |
| timers | lazy create/reconfig | core destroy/reconfig | Start/stop pairs cover successful and failed upload/map paths. |

## Public API and ABI

- `MPV_RENDER_PARAM_TARGET_COLORSPACE=21`, D3D11 params 22/23, and Vulkan
  params 24/25 are explicit append-only values.
- Client API is 2.7; 2.6 records PL_OPENGL and 2.7 records D3D11, Vulkan,
  target colorspace, and target-wrap lifetime.
- `render_d3d11.h` and `render_vulkan.h` are installed headers.
- Strict C11 and C++17 translation units including all three render headers
  compile with `-Wall -Wextra -Werror`.
- D3D11 uses `void *` handles for public-header portability and documents COM,
  immediate-context threading, texture bind/usage, and resize lifetime.
- Vulkan uses Vulkan's public handle/types directly and documents imported
  device lifetime, queue serialization, image acquire/release semaphores,
  current/final layouts, and required queue-family sharing.
- `a8c7cfe781` closes an audit gap: because the implementation deliberately
  passes `VK_QUEUE_FAMILY_IGNORED`, targets exposed to multiple imported queue
  families must be `VK_SHARING_MODE_CONCURRENT`; exclusive targets must expose
  only one family.
- Public mpv color enums are numerically independent from libplacebo and are
  translated by exhaustive explicit switches.
- The direct future conflict with #17764 is acknowledged: that proposal adds a
  legacy D3D11 render API with overlapping problem space but a different public
  surface. This frozen fork does not preemptively rename or redesign its API.

## Rendering and color correctness

The CodeRabbit sequence is accurate with one ownership nuance: `wrap_fbo`
returns a caller-owned wrapper; after render/flush, Vulkan `done_frame` either
hands it back so the caller destroys it or moves it into the backend's owning
orphan slot.

Trace:

1. Host target is wrapped and dimensions/capabilities are validated.
2. Vulkan releases control to libplacebo, waiting on the acquire semaphore in
   `current_layout`; GL/D3D11 require no explicit acquisition.
3. Source frames enter the same queue/map path as the windowed frontend.
4. Target colorspace starts from host metadata when present, otherwise sRGB.
5. User target options fill only unpinned fields when the host target is known.
6. ICC, HDR contrast/peak, bit depth, crop, rotation, OSD, LUTs, and dynamic
   hooks run through the shared core.
7. Render completes, GPU commands flush, and Vulkan transitions to
   `final_layout` before signaling the release semaphore.
8. Successful wrappers are destroyed before returning to the host.

The render-API D3D11 and Vulkan matrices prove:

- all-zero sRGB target metadata is identical to omission;
- BT.2020/PQ changes output;
- 10-bit PQ differs from SDR output;
- D3D11 and Vulkan candidate pixels and metadata match preserved baselines;
- windowed SW and d3d11va HDR captures are byte-identical to the pristine
  baseline (`46CDAFFF48CFFFA4...`) and preserve negotiated metadata.

Dolby Vision 8.1 through ARCA retains `colormatrix=dolbyvision`, BT.2020/PQ,
mastering primaries, luminance metadata, and matching `video-out-params`.

Capability statement:

- D3D11VA: the device/mapper bridge is implemented, but frozen SHA `a8c7cfe781`
  omits hwdec formats from render-backend `check_format()` and falls back to a
  CPU download. Local `f728638bc1` fixes the advertisement and is validated on
  a real NVIDIA device (`video-out-params=d3d11[p010]`, zero decoder drops).
- Vulkan: exposes a generic `ra_pl` over the imported libplacebo GPU for
  compatible mpv hwdec interops; the target/synchronization path is validated.
- VideoToolbox-over-MoltenVK: structurally possible but unvalidated on macOS.
- FFmpeg Vulkan Video decoding: unsupported/deferred; no claim is made.

## Concurrency and synchronization

- libmpv render calls are host-synchronized by the existing render API
  contract; option/update callbacks do not mutate target ownership concurrently.
- D3D11 immediate-context access must be same-thread or host-mutex serialized.
- Vulkan host queues must not race render calls. Image semaphores synchronize
  image use, not arbitrary host access to the imported queues.
- A render failure after Vulkan acquisition still flushes and executes
  `done_frame`, so the host gets the image back or receives an explicit error
  with the wrapper retained for recovery.
- An acquisition-parameter error occurs before control transfer and destroys
  the still-held wrapper without calling `done_frame`.
- D3D11 wrapper destruction occurs before return; the refcount/resize invariant
  is proven by the WARP harness and ARCA's live `ResizeBuffers` test.
- Core destruction calls `pl_gpu_finish`, drains the queue, destroys core/OSD
  GPU objects, uninitializes hwdec, then destroys RA and libplacebo contexts.

## Build guards and portability

- Source inclusion and C guards agree for GL, D3D11, and Vulkan.
- The backend table always ends in `NULL`, so a build with none of the three
  surface APIs compiles and cleanly reports not implemented.
- UCRT64 full build with GL+D3D11+Vulkan: pass.
- UCRT64 GL-only build: pass.
- UCRT64 no-GL/no-D3D11/no-Vulkan build: pass after disabling unrelated
  auto-enabled Windows D3D hwaccel/VAAPI features. The first attempt exposed an
  upstream configuration issue in `d3d11_helpers.h`, not a fork source error.
- No unconditional Windows/Vulkan headers escape their Meson/C guards.
- macOS compile/runtime validation remains required; no result is inferred from
  Windows.

## Style and history

- `git diff --check 2d5dfb3..HEAD`: pass.
- `ci/lint-commit-msg.py 2d5dfb3..HEAD`: all 71 commits pass.
- History is linear with no merge commits.
- No unused `gpu_next_core_*` public wrappers were found.
- No planning-phase references remain in source comments. Comments retained in
  new backend code explain ownership, synchronization, ABI, or platform
  contracts; mechanical comments inherited from upstream were not churned.
- clang-format was not run, per policy.
- `pre-commit`, `editorconfig-checker`, `uncrustify`, and `clang-tidy` are not
  installed in this environment. Equivalent whitespace/EOL checks and compiler
  warning builds passed locally; GitHub's pre-commit and editorconfig checks
  also passed. Uncrustify and clang-tidy are not represented as run.

## Verification record

### Passed in this audit

- UCRT64 incremental release build.
- Meson tests: 35/39 directly; the four FFmpeg reference tests differed only
  because `autocrlf` changed checked-out fixtures to CRLF. All four pass against
  LF-clean blobs extracted from Git, giving an effective 39/39 source result.
- Strict public-header compile in C11 and C++17.
- Full, GL-only, and no-surface-API build matrices.
- WARP D3D11 render API: 6/6 pixel+metadata baseline matches, HDR/no-op
  invariants, and target COM-refcount test.
- NVIDIA Vulkan render API: context probe plus SDR, all-zero sRGB, HDR10/PQ
  R10G10B10A2, and PQ RGBA16F; all pixels+metadata match the preserved baseline.
- NVIDIA D3D11VA render API at the frozen SHA: decoder engaged, but later ARCA
  verbose validation proved the frame path was CPU-downloaded; the original
  probe was insufficient to claim zero-copy.
- Local follow-up `f728638bc1`: ARCA DV8.1 4K24 retains `d3d11[p010]` through
  `video-out-params`, seek/resize pass, zero decoder/VO drops. DV8.1 4K60 has
  zero decoder drops and a finite 17-frame warmup/resize burst, flat at exit.
- Windowed D3D11 gpu-next SW and d3d11va: two runs each, pixels+metadata
  byte-identical to preserved real-HDR baseline; hardware logs confirm d3d11va.
- ARCA re-vendor/build at this candidate; `lib-verify` 27/27.
- ARCA HDR10 4K24 and synthetic DV8.1 4K24: HDR active, seek pass, resize pass,
  zero decoder drops, zero steady-state VO drops.
- ARCA synthetic DV8.1 4K60: seek/resize pass, zero decoder drops, 76 VO drops
  during shader warmup/resize, then a flat counter from 8 through 12 seconds.
- GitHub Actions: Linux GCC/Clang, Windows MSYS2/MinGW, Windows x64/ARM64 MSVC,
  macOS, FreeBSD, OpenBSD, lint, documentation, pre-commit, editorconfig, and
  in-tree Linux fuzz builds passed. The three OSS-Fuzz jobs failed before
  testing this candidate because CIFuzz cloned `mpv-player/mpv` and fetched
  `refs/pull/1/merge` there (commit `5ff36d6e...`) instead of this fork PR
  (`a8c7cfe781...`); this is the known fork-PR ref collision.

### Environment-blocked or deferred

- WSL GCC/Clang, Linux Meson, lavapipe windowed 12-case golden, teardown 9-case
  matrix, and Linux PL_OPENGL/Vulkan harnesses: blocked because this Windows
  environment has no WSL distribution installed. Preserved earlier evidence is
  informative but is not presented as a fresh rerun.
- Generic OpenGL render-API matrix: same WSL blocker; GL-only compilation passes.
- Pre-commit/editorconfig: unavailable locally but passed in GitHub Actions.
- Uncrustify/clang-tidy: tools unavailable locally.
- macOS MoltenVK/VideoToolbox: deferred to the post-freeze macOS phase.
- True Vulkan Video: intentionally not implemented.

## Maintainer-facing summary

### Problem

libmpv hosts need the gpu-next/libplacebo renderer without duplicating
`vo_gpu_next.c`, leaking libplacebo into the public ABI, or forcing HDR-capable
Windows/macOS/Linux hosts through an SDR-oriented OpenGL FBO path.

### Why the alternatives are insufficient

- #16818 was rejected because it did not provide the requested shared-core
  architecture.
- #17828 exposes libplacebo types/contracts publicly, binding libmpv ABI to an
  external renderer library.
- #17764 adds a legacy D3D11 renderer path but does not provide gpu-next,
  target HDR negotiation, or a Vulkan sibling.

### Architecture and invariants

- One `gpu_next_core`, two frontends: windowed swapchain and libmpv host target.
- Per-API wrappers are narrow and private.
- Host targets are wrapped per call and not retained after successful return.
- Core GPU resources die before the imported/created GPU context.
- Queue frames die before mapper/hwdec/RA resources.
- Vulkan image control is always host-held or explicitly retained for recovery;
  a released wrapper is never destroyed.

### Public API rationale

- Backend strings select implementation without changing legacy APIs.
- Target colorspace is orthogonal to graphics API and uses stable mpv enums.
- D3D11 and Vulkan descriptors state host ownership and synchronization
  explicitly.
- No libplacebo public type crosses the ABI.

### Known limitations

- macOS VideoToolbox-over-MoltenVK needs real hardware validation and likely
  `VK_EXT_metal_objects` integration constraints.
- Vulkan RA availability is not a claim of Vulkan Video decode support.
- Render-API screenshot alpha/depth differs from windowed behavior because the
  host provides no such intent.
- Linux fresh runtime verification is still required before upstream submission.
- #17764 may force an API naming/design reconciliation if merged first.

### Likely objections and factual answers

| Objection | Answer |
|---|---|
| “This duplicates gpu-next.” | Rendering, queueing, color, OSD, LUT, screenshot, upload, and hwdec mapping are shared; only frontend/surface mechanics differ. |
| “libplacebo becomes ABI.” | It does not. Public enums/structs are mpv-owned and explicitly translated. |
| “Backbuffers stay referenced.” | Per-call wrappers are destroyed before return; WARP refcount and live ARCA resize tests pass. |
| “Vulkan ownership is unsafe.” | Acquire/release semaphore and layout contracts are paired; failed hand-back retains/retries the wrapper; queue-family sharing is now explicit. |
| “The windowed VO changed behavior.” | The staged extraction preserves source dimensions and control flow; real-HDR SW/HW captures are byte-identical to the preserved baseline. |
| “Vulkan means Vulkan Video.” | No. It means a Vulkan render target and generic imported libplacebo RA; Vulkan Video decode is deferred. |
| “Why 71 commits?” | The history intentionally exposes small extraction steps and validation boundaries. Upstream polishing/squashing is a separate, post-review branch. |

### Recommended review order

1. Public ABI: `render.h`, `render_d3d11.h`, `render_vulkan.h`, version/docs.
2. Private surface contract: `libmpv_gpu_next.h` and the three backend files.
3. Render frontend lifecycle: `libmpv_gpu_next.c`, especially init/render/destroy.
4. Extraction equivalence: `vo_gpu_next.c` against `gpu_next/core.{c,h}`.
5. Meson guards and backend registration.
6. Audit commits and CodeRabbit resolutions.

## Freeze record

Candidate SHA: `a8c7cfe7812e3d828eb603397860500a1569c512`.

This SHA is the frozen GitHub review snapshot. It was pushed without rewriting
history and reviewed by CodeRabbit. Local branch `local/d3d11va-format-fix`
contains additive commit `f728638bc1`; do not move the GitHub snapshot until
the user decides whether to add that commit after reviewing the current diff.
Any upstream-polish history rewrite belongs on a new branch after explicit
approval.
