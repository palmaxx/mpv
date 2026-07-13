# Phase 0 — Feasibility Report: HDR over the libmpv render API
## D3D11 context-fns + `MPV_RENDER_PARAM_TARGET_COLORSPACE`

**Status:** GO — confidence MEDIUM — no kill-points identified.

**Scope:** assess feasibility of extending the W5-6 + W6 libmpv render-API
surface (`MPV_RENDER_API_TYPE_PL_OPENGL`, validated bit-faithfully through
`c8e9298` on real NVIDIA D3D11 HDR hardware) with (1) a parallel
`MPV_RENDER_API_TYPE_PL_D3D11` context-fns backend that wraps a
host-provided `ID3D11Texture2D` as a `pl_tex`, and (2) a new
`MPV_RENDER_PARAM_TARGET_COLORSPACE` render param that lets the host
inform mpv about an HDR target surface.

**Branch:** `gpu-next-render-api-hdr` off `c8e9298` (W6b tip).

---

## §0. Methodology + verification status

This report was produced in two passes:

1. A read-only AI subagent was dispatched (per [plan-hdr-render-api.md](plan-hdr-render-api.md)
   §"Phase 0 — AI feasibility check") to audit the candidate change.
2. The subagent was sandboxed — `Bash`, `WebFetch`, `WebSearch`, and
   `Read` against paths outside the mpv tree were denied — and so its
   §1 (libplacebo signatures) and §3 (host-framework audit) had to be
   reconstructed from API knowledge rather than direct header reads.
   The agent flagged this in its own caveat block.
3. **The parent (this doc's author) then verified the bounded claims
   first-hand**: read [libplacebo/d3d11.h](/home/maxde/libplacebo/src/include/libplacebo/d3d11.h),
   [libplacebo/colorspace.h](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h),
   `git show e7f2ffa` (W4 commit), [include/mpv/render.h](include/mpv/render.h),
   [include/mpv/client.h](include/mpv/client.h), [DOCS/client-api-changes.rst](DOCS/client-api-changes.rst),
   [meson.build](meson.build) (D3D11 + libplacebo feature-detection
   patterns). Where the agent's reconstructed claims diverged from
   ground truth, this doc carries the verified version with a
   ⚠️ marker noting the correction.

**Verification status by section:**

| Section | Verification |
|---|---|
| §1 libplacebo D3D11 audit | ✅ first-hand from in-tree libplacebo 7.360.1 headers |
| §2 mpv render-API ABI hygiene | ✅ first-hand from render.h / client.h / W4 commit |
| §3 Host HDR surface feasibility | ⚠️ direction-of-truth (agent could not WebFetch); architectural verdict is firm, framework version pins should be re-confirmed in Phase 5b |
| §4 TARGET_COLORSPACE design | ✅ enum table cross-checked against libplacebo colorspace.h |
| §5 Top-5 hazards | ✅ first-hand from existing mpv source |
| §6 Go / No-Go | ✅ synthesized from verified §§1-5 |

---

## §1. libplacebo D3D11 backend audit (verified)

All signatures below are quoted directly from
[/home/maxde/libplacebo/src/include/libplacebo/d3d11.h](/home/maxde/libplacebo/src/include/libplacebo/d3d11.h)
in the in-tree libplacebo **7.360.1**.

### 1a. The structural template

The existing GL context-fns implementation
([video/out/opengl/libmpv_pl_gl.c](video/out/opengl/libmpv_pl_gl.c)) is
the proven shape `libmpv_pl_context_d3d11.c` mirrors 1:1. Its lifecycle:

- `init()` reads its API-specific init params, calls `pl_opengl_create`,
  stores the result, populates `ctx->pllog` + `ctx->gpu` so the shared
  `gpu_next_core_create` can consume them ([libmpv_pl_gl.c:54-68](video/out/opengl/libmpv_pl_gl.c#L54)).
- `wrap_fbo()` reads its API-specific FBO descriptor, calls
  `pl_<api>_wrap`, frees the previous wrap via `pl_tex_destroy`
  ([libmpv_pl_gl.c:82-86](video/out/opengl/libmpv_pl_gl.c#L82)).
- `done_frame()` is a no-op (no swapchain; host owns presentation).
- `destroy()` runs `pl_tex_destroy` → `pl_<api>_destroy` →
  `pl_log_destroy`.

The D3D11 backend swaps `pl_opengl_*` → `pl_d3d11_*` and
`mpv_opengl_init_params/mpv_opengl_fbo` →
`mpv_d3d11_init_params/mpv_d3d11_tex`. Concrete `priv` shape:

```c
struct priv {
    pl_d3d11 pl_d3d11;
    pl_tex   wrapped_fbo;
};
```

### 1b. `pl_d3d11_create()` — verified

```c
PL_API pl_d3d11 pl_d3d11_create(pl_log log, const struct pl_d3d11_params *params);
PL_API void     pl_d3d11_destroy(pl_d3d11 *d3d11);
PL_API pl_d3d11 pl_d3d11_get(pl_gpu gpu);  // accessor: pl_gpu -> pl_d3d11

struct pl_d3d11_params {
    // If non-NULL, libplacebo wraps the existing device and ALL options
    // below are ignored. If NULL, libplacebo creates its own device.
    // "If an existing device is provided in params->device,
    //  `pl_d3d11_create` will take a reference to it that will be
    //  released in `pl_d3d11_destroy`."  (d3d11.h:139-140)
    ID3D11Device *device;

    // --- Adapter selection (only consulted when device == NULL) ---
    IDXGIAdapter *adapter;      // overrides adapter_luid
    LUID          adapter_luid; // ⚠️ struct LUID, not int (header line 56)
    bool          allow_software;
    bool          force_software;

    // --- Device creation options ---
    bool debug;
    bool no_compute;            // forces non-compute paths (also disables features)
    UINT flags;                 // extra D3D11_CREATE_DEVICE_FLAGs

    // Defaults: min 9_1, max 12_1. 10level9 (<= 9_3) is supported on a
    // best-effort basis only; full pl_gpu API requires >= 10_0.
    int min_feature_level;
    int max_feature_level;

    int max_frame_latency;      // device-wide, like swapchain_depth
};

#define PL_D3D11_DEFAULTS  .allow_software = true,
#define pl_d3d11_params(...) (&(struct pl_d3d11_params) { PL_D3D11_DEFAULTS __VA_ARGS__ })
PL_API extern const struct pl_d3d11_params pl_d3d11_default_params;
```

**Implications for the render-API backend.** The libmpv path
exclusively uses the host-supplies-device branch — the host owns GPU
lifetime, that's the entire point of the render API. Phase 2's
`init()` reads `MPV_RENDER_PARAM_D3D11_INIT_PARAMS` for
`ID3D11Device*`, asserts it is non-NULL, and passes it as
`params.device`. libplacebo `AddRef`s on success; `pl_d3d11_destroy`
releases. The adapter / feature-level / flags fields are left zero
(irrelevant when `device` is set).

### 1c. `pl_d3d11_wrap()` — verified, with corrections to the agent's reconstruction

```c
struct pl_d3d11_wrap_params {
    // "The D3D11 texture to wrap, or a texture array containing the
    //  texture to wrap. Must be a ID3D11Texture1D, ID3D11Texture2D or
    //  ID3D11Texture3D created by the same device used by `gpu`, must
    //  have D3D11_USAGE_DEFAULT, and must not be mipmapped or
    //  multisampled."  (d3d11.h:228-231)
    ID3D11Resource *tex;        // ⚠️ ID3D11Resource*, NOT ID3D11Texture2D*
    int             array_slice;

    // Video-resource only: shader-view type + size for planar
    // formats (NV12, P010, AYUV, ...). Unused for non-video, where
    // libplacebo auto-detects the format.
    DXGI_FORMAT fmt;
    int         w;              // ⚠️ also present (agent missed)
    int         h;
};

#define pl_d3d11_wrap_params(...) (&(struct pl_d3d11_wrap_params) { __VA_ARGS__ })

// "`pl_d3d11_wrap` takes a reference to the texture, which is
//  released when `pl_tex_destroy` is called."  (d3d11.h:256-257)
PL_API pl_tex pl_d3d11_wrap(pl_gpu gpu, const struct pl_d3d11_wrap_params *params);
```

**⚠️ Three corrections vs the subagent's reconstruction**, all
material for the Phase 2 implementation:

1. **`tex` is `ID3D11Resource*`, not `ID3D11Texture2D*`.** The wrap
   function accepts 1D / 2D / 3D textures — phase 2's public
   `mpv_d3d11_tex` struct should mirror this if we want to expose the
   full surface (probably overkill — host backbuffers are always 2D —
   but the internal call into `pl_d3d11_wrap` takes the more general
   pointer).
2. **`pl_d3d11_wrap` takes a reference to the texture.** The host is
   *not* responsible for keeping the texture alive while the wrapper
   exists; libplacebo `AddRef`s on wrap and `Release`s on
   `pl_tex_destroy`. This is cleaner than the agent's reconstructed
   "host owns lifetime" model.
3. **`pl_d3d11_wrap_params` has `int w, h`** for video-format wraps.
   Backbuffer wraps don't need them (auto-detected), but the field
   exists.

### 1d. Required `ID3D11Texture2D` state (host's contract)

Per `pl_d3d11_wrap_params::tex` documentation, the backbuffer passed
via `MPV_RENDER_PARAM_D3D11_TEX` must satisfy:

- **`D3D11_USAGE_DEFAULT`** — `IMMUTABLE`/`DYNAMIC`/`STAGING` are
  incompatible (the host's DXGI backbuffer is always `DEFAULT`, so
  trivially satisfied for the common case).
- **Not mipmapped** (`MipLevels = 1`).
- **Not multisampled** (`SampleDesc.Count = 1`). Host MSAA must be
  resolved to a non-MSAA target before being handed to mpv.
- **Same `ID3D11Device`** as the one libplacebo wraps via
  `pl_d3d11_create`. This is what makes [§5.3](#53-hwdec-interop-on-d3d11)
  a real constraint for hwdec.
- **`BindFlags`** — not directly constrained by `pl_d3d11_wrap`, but
  libplacebo's renderer writes the target via RTV (and UAV when
  compute is engaged), so the host must include
  `D3D11_BIND_RENDER_TARGET` at minimum. For compute paths
  (peak-detect, certain scalers, FSR) `D3D11_BIND_UNORDERED_ACCESS`
  is also required. The safe host contract is `RT | UAV`, set on the
  DXGI swapchain via `DXGI_USAGE_RENDER_TARGET_OUTPUT |
  DXGI_USAGE_UNORDERED_ACCESS` (exactly the flags mpv's own windowed
  D3D11 path uses, see [d3d11_helpers.c](video/out/gpu/d3d11_helpers.c)).
  Document in `render_d3d11.h`.

### 1e. Feature level

`pl_d3d11_params::min_feature_level` defaults to `D3D_FEATURE_LEVEL_9_1`
([d3d11.h:119](/home/maxde/libplacebo/src/include/libplacebo/d3d11.h#L119)),
but the header's own comment ([d3d11.h:92-118](/home/maxde/libplacebo/src/include/libplacebo/d3d11.h#L92))
catalogues the 10level9 restrictions and concludes "If these
restrictions are undesirable and you don't need to support ancient
hardware, set `min_feature_level` to `D3D_FEATURE_LEVEL_10_0`."

For the render-API, the host owns the device — we *constrain* the
host's device, not libplacebo's auto-creation. Recommendation: the
public mpv contract documents `D3D_FEATURE_LEVEL_11_0` as the
minimum required device feature level (compute support → all
gpu_next paths usable). `init()` verifies with
`ID3D11Device::GetFeatureLevel` and returns `MPV_ERROR_UNSUPPORTED`
on lower. This is conservative vs libplacebo's own minimum but
matches mpv's actual rendering needs.

### 1f. Teardown order

Per [d3d11.h:144-148](/home/maxde/libplacebo/src/include/libplacebo/d3d11.h#L144):

> "Release the D3D11 device. Note that all libplacebo objects
> allocated from this pl_d3d11 object (e.g. via d3d11->gpu or using
> pl_d3d11_create_swapchain) *must* be explicitly destroyed by the
> user before calling this."

Order in `destroy()`:

```c
pl_tex_destroy(ctx->gpu, &p->wrapped_fbo);  // wrap first
pl_d3d11_destroy(&p->pl_d3d11);              // releases pl_gpu (and Releases the host device ref)
pl_log_destroy(&ctx->pllog);
```

The render backend's `destroy` already calls
`gpu_next_core_destroy(core)` *before* the context-fns destroy
([libmpv_gpu_next.c](video/out/gpu_next/libmpv_gpu_next.c)), so the
core has already torn down all its libplacebo objects living on
`ctx->gpu`. This matches the GL backend's working pattern.

### 1g. Device-removed handling

`ID3D11Device::GetDeviceRemovedReason()` returns `DXGI_ERROR_DEVICE_REMOVED`
after a TDR or driver upgrade. The recovery model is "destroy
everything and recreate" — by D3D11 convention. On the libmpv render
API this means the *host* catches the failure (it owns the device),
destroys the `mpv_render_context`, recreates with a new device, and
re-injects it. mpv must propagate the device-removed failure cleanly
(an `MPV_ERROR_GENERIC` from `render` is acceptable; do NOT crash
inside libplacebo). Phase 2/4 gates only exercise the happy path;
device-removed is a Phase 5+ host-shell concern, not blocking.

### 1h. Build gates

WSL libplacebo is built without D3D11 (`pl_has_d3d11=0` per
`pkg-config`). The header is still installed; the actual D3D11
backend symbols only exist when libplacebo is built with D3D11
support, which is the case on the Windows MSYS2 rig that runs
`run-hdrval.ps1`. The new mpv source file must dual-gate:

```meson
# meson.build  (new — mirrors W3's pl_has_opengl pattern, ref e7f2ffa)
if features['d3d11'] and libplacebo.get_variable('pl_has_d3d11', default_value: '0') == '1'
    sources += files('video/out/d3d11/libmpv_pl_d3d11.c')
endif
```

```c
// video/out/d3d11/libmpv_pl_d3d11.c
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
... full file ...
#endif
```

The `context_backends[]` entry in
[libmpv_gpu_next.c](video/out/gpu_next/libmpv_gpu_next.c) is also
gated:

```c
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    &libmpv_pl_context_d3d11,
#endif
```

On WSL the array stays GL-only; on Windows-MSYS2 both backends
register. No `MPV_ERROR_NOT_IMPLEMENTED` at the runtime layer — the
backend is just absent when not compiled in, and string-match in
`init` returns `MPV_ERROR_NOT_IMPLEMENTED` exactly as the GL backend
does on a libplacebo without GL.

### 1i. Version pin against 7.360.1

Every signature quoted above is in the in-tree libplacebo 7.360.1
header — no `PL_API_VER >= X` guards needed. If any
hypothetical future Phase 6+ Vulkan/Metal work hits a since-7.4xx+
field, defensive `PL_API_VER` guarding can follow the existing
[core.c:2381](video/out/gpu_next/core.c#L2381) pattern. Not relevant
here.

---

## §2. mpv render-API ABI hygiene (verified)

### 2a. Adding the new constants is ABI-safe

`mpv_render_param_type` is an enum densely numbered 0..20
([render.h:172-426](include/mpv/render.h#L172)). Per the header's
own comment, only the `0` terminator's value is *guaranteed*:

> "Not a valid value, but also used to terminate a params array.
> Its value is always guaranteed to be 0 (even if the ABI changes in
> the future)."  ([render.h:174-176](include/mpv/render.h#L174))

Adding enumerators at the end (`21 = TARGET_COLORSPACE`,
`22 = D3D11_INIT_PARAMS`, `23 = D3D11_TEX`) is safe: existing
enumerator values do not move; old code passing
`MPV_RENDER_PARAM_INVALID = 0` as terminator stays correct.

`MPV_RENDER_API_TYPE_*` is a `#define` of a string
([render.h:469-475](include/mpv/render.h#L469)), runtime-matched
against `MPV_RENDER_PARAM_API_TYPE`. Adding a new string
(`"pl-d3d11"`) is unconditionally ABI-safe. Verified W4 (`e7f2ffa`)
added `MPV_RENDER_API_TYPE_PL_OPENGL = "pl-opengl"` this way and
shipped clean.

### 2b. ⚠️ The W4 commit did NOT bump `MPV_CLIENT_API_VERSION`

`git show e7f2ffa -- include/mpv/render.h` confirms W4 added only the
one `#define` line — no `MPV_CLIENT_API_VERSION` bump, no
`client-api-changes.rst` entry. Current version is still
`MPV_MAKE_VERSION(2, 5)` ([client.h:251](include/mpv/client.h#L251))
and the latest `client-api-changes.rst` entry is the 2.5 row
("Deprecate MPV_RENDER_PARAM_AMBIENT_LIGHT",
[client-api-changes.rst:36](DOCS/client-api-changes.rst#L36)).

Per the documented convention:

> "Normally, changes to the C API that are incompatible to previous
> iterations receive a major version bump … while C API additions
> bump the minor version."  ([client-api-changes.rst:9-13](DOCS/client-api-changes.rst))

Strictly read, W4 should have bumped 2.5→2.6 and added a row.
Personal-fork slack made it acceptable; upstream review will not.

**Recommendation for this plan's Phase 1 commit**: bump
`MPV_CLIENT_API_VERSION` to `MPV_MAKE_VERSION(2, 6)` and add **one**
`client-api-changes.rst` row covering both W4's missed bump and this
plan's additions:

```
 2.6    - add MPV_RENDER_API_TYPE_PL_OPENGL and associated render_gl.h
          usage (see render.h, render_gl.h)
        - add MPV_RENDER_API_TYPE_PL_D3D11 and associated
          MPV_RENDER_PARAM_D3D11_INIT_PARAMS / _D3D11_TEX
          (see render.h, render_d3d11.h)
        - add MPV_RENDER_PARAM_TARGET_COLORSPACE for HDR-capable
          target surface negotiation (see render.h)
```

This corrects the W4 procedural gap and lands the new additions in
the same minor bump.

### 2c. The struct is forward-compatible by design

`mpv_render_param.data` is `void*`
([render.h:459-462](include/mpv/render.h#L459)); the host passes a
pointer to its own struct. Future struct extensions append fields
(C ABI rule: never reorder, never reinterpret old fields). The
proposed [§4](#4-mpv_render_param_target_colorspace-design) struct
keeps every field semantic-zero-meaning-"unknown/default", so old
hosts that `memset(0)` and fill only what they know remain forward-
compatible.

### 2d. `_PARAM_DEPTH` + `_TARGET_COLORSPACE` compose cleanly

`MPV_RENDER_PARAM_DEPTH = 5` ([render.h:213-220](include/mpv/render.h#L213))
is already plumbed through to `gpu_next_core_apply_target_options`'
`fallback_depth` parameter
([libmpv_gpu_next.c:239](video/out/gpu_next/libmpv_gpu_next.c#L239),
[core.c:2378-2389](video/out/gpu_next/core.c#L2378)).
`TARGET_COLORSPACE` is orthogonal — primaries / transfer / HDR
metadata, NOT depth — so the two compose without interaction. Phase
3's render hook reads both and writes them into different `target.*`
fields.

---

## §3. Host HDR surface feasibility — direction-of-truth

⚠️ This section was not verifiable in this audit pass (WebFetch /
WebSearch denied to the subagent and not used in the parent
verification — Phase 5b should re-check framework version pins
against current upstream docs). The architectural verdict — "at
least one viable HDR-capable D3D11 path exists per candidate host"
— is firm; the framework-API specifics are direction-of-truth as
of 2026-05.

### 3a. WinUI 3 / `SwapChainPanel` — **STRONG (recommended for new Windows-native shells)**

`SwapChainPanel` is a XAML element that implements
`ISwapChainPanelNative`. The host creates an `IDXGISwapChain1` via
`IDXGIFactory2::CreateSwapChainForComposition` (DComp-backed), then
calls `ISwapChainPanelNative::SetSwapChain(swapchain)` to inject
into the XAML tree.

HDR path:
1. Create swapchain with `DXGI_FORMAT_R10G10B10A2_UNORM` (or
   `R16G16B16A16_FLOAT`).
2. `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)`.
3. Per frame: `GetBuffer(0, IID_PPV_ARGS(&backbuffer))` → pass into
   mpv as `MPV_RENDER_PARAM_D3D11_TEX` → `mpv_render_context_render`
   → `Present(1, 0)`.

Best fit for new Windows-native player UIs. WinUI 3 1.4+ /
Win11 22H2+ for the modern HDR composition path.

### 3b. Qt 6.7+ `QQuickRhiItem` — **MEDIUM-STRONG (cross-platform Qt apps)**

Qt 6.7 (April 2024) shipped `QQuickRhiItem`, the public RHI item
replacement for `QQuickFramebufferObject`. `QRhi::nativeHandles()`
returns `QRhiD3D11NativeHandles*` (struct with `ID3D11Device*` +
`ID3D11DeviceContext*`). `QRhiTexture::nativeTexture()` returns the
backing `ID3D11Texture2D*` as a `quint64`. Host plumbing: bridge
the `pl_d3d11_create(device)` call to Qt's RHI device, hand
`mpv_render_context_render` the texture per frame.

HDR signaling through Qt's own swapchain has been a moving target;
hosts may need to bypass Qt's swapchain and manage their own
(sharing the RHI device via `QRhi::AdoptedNativeHandles`).
Re-confirm on the live Qt 6.7+ docs in Phase 5b.

### 3c. Qt 6.x + custom `QWindow` + manual DXGI — **STRONG (lowest framework risk)**

For any Qt 6, subclass `QWindow`, set
`setSurfaceType(QSurface::Direct3DSurface)`, retrieve HWND via
`winId()`, manage a DXGI swapchain on that HWND directly. Zero
framework HDR-API churn risk; the host owns swapchain + colorspace
negotiation entirely. Falls back here if 3b's RHI bridge proves
unreliable.

### 3d. Avalonia + `NativeControlHost` — **MEDIUM (Windows-only without further work)**

No first-class GPU-texture interop in Avalonia 11.x. Viable path:
`NativeControlHost` embeds an HWND inside Avalonia's visual tree;
host then runs the §3c manual-DXGI path on that HWND. Cross-platform
Avalonia to macOS would need Phase 6 (Metal) — out of this plan's
scope.

### 3e. Bare Win32 + DComp `CreateSwapChainForComposition` — **STRONG (Phase 5a verification host)**

The Phase 5a deliverable. Smallest surface: `CreateWindowExW` →
`D3D11CreateDevice` (FL ≥ 11_0) → `CreateSwapChainForHwnd` with
`R10G10B10A2_UNORM` + `SetColorSpace1` → per-frame `GetBuffer(0)`
→ `mpv_render_context_render` → `Present`. ~200 LOC. The right
choice for the mpv-side proof-of-concept.

### 3f. Electron + `--wid=HWND` — **FALLBACK (not this plan)**

Status quo: mpv owns the HWND swapchain via the windowed
`vo_gpu_next` path. HDR works today, validated through `c8e9298`.
The render-API plan exists to give hosts that *cannot* relinquish
HWND control (XAML shells, RHI items, custom compositors) the same
HDR. `--wid` remains the explicit fallback if Phase 4 fails.

### 3g. Recommendation

- **New Windows-native shell**: §3a (WinUI 3 + SwapChainPanel) —
  best Windows compositor HDR integration, mature docs.
- **Cross-platform Qt 6.7+**: §3b if RHI bridge proves out in Phase
  5b, else §3c.
- **Avalonia**: §3d, Windows-only.
- **Phase 5a verification host**: §3e (bare Win32 + DComp).

**No kill-point**: at least one path per candidate gives mpv an
HDR-capable `ID3D11Texture2D` per frame. The mpv-side plan
(Phases 0-4) is identical regardless.

---

## §4. `MPV_RENDER_PARAM_TARGET_COLORSPACE` design

The mpv-side enums must NOT leak libplacebo types into the public
mpv header (per existing convention — [render_gl.h](include/mpv/render_gl.h)
is libplacebo-free). Numeric values are mpv-internal; libplacebo's
`pl_color_*` enum numbering may change between versions. Phase 3
maintains a static translation in the render hook.

### 4a. Proposed public struct (in `include/mpv/render.h`)

```c
// Color primaries / chromaticities — mpv-side enum, parallel to
// the in-tree gl_video_opts::target_prim and the video-params
// property "primaries" string set.
typedef enum mpv_color_primaries {
    MPV_COLOR_PRIMARIES_AUTO        = 0,  // host doesn't know; renderer uses default
    MPV_COLOR_PRIMARIES_BT_601_525,
    MPV_COLOR_PRIMARIES_BT_601_625,
    MPV_COLOR_PRIMARIES_BT_709,
    MPV_COLOR_PRIMARIES_BT_470M,
    MPV_COLOR_PRIMARIES_EBU_3213,
    MPV_COLOR_PRIMARIES_BT_2020,
    MPV_COLOR_PRIMARIES_APPLE,
    MPV_COLOR_PRIMARIES_ADOBE,
    MPV_COLOR_PRIMARIES_PRO_PHOTO,
    MPV_COLOR_PRIMARIES_CIE_1931,
    MPV_COLOR_PRIMARIES_DCI_P3,
    MPV_COLOR_PRIMARIES_DISPLAY_P3,
    MPV_COLOR_PRIMARIES_V_GAMUT,
    MPV_COLOR_PRIMARIES_S_GAMUT,
    MPV_COLOR_PRIMARIES_FILM_C,
    MPV_COLOR_PRIMARIES_ACES_AP0,
    MPV_COLOR_PRIMARIES_ACES_AP1,
} mpv_color_primaries;

// Transfer / EOTF.
typedef enum mpv_color_transfer {
    MPV_COLOR_TRANSFER_AUTO         = 0,
    MPV_COLOR_TRANSFER_BT_1886,
    MPV_COLOR_TRANSFER_SRGB,
    MPV_COLOR_TRANSFER_LINEAR,
    MPV_COLOR_TRANSFER_GAMMA18,
    MPV_COLOR_TRANSFER_GAMMA20,
    MPV_COLOR_TRANSFER_GAMMA22,
    MPV_COLOR_TRANSFER_GAMMA24,
    MPV_COLOR_TRANSFER_GAMMA26,
    MPV_COLOR_TRANSFER_GAMMA28,
    MPV_COLOR_TRANSFER_PRO_PHOTO,
    MPV_COLOR_TRANSFER_ST428,
    MPV_COLOR_TRANSFER_PQ,          // SMPTE ST 2084 / HDR10
    MPV_COLOR_TRANSFER_HLG,         // ARIB STD-B67
    MPV_COLOR_TRANSFER_V_LOG,
    MPV_COLOR_TRANSFER_S_LOG1,
    MPV_COLOR_TRANSFER_S_LOG2,
} mpv_color_transfer;

// HDR mastering / display metadata. All fields zero = "unknown".
typedef struct mpv_hdr_metadata {
    float max_luma;   // mastering / display max luma (cd/m²)
    float min_luma;   // mastering / display min luma (cd/m²)
    float max_cll;    // MaxCLL (cd/m²)
    float max_fall;   // MaxFALL (cd/m²)
    // Display primary chromaticities — zero falls back to the
    // primaries enum above.
    struct { float x, y; } prim_red, prim_green, prim_blue, white_point;
} mpv_hdr_metadata;

// MPV_RENDER_PARAM_TARGET_COLORSPACE pointee.
// All fields default to zero/AUTO meaning "renderer uses its default"
// — so a host that pins only the transfer can leave primaries=AUTO.
typedef struct mpv_render_param_target_colorspace {
    mpv_color_primaries primaries;
    mpv_color_transfer  transfer;
    mpv_hdr_metadata    hdr;
} mpv_render_param_target_colorspace;
```

### 4b. Why mpv enums, not libplacebo enums

Two maintainer-bar concerns:
1. **API hygiene** — libplacebo is an implementation detail; nothing
   in the public mpv API today leaks it
   ([render.h](include/mpv/render.h) and
   [render_gl.h](include/mpv/render_gl.h) are libplacebo-free).
2. **Value stability** — `pl_color_primaries` and
   `pl_color_transfer` numeric values may shift between libplacebo
   versions. mpv-defined values insulate hosts from libplacebo
   bumps.

The enum *names* match mpv's existing `video-params` property strings
(`"primaries"`, `"gamma"`) and the `--target-prim` / `--target-trc`
choice lists in `video/out/gpu/video.c` — so a host that knows mpv's
property/option strings can map 1:1.

### 4c. Mapping into libplacebo (Phase 3 implementation)

Static translation tables in `render_backend_gpu_next::render`:

```c
static enum pl_color_primaries mp_to_pl_prim(mpv_color_primaries p) {
    switch (p) {
    case MPV_COLOR_PRIMARIES_BT_601_525:  return PL_COLOR_PRIM_BT_601_525;
    case MPV_COLOR_PRIMARIES_BT_601_625:  return PL_COLOR_PRIM_BT_601_625;
    case MPV_COLOR_PRIMARIES_BT_709:      return PL_COLOR_PRIM_BT_709;
    case MPV_COLOR_PRIMARIES_BT_470M:     return PL_COLOR_PRIM_BT_470M;
    case MPV_COLOR_PRIMARIES_EBU_3213:    return PL_COLOR_PRIM_EBU_3213;
    case MPV_COLOR_PRIMARIES_BT_2020:     return PL_COLOR_PRIM_BT_2020;
    case MPV_COLOR_PRIMARIES_APPLE:       return PL_COLOR_PRIM_APPLE;
    case MPV_COLOR_PRIMARIES_ADOBE:       return PL_COLOR_PRIM_ADOBE;
    case MPV_COLOR_PRIMARIES_PRO_PHOTO:   return PL_COLOR_PRIM_PRO_PHOTO;
    case MPV_COLOR_PRIMARIES_CIE_1931:    return PL_COLOR_PRIM_CIE_1931;
    case MPV_COLOR_PRIMARIES_DCI_P3:      return PL_COLOR_PRIM_DCI_P3;
    case MPV_COLOR_PRIMARIES_DISPLAY_P3:  return PL_COLOR_PRIM_DISPLAY_P3;
    case MPV_COLOR_PRIMARIES_V_GAMUT:     return PL_COLOR_PRIM_V_GAMUT;
    case MPV_COLOR_PRIMARIES_S_GAMUT:     return PL_COLOR_PRIM_S_GAMUT;
    case MPV_COLOR_PRIMARIES_FILM_C:      return PL_COLOR_PRIM_FILM_C;
    case MPV_COLOR_PRIMARIES_ACES_AP0:    return PL_COLOR_PRIM_ACES_AP0;
    case MPV_COLOR_PRIMARIES_ACES_AP1:    return PL_COLOR_PRIM_ACES_AP1;
    case MPV_COLOR_PRIMARIES_AUTO:
    default:                              return PL_COLOR_PRIM_UNKNOWN;
    }
}
```

(Symmetric `mp_to_pl_trc()` over the transfer enum.) The mpv enum is
designed as a **complete superset** of libplacebo's, so each
non-AUTO mpv value maps to exactly one libplacebo enumerator — no
lossy mapping. The agent's reconstructed enum list was confirmed
against libplacebo
[colorspace.h:200-221](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h#L200)
and [colorspace.h:236-256](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h#L236):
the values match 1:1 with the table above.

`mpv_hdr_metadata` maps field-for-field into libplacebo's
`pl_hdr_metadata` struct
([colorspace.h:412+](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h#L412)),
which carries `prim`/`min_luma`/`max_luma`/`max_cll`/`max_fall`
under the exact same names. The chromaticity x,y pairs map into
`pl_raw_primaries` ([colorspace.h:354](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h#L354)).

### 4d. `pl_color_repr` is **NOT** in the struct

`pl_color_repr` carries (color system, levels, bits, alpha). These
should derive from the texture / depth / other render params, NOT
TARGET_COLORSPACE:

- `system` is always `PL_COLOR_SYSTEM_RGB` for render-API target
  surfaces (the libmpv target is an RGB texture; YCbCr lives upstream
  on the *source* frame).
- `levels` is implicit from the transfer (HDR10 → full-range PQ).
  Default `PL_COLOR_LEVELS_FULL`; `gpu_next_core_output_levels()`
  ([core.c:2357-2359](video/out/gpu_next/core.c#L2357)) refines it
  from mpv's options.
- `bits.sample_depth` comes from `MPV_RENDER_PARAM_DEPTH` — already
  plumbed.
- `alpha` derives from the wrapped texture's `DXGI_FORMAT` (libplacebo
  populates this in `pl_d3d11_wrap`).

Putting any of these in TARGET_COLORSPACE creates a duplicate
source-of-truth hazard. Keep the struct HDR-focused.

### 4e. Per-frame lifetime, not per-context

TARGET_COLORSPACE belongs in `mpv_render_context_render`'s param
array, not `_create`. Reasons:
1. **HDR mode toggles mid-playback.** Display HDR toggled on Windows
   → host recreates swapchain → new colorspace per frame after.
2. **Multi-monitor moves.** Different displays have different HDR
   characteristics.
3. **Convention.** `MPV_RENDER_PARAM_DEPTH` /
   `MPV_RENDER_PARAM_FLIP_Y` are also per-frame
   ([render.h:213-225](include/mpv/render.h#L213)).

Hosts that don't care pass the same struct every frame.

### 4f. Side-by-side mpv ↔ libplacebo (verified)

| mpv (proposed) | libplacebo (verified in colorspace.h) |
|---|---|
| `MPV_COLOR_PRIMARIES_AUTO` | `PL_COLOR_PRIM_UNKNOWN` |
| `MPV_COLOR_PRIMARIES_BT_709` | `PL_COLOR_PRIM_BT_709` |
| `MPV_COLOR_PRIMARIES_BT_2020` | `PL_COLOR_PRIM_BT_2020` |
| `MPV_COLOR_PRIMARIES_DCI_P3` | `PL_COLOR_PRIM_DCI_P3` |
| `MPV_COLOR_PRIMARIES_DISPLAY_P3` | `PL_COLOR_PRIM_DISPLAY_P3` |
| `MPV_COLOR_TRANSFER_AUTO` | `PL_COLOR_TRC_UNKNOWN` |
| `MPV_COLOR_TRANSFER_SRGB` | `PL_COLOR_TRC_SRGB` |
| `MPV_COLOR_TRANSFER_BT_1886` | `PL_COLOR_TRC_BT_1886` |
| `MPV_COLOR_TRANSFER_PQ` | `PL_COLOR_TRC_PQ` |
| `MPV_COLOR_TRANSFER_HLG` | `PL_COLOR_TRC_HLG` |

(Complete cross-check: all 17 mpv primaries map 1:1 onto
`PL_COLOR_PRIM_*` lines 201-221; all 17 mpv transfers map 1:1 onto
`PL_COLOR_TRC_*` lines 237-256. No mpv enum value is unmappable;
no libplacebo enum value is unrepresentable.)

---

## §5. Top-5 no-regression hazards (verified)

### 5.1 HDR↔SDR toggle + wrap-cache staleness — HIGHEST

**Mechanism.** Host's DXGI swapchain is recreated mid-playback:
display HDR toggled, window dragged across monitors, or
`ResizeBuffers` called. Old `ID3D11Texture2D` pointers go invalid;
new ones arrive on the next `wrap_fbo`. If `wrap_fbo` caches by
pointer comparison, a re-creation that happens to reuse the same
pointer value (rare but possible after Release/realloc) would serve
the stale wrap.

**Mitigation.** The GL backend's `wrap_fbo` unconditionally calls
`pl_tex_destroy` on the cached wrap *every* call before re-wrapping
([libmpv_pl_gl.c:85-86](video/out/opengl/libmpv_pl_gl.c#L85)). The
D3D11 backend must mirror this — the wrap cost is microseconds,
cheaper than a stale-texture crash. Phase 4's HDR validation should
include an explicit HDR↔SDR toggle case (extend `run-hdrval.ps1`'s
Windows-side rig).

### 5.2 Precedence: TARGET_COLORSPACE + user `--target-*` opts

**Mechanism.** `gpu_next_core_apply_target_options` already
implements the right precedence
([core.c:2360-2369](video/out/gpu_next/core.c#L2360)):

```c
if (opts->target_prim && (!target->color.primaries || !hint))
    target->color.primaries = opts->target_prim;
if (opts->target_trc && (!target->color.transfer || !hint))
    target->color.transfer = opts->target_trc;
if (opts->target_peak && (!target->color.hdr.max_luma || !hint))
    target->color.hdr.max_luma = opts->target_peak;
```

When `hint=true`, user opts override only fields the host did NOT
pin. When `hint=false`, user opts always override.

**The risk.** The W6 stub currently passes `hint=false`
([libmpv_gpu_next.c:296](video/out/gpu_next/libmpv_gpu_next.c#L296)) —
correct for the W5-6 PL_OPENGL path (no host colorspace pin) but
**wrong** for PL_D3D11 with TARGET_COLORSPACE. Phase 3 flips the
hint:

```c
hint = (target_colorspace != NULL &&
        target_colorspace->transfer != MPV_COLOR_TRANSFER_AUTO);
```

**Mitigation.** Phase 3's gate matrix includes three explicit
checks: (1) PL_OPENGL without TARGET_COLORSPACE = byte-identical
to W6 baseline; (2) PL_D3D11 with TARGET_COLORSPACE=sRGB = matches
PL_OPENGL on same hardware; (3) PL_D3D11 with TARGET_COLORSPACE=PQ +
`--target-trc=hlg` user override = HLG output (visible: option does
override).

### 5.3 Hwdec interop on D3D11 (same-device requirement)

**Mechanism.** D3D11 textures are device-scoped. When the source is
`d3d11va`-decoded, the source `ID3D11Texture2D` lives on the
*decoder's* device; libplacebo's hwdec path wraps it on the
*renderer's* `pl_gpu`. **If host device ≠ decoder device, wrap
fails** — there's no cross-device zero-copy without explicit shared-
handle plumbing.

The windowed path doesn't hit this: `vo_gpu_next` creates the D3D11
device for both decoder and renderer. On the render-API, the *host*
owns the device — and mpv's hwdec init currently creates its own.

**Resolution (two stages):**

- **Phase 2 (SDR self-baseline):** explicitly disable d3d11va hwdec
  when the render-API is `pl-d3d11` — fall back to SW decode.
  Documented limitation; unblocks the Phase 2 gate.
- **Phase 4 (HDR validation):** teach the hwdec init path to consume
  the render-API device. There is exactly one bottleneck —
  `D3D11CreateDevice` in
  [d3d11_helpers.c:497](video/out/gpu/d3d11_helpers.c#L497) — to
  redirect. The user's HDR rig validation requires this: the 4K
  HDR10 clip used by `run-hdrval.ps1` is d3d11va-decoded, and
  losing zero-copy would change the rendered output via CPU
  upload paths.

### 5.4 Resize + DPI scaling

**Mechanism.** Host's swapchain resizes via `ResizeBuffers`; new
backbuffers arrive. mpv's `render_backend_fns::resize` is called
with new src/dst rects, then the next `render` call brings a new
`MPV_RENDER_PARAM_D3D11_TEX`.

**Mitigation.** Same as §5.1 — `wrap_fbo` destroys-and-rewraps
unconditionally. Phase 2's render-API harness should grow a resize
test (extend `_golden/render_api/rapi_harness.c` to capture
1280×720, resize to 1920×1080, re-capture, both pass against the
self-baseline).

### 5.5 Threading model

**Mechanism.** D3D11's immediate context is not thread-safe by
default. libplacebo's D3D11 backend uses it internally. mpv's
render API explicitly relaxes mpv's thread rule:

> "The mpv_render_* functions can be called from any thread, under
> the following conditions: only one of the mpv_render_* functions
> can be called at the same time"
> ([render.h:62-67](include/mpv/render.h#L62))

But this guarantees only *mpv's* serialization, not synchronization
between mpv's render call and the host's *other* D3D11 work on the
same device.

**Mitigation.** Document in `render_d3d11.h`: the host must call
`mpv_render_context_render` on the same thread as its
`ID3D11DeviceContext` work, OR serialize via a host-owned mutex
around all `ID3D11DeviceContext` calls including mpv's. The Phase
5a verification host single-threads the render loop; 5b production
hosts have framework-specific guidance (Qt RHI serializes on its
render thread; SwapChainPanel rendering serializes; Avalonia
`NativeControlHost` needs care).

### Lower-tier hazards (documented; not blocking)

- **OSD/subtitles in HDR**: `gpu_next_core_update_overlays` already
  passes the OSD through libplacebo's `pl_overlay` with HDR-correct
  tone-mapping. Phase 4 should extend the `sdr_subs` golden case to
  an HDR variant.
- **`MPV_RENDER_PARAM_FLIP_Y`**: D3D11's default Y-down matches
  mpv's; most hosts pass `flip_y=0`. The W6 hook already reads
  this param ([libmpv_gpu_next.c:240](video/out/gpu_next/libmpv_gpu_next.c#L240)).

---

## §6. Go / No-Go verdict

### GO — confidence MEDIUM

**Why GO.** The decisive architecture question — "can
`render_backend_gpu_next` add a second context-fns backend and one
new render param without disturbing the W5-6 architecture or the
proven HDR fidelity of the windowed path?" — resolves to **yes**,
because:

1. **The architecture proves it.** `render_backend_gpu_next` +
   `libmpv_pl_context_fns` were structured precisely to admit
   additional context-fns backends with zero changes to the shared
   core. The diff for D3D11: one new file (`libmpv_pl_d3d11.c`,
   ~150 LOC parallel to [libmpv_pl_gl.c](video/out/opengl/libmpv_pl_gl.c)),
   one new entry in `context_backends[]`, two `meson.build` lines,
   two new `mpv/render*.h` additions.
2. **The HDR machinery is already in the core.**
   `gpu_next_core_apply_target_options`'s `hint=true` path
   ([core.c:2360-2389](video/out/gpu_next/core.c#L2360)) exists
   precisely for this case — host-supplied baseline +
   user-opt refinement. `gpu_next_core_finalize_target_csp`'s
   `target_csp`/`target_unknown` parameters
   ([core.c:2395-2451](video/out/gpu_next/core.c#L2395)) already
   accept a host-supplied target colorspace. The windowed path
   ([vo_gpu_next.c:228-231](video/out/vo_gpu_next.c#L228)) uses both;
   the render hook will too.
3. **The bit-faithfulness gate is well-understood.** The W5-6 work
   shipped `run-hdrval.ps1` SW+HW with the reference PNG hash
   `46cdafff…` produced on real NVIDIA D3D11 HDR. Phase 4 must
   match it; the target pixels are *known*, on the user's hardware.

**Why MEDIUM, not HIGH.** Three calibrated concerns:

- **[§5.3](#53-hwdec-interop-on-d3d11) (hwdec same-device).** Real
  engineering — Phase 4's HDR validation needs zero-copy hwdec to
  reach the reference PNG. The fix is mechanical (one
  `D3D11CreateDevice` redirect) but non-trivial.
- **[§5.1](#51-hdrsdr-toggle--wrap-cache-staleness) +
  [§5.2](#52-precedence-target_colorspace--user---target--opts) (silent-failure HDR).**
  TARGET_COLORSPACE precedence ridges on one boolean (`hint`)
  computed correctly in Phase 3. Mis-setting it produces silently
  wrong HDR — exactly the failure class TARGET_COLORSPACE exists
  to fix.
- **[§3](#3-host-hdr-surface-feasibility--direction-of-truth)
  framework specifics** weren't deep-verifiable in this audit
  (sandboxed WebFetch). Architectural verdict is firm; framework
  version pins should be re-confirmed in Phase 5b. **Not blocking**
  — Phase 2 doesn't need any host beyond the existing render-API
  harness.

**Why not LOW / not NO-GO.** No kill-points. The W5-6 architecture
proves the second-context-fns model in principle. The host audit
(§3) found ≥1 strong path per candidate. The HDR core math is
already swapchain-free
([core.c:2475-2545](video/out/gpu_next/core.c#L2475)). The worst
case is Phase 4 shows pixels diverge, fallback (`--wid=HWND`) kicks
in — which is the status quo, already shipping, already validated.

### Single biggest unknown (Phase 4 gate)

**Whether `pl_d3d11_wrap` on a host-supplied `ID3D11Texture2D` from
a DXGI HDR10 swapchain (`R10G10B10A2_UNORM` +
`RGB_FULL_G2084_NONE_P2020`) produces pixel-identical output to
the windowed `vo_gpu_next` path on the same GPU + same display +
same target options — given that (a) the host owns the device (vs
the windowed path where mpv does), and (b) hwdec interop must be
redirected to that host device for the 4K HDR10 clip to retain
zero-copy fidelity.**

This is the Phase 4 gate. Cannot be settled by static reading or
SDR validation; requires `run-hdrval-rapi.ps1` on the same hardware
as the W5-6 sign-off. Acceptance: bit-identical PNG hash to
`46cdafff…` (the W6b reference). If divergent, fallback.

### Fallback cost vs proceed cost

- **Fallback cost: zero.** `--wid=HWND` ships today, works on the
  user's rig today, bit-validated through `c8e9298`. Cost: host
  loses fine-grained compositor control over the video region
  (Electron + DComp hybrid still works via `--wid`).
- **Proceed cost: ~3-5 weeks** focused work for Phases 1-4
  (per-commit golden-gated, conservative pace matching W5-6
  discipline). Architectural risk low; execution risk medium (the
  three §6 concerns); reward = render-API path usable by any modern
  host shell with HDR.

**Recommendation: GO**, with the time-box hard stop at the
Reassessment Gate after Phase 2 as the plan demands. Phase 2's SDR
self-baseline gate is cheap (~1 week) and proves the wrap mechanism
without spending the HDR-validation budget; Phase 4's HDR fidelity
gate is the real go/no-go.

---

## §7. Phase 1 cat-confirm checklist

The subagent's §1 was sandboxed; the parent verified the libplacebo
signatures directly. **No further cat-confirm step is needed before
Phase 1 begins** — the signatures in [§1](#1-libplacebo-d3d11-backend-audit-verified)
are quoted from the in-tree headers and are correct as of this
report. Phase 1's first action is the public-API additions, not
re-confirmation. If a future libplacebo bump introduces signature
churn, defensive `PL_API_VER` guarding follows the existing
[core.c:2381](video/out/gpu_next/core.c#L2381) pattern.

---

## §8. Files audited

**Plan / status:**
- [plan-hdr-render-api.md](plan-hdr-render-api.md), [HANDOFF.md](HANDOFF.md),
  [CLAUDE.md](CLAUDE.md), [phase0-feasibility.md](phase0-feasibility.md)
  (W5-6 phase-0 doc, style template).

**libplacebo 7.360.1 headers (verified):**
- [/home/maxde/libplacebo/src/include/libplacebo/d3d11.h](/home/maxde/libplacebo/src/include/libplacebo/d3d11.h) (full).
- [/home/maxde/libplacebo/src/include/libplacebo/colorspace.h](/home/maxde/libplacebo/src/include/libplacebo/colorspace.h) (lines 1-450).

**mpv public render-API surface:**
- [include/mpv/render.h](include/mpv/render.h) (full, lines 1-766).
- [include/mpv/render_gl.h](include/mpv/render_gl.h) (structural template).
- [include/mpv/client.h](include/mpv/client.h) (version macros).
- [DOCS/client-api-changes.rst](DOCS/client-api-changes.rst) (bump conventions).
- `git show e7f2ffa -- include/mpv/render.h` (W4 commit's exact diff).

**W5-6 internals (structural template):**
- [video/out/gpu_next/libmpv_gpu_next.c](video/out/gpu_next/libmpv_gpu_next.c),
  [.h](video/out/gpu_next/libmpv_gpu_next.h).
- [video/out/opengl/libmpv_pl_gl.c](video/out/opengl/libmpv_pl_gl.c).
- [video/out/libmpv.h](video/out/libmpv.h).
- [video/out/gpu_next/core.h](video/out/gpu_next/core.h),
  [core.c](video/out/gpu_next/core.c) (apply_target_options,
  finalize_target_csp, target_hint).
- [video/out/vo_gpu_next.c](video/out/vo_gpu_next.c) (windowed
  draw_frame + set_colorspace_hint).

**mpv D3D11 conventions:**
- [video/out/d3d11/ra_d3d11.h](video/out/d3d11/ra_d3d11.h).
- [video/out/gpu/d3d11_helpers.c](video/out/gpu/d3d11_helpers.c)
  (device creation, feature-level handling).

**Build system:**
- [meson.build](meson.build) (D3D11 feature gate + libplacebo
  variable-query pattern).

**NOT verified (out of audit perimeter):**
- Host framework live docs (WinUI 3 / Qt 6.7+ RHI / Avalonia) —
  §3 is direction-of-truth as of 2026-05, Phase 5b should
  re-confirm framework version pins against current upstream
  documentation.
