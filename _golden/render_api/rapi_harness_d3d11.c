// Headless libmpv render-API golden harness for the gpu_next render backend,
// D3D11 surface backend (MPV_RENDER_API_TYPE_PL_D3D11).
//
//   rapi_harness_d3d11 --probe                                [create+free a ctx]
//   rapi_harness_d3d11 <clip> <pts> <w> <h> <out.raw> [csp]   [render a frame]
//   rapi_harness_d3d11 --refcount <clip> <pts> ...            [+ P5b.1 ref probe]
//
// Optional [csp] selects the MPV_RENDER_PARAM_TARGET_COLORSPACE passed per
// frame (Phase 3): "none" (default, omit the param entirely = the Phase-2
// behaviour), "srgb" (an all-AUTO struct, documented as "same as omitted" =>
// must be byte-identical to "none"), or "pq2020" (BT.2020 / PQ HDR10 with
// 1000-nit mastering metadata => a different, deterministic render).
//
// The D3D11 analogue of rapi_harness.c (which drives the pl-opengl path on a
// surfaceless EGL context). Here the "host" is a WARP software D3D11 device
// (D3D_DRIVER_TYPE_WARP) -- deterministic run-to-run on one machine, so this
// harness is SELF-baselined exactly like the GL one: capture once on a
// known-good build, then re-verify byte-identical after each commit (see
// rapi_d3d11_run.sh). WARP is a full Feature-Level-11_1 device (compute /
// UAV), so libplacebo's gpu-next paths run as on real hardware, just slower.
//
// This is the Phase-2 gate that proves the P2.1 libmpv_pl_context_d3d11
// implementation (pl_d3d11_create / pl_d3d11_wrap) actually renders, not just
// compiles. It renders into an SDR DXGI_FORMAT_R8G8B8A8_UNORM render target
// (no MPV_RENDER_PARAM_TARGET_COLORSPACE -> sRGB default), reads the pixels
// back through a STAGING copy, and writes raw RGBA8 + a tiny sidecar.
//
// --probe mode: create + free a pl-d3d11 render context and report the result
// (validates the context_backends[] registration + init() / destroy()).

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_d3d11.h>

#define API_TYPE MPV_RENDER_API_TYPE_PL_D3D11

static void die(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

static void die_hr(const char *msg, HRESULT hr)
{
    fprintf(stderr, "FATAL: %s (hr=0x%08lx)\n", msg, (unsigned long)hr);
    exit(1);
}

// --- D3D11 device ----------------------------------------------------------

static ID3D11Device        *d3d_dev;
static ID3D11DeviceContext *d3d_ctx;
static bool                 g_hw_device;   // --hw: real GPU (NVIDIA) vs WARP
static bool                 g_refcount;    // --refcount: P5b.1 wrap-lifetime probe
static bool                 g_hwdec;       // --hwdec: P7.1 d3d11va functional probe

static void d3d_init(void)
{
    static const D3D_FEATURE_LEVEL want[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL got = 0;
    // WARP = Microsoft's software rasterizer (deterministic, self-baseline) for
    // the SDR/HDR-render-path gates; --hw uses the real GPU (D3D_DRIVER_TYPE_
    // HARDWARE = the NVIDIA adapter) for Phase-4b on-rig HDR fidelity vs the
    // windowed path. The render-API model has no swapchain here -- the host
    // reads the target back / lets mpv's screenshot path capture it.
    D3D_DRIVER_TYPE dt = g_hw_device ? D3D_DRIVER_TYPE_HARDWARE
                                     : D3D_DRIVER_TYPE_WARP;
    HRESULT hr = D3D11CreateDevice(NULL, dt, NULL, 0,
                                   want, (UINT)(sizeof(want) / sizeof(want[0])),
                                   D3D11_SDK_VERSION, &d3d_dev, &got, &d3d_ctx);
    if (FAILED(hr))
        die_hr("D3D11CreateDevice failed", hr);
    if (got < D3D_FEATURE_LEVEL_11_0)
        die("D3D11 device feature level below 11_0");
}

static void d3d_uninit(void)
{
    if (d3d_ctx)
        ID3D11DeviceContext_Release(d3d_ctx);
    if (d3d_dev)
        ID3D11Device_Release(d3d_dev);
    d3d_ctx = NULL;
    d3d_dev = NULL;
}

// Bytes per pixel for the target formats this harness emits.
static int fmt_bpp(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return 4;   // SDR
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return 4;   // HDR10 (PQ), 10-bit packed
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 8;   // scRGB, half-float
    default: die("unsupported target format"); return 0;
    }
}

// Host render target with the bind flags libplacebo's gpu-next paths need
// (RENDER_TARGET + UNORDERED_ACCESS, the non-swapchain equivalent of
// DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS documented in
// render_d3d11.h). R10G10B10A2_UNORM has no guaranteed typed-UAV support, so
// fall back to RENDER_TARGET only if the UAV variant is rejected -- libplacebo
// then writes the final pass through a fragment shader instead of a compute
// store, which is exactly what it does against a real non-UAV HDR backbuffer.
static ID3D11Texture2D *make_rendertarget(int w, int h, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC td = {
        .Width = (UINT)w,
        .Height = (UINT)h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = fmt,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS,
        .CPUAccessFlags = 0,
        .MiscFlags = 0,
    };
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(d3d_dev, &td, NULL, &tex);
    if (FAILED(hr)) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = ID3D11Device_CreateTexture2D(d3d_dev, &td, NULL, &tex);
    }
    if (FAILED(hr))
        die_hr("CreateTexture2D(render target) failed", hr);
    return tex;
}

// CPU-readable mirror for glReadPixels-equivalent readback.
static ID3D11Texture2D *make_staging(int w, int h, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC td = {
        .Width = (UINT)w,
        .Height = (UINT)h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = fmt,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Usage = D3D11_USAGE_STAGING,
        .BindFlags = 0,
        .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
        .MiscFlags = 0,
    };
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(d3d_dev, &td, NULL, &tex);
    if (FAILED(hr))
        die_hr("CreateTexture2D(staging) failed", hr);
    return tex;
}

// --- P5b.1 wrap-lifetime probe ----------------------------------------------
//
// After mpv_render_context_render() returns, the engine must hold NO COM
// reference on the host texture (render_d3d11.h reference-lifetime contract):
// the public refcount must show the host as sole owner. A wrap retained across
// render calls (the pre-P5b.1 behaviour, hdr-phase5b-assessment.md §2) blocks
// IDXGISwapChain::ResizeBuffers for hosts that wrap the backbuffer directly,
// since DXGI requires zero outstanding app references on all backbuffers.
// (In-flight GPU use is tracked by the driver outside the public refcount, so
// this probe sees retained wraps/views, not pending commands.)

static ULONG tex_public_refs(ID3D11Texture2D *tex)
{
    ID3D11Texture2D_AddRef(tex);
    return ID3D11Texture2D_Release(tex);   // Release returns the new count
}

// --- common ----------------------------------------------------------------

static mpv_handle *mpv_start(void)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("mpv_create failed");
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "config", "no");
    return mpv;
}

static void drain_log(mpv_handle *mpv)
{
    while (1) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            fprintf(stderr, "[mpv:%s] %s: %s", m->level, m->prefix, m->text);
        }
    }
}

// --- probe -----------------------------------------------------------------

static int run_probe(void)
{
    d3d_init();

    mpv_handle *mpv = mpv_start();
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize failed");
    mpv_request_log_messages(mpv, "error");

    mpv_d3d11_init_params d3d11_init = { .device = (void *)d3d_dev };
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)API_TYPE},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &d3d11_init},
        {0},
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, params);
    drain_log(mpv);

    printf("mpv_render_context_create(\"%s\") -> %d (%s)\n",
           API_TYPE, err, mpv_error_string(err));

    int rc;
    if (err >= 0) {
        printf("PROBE OK: pl-d3d11 render context created\n");
        mpv_render_context_free(rctx);
        rc = 0;
    } else {
        printf("PROBE FAIL: backend unavailable\n");
        rc = 2;
    }

    mpv_destroy(mpv);
    d3d_uninit();
    return rc;
}

// --- target colorspace (Phase 3) -------------------------------------------

enum csp_mode { CSP_NONE, CSP_SRGB, CSP_PQ2020, CSP_PQDISP };

static enum csp_mode parse_csp(const char *tag)
{
    if (!tag || strcmp(tag, "none") == 0)
        return CSP_NONE;
    if (strcmp(tag, "srgb") == 0)
        return CSP_SRGB;
    if (strcmp(tag, "pq2020") == 0)
        return CSP_PQ2020;
    if (strcmp(tag, "pqdisp") == 0)
        return CSP_PQDISP;
    die("unknown csp tag (want: none | srgb | pq2020 | pqdisp)");
    return CSP_NONE;
}

// Returns true and fills *tc if the mode should pass a TARGET_COLORSPACE param.
static bool fill_target_colorspace(enum csp_mode mode,
                                   mpv_render_param_target_colorspace *tc)
{
    *tc = (mpv_render_param_target_colorspace){0};
    switch (mode) {
    case CSP_NONE:
        return false;                       // omit the param entirely
    case CSP_SRGB:
        return true;                        // all-AUTO struct == "same as omitted"
    case CSP_PQ2020:
        tc->primaries = MPV_COLOR_PRIMARIES_BT_2020;
        tc->transfer  = MPV_COLOR_TRANSFER_PQ;
        tc->hdr.max_luma = 1000.0f;
        tc->hdr.max_cll  = 1000.0f;
        tc->hdr.max_fall = 400.0f;
        return true;
    case CSP_PQDISP:
        // Phase 4b: mirror the exact target metadata the windowed swapchain
        // negotiated against this NVIDIA HDR display (run-hdrval.ps1 ref JSON:
        // max-luma 1000, max-cll 1000, max-fall 400, min-luma 0.005), so the
        // render-API render matches the windowed-path render.
        tc->primaries = MPV_COLOR_PRIMARIES_BT_2020;
        tc->transfer  = MPV_COLOR_TRANSFER_PQ;
        tc->hdr.max_luma = 1000.0f;
        tc->hdr.min_luma = 0.005f;
        tc->hdr.max_cll  = 1000.0f;
        tc->hdr.max_fall = 400.0f;
        return true;
    }
    return false;
}

// --- render a held frame into a D3D11 texture and read it back -------------

static int run_render(const char *clip, const char *pts, int w, int h,
                      const char *out, enum csp_mode csp, DXGI_FORMAT fmt)
{
    d3d_init();

    mpv_handle *mpv = mpv_start();
    // Deterministic capture: no audio, paused single frame, fixed scalers and
    // dither -- mirrors rapi_harness.c / _golden/capture.sh's SDR profile.
    mpv_set_option_string(mpv, "audio", "no");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "pause", "yes");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "osd-level", "0");
    mpv_set_option_string(mpv, "interpolation", "no");
    mpv_set_option_string(mpv, "scale", "bilinear");
    mpv_set_option_string(mpv, "cscale", "bilinear");
    mpv_set_option_string(mpv, "dscale", "bilinear");
    mpv_set_option_string(mpv, "dither-depth", "8");
    mpv_set_option_string(mpv, "hr-seek", "yes");
    mpv_set_option_string(mpv, "start", pts);
    // P7.1: --hwdec drives d3d11va hardware decode through the render API. The
    // render device and the d3d11va decode device are the same ID3D11Device by
    // construction (libmpv_pl_d3d11.c builds its ra_d3d11 on the host device),
    // so the decoded surface wraps zero-copy into the render gpu. Without the
    // flag, keep hwdec off for the deterministic SW self-baseline (WARP has no
    // hardware video decoder, so d3d11va is meaningless there anyway).
    mpv_set_option_string(mpv, "hwdec", g_hwdec ? "d3d11va" : "no");
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize failed");
    mpv_request_log_messages(mpv, "error");

    mpv_d3d11_init_params d3d11_init = { .device = (void *)d3d_dev };
    mpv_render_param cparams[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)API_TYPE},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &d3d11_init},
        {0},
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, cparams);
    if (err < 0) {
        drain_log(mpv);
        fprintf(stderr, "FATAL: mpv_render_context_create -> %d (%s)\n",
                err, mpv_error_string(err));
        return 1;
    }

    const char *cmd[] = {"loadfile", clip, NULL};
    if (mpv_command(mpv, cmd) < 0)
        die("loadfile failed");

    ID3D11Texture2D *rt = make_rendertarget(w, h, fmt);
    ID3D11Texture2D *staging = make_staging(w, h, fmt);
    int bpp = fmt_bpp(fmt);

    // Pump events until the seeked-to frame is decoded and presented, then
    // render it. The frame is held (paused), so the render is deterministic.
    bool restarted = false, rendered = false;
    int rerr = 0;
    for (int i = 0; i < 2000 && !rendered; i++) {
        mpv_event *ev = mpv_wait_event(mpv, 0.05);
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            die("mpv shut down before a frame was rendered");
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            fprintf(stderr, "[mpv:%s] %s: %s", m->level, m->prefix, m->text);
        }
        if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART)
            restarted = true;

        uint64_t flags = mpv_render_context_update(rctx);
        if (restarted && (flags & MPV_RENDER_UPDATE_FRAME)) {
            mpv_d3d11_tex d3dtex = { .tex = (void *)rt, .w = w, .h = h };
            int block = 1;
            mpv_render_param_target_colorspace tc;
            bool have_tc = fill_target_colorspace(csp, &tc);
            // No MPV_RENDER_PARAM_FLIP_Y: D3D11 textures are top-left origin,
            // matching mpv's image orientation (the GL path flips only to
            // undo GL's bottom-left FBO convention).
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_D3D11_TEX, &d3dtex},
                {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                // Trailing slot for the optional target colorspace, then {0}.
                {have_tc ? MPV_RENDER_PARAM_TARGET_COLORSPACE : 0,
                 have_tc ? (void *)&tc : NULL},
                {0},
            };
            rerr = mpv_render_context_render(rctx, rp);
            rendered = true;
        }
    }
    if (!rendered)
        die("timed out waiting for a frame");
    if (rerr < 0) {
        drain_log(mpv);
        fprintf(stderr, "FATAL: mpv_render_context_render -> %d (%s)\n",
                rerr, mpv_error_string(rerr));
        return 1;
    }

    // P5b.1 probe point 1: render returned, context still alive. The host
    // created the texture (1 ref) and handed it to exactly one render call.
    ULONG refs_post_render = g_refcount ? tex_public_refs(rt) : 0;

    // Read the rendered pixels back: copy the render target into the staging
    // texture (the render hook already pl_gpu_flush()'d libplacebo's commands
    // onto the immediate context), then Map(READ) forces completion.
    ID3D11DeviceContext_CopyResource(d3d_ctx, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr = ID3D11DeviceContext_Map(d3d_ctx, (ID3D11Resource *)staging, 0,
                                         D3D11_MAP_READ, 0, &map);
    if (FAILED(hr))
        die_hr("Map(staging, READ) failed", hr);

    size_t row = (size_t)w * bpp;
    size_t npix = row * (size_t)h;
    uint8_t *pix = malloc(npix);
    if (!pix)
        die("out of memory");
    // Pack tightly, dropping the driver's row padding so the raw dump is
    // identical regardless of staging RowPitch alignment.
    const uint8_t *src = map.pData;
    for (int y = 0; y < h; y++)
        memcpy(pix + (size_t)y * row, src + (size_t)y * map.RowPitch, row);
    ID3D11DeviceContext_Unmap(d3d_ctx, (ID3D11Resource *)staging, 0);

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(pix, 1, npix, f) != npix)
        die("writing raw output failed");
    fclose(f);

    // Tiny sidecar: structural signal beyond the raw pixels.
    char *pixfmt = mpv_get_property_string(mpv, "video-params/pixelformat");
    char sidecar[512];
    snprintf(sidecar, sizeof(sidecar), "%s.txt", out);
    static const char *const csp_name[] = {
        [CSP_NONE] = "none", [CSP_SRGB] = "srgb",
        [CSP_PQ2020] = "pq2020", [CSP_PQDISP] = "pqdisp",
    };
    const char *fmt_name = fmt == DXGI_FORMAT_R10G10B10A2_UNORM ? "rgb10a2"
                         : fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? "rgba16f"
                         : "rgba8";
    f = fopen(sidecar, "w");
    if (f) {
        fprintf(f, "width=%d\nheight=%d\npixelformat=%s\nsurface=d3d11-%s\n"
                   "target_colorspace=%s\n",
                w, h, pixfmt ? pixfmt : "?", fmt_name, csp_name[csp]);
        fclose(f);
    }
    mpv_free(pixfmt);

    printf("rendered %s @ %s -> %s (%dx%d, %zu bytes)\n",
           clip, pts, out, w, h, npix);

    int rc = 0;

    // P7.1 functional probe: confirm d3d11va actually engaged (this is the
    // proof WARP structurally cannot give -- it has no hardware decoder) and
    // that the decoded frame produced real, non-uniform content (not a purple
    // error clear / black). Must run on the real GPU (--hwdec implies --hw).
    if (g_hwdec) {
        char *cur = mpv_get_property_string(mpv, "hwdec-current");
        char *drops = mpv_get_property_string(mpv, "decoder-frame-drop-count");
        bool engaged = cur && strstr(cur, "d3d11va");
        // A genuine decoded frame varies; a uniform buffer means blank/error.
        bool uniform = true;
        for (size_t k = 1; k < npix; k++) {
            if (pix[k] != pix[0]) { uniform = false; break; }
        }
        printf("hwdec: hwdec-current=%s decoder-frame-drop-count=%s "
               "non-uniform=%d\n", cur ? cur : "(null)",
               drops ? drops : "?", !uniform);
        if (!engaged) {
            fprintf(stderr, "FAIL: hwdec-current=%s, expected d3d11va "
                    "(hardware decode did not engage through the render API)\n",
                    cur ? cur : "(null)");
            rc = 1;
        }
        if (uniform) {
            fprintf(stderr, "FAIL: rendered frame is a single uniform colour "
                    "(decode/render produced no content)\n");
            rc = 1;
        }
        if (rc == 0)
            printf("PASS: d3d11va hwdec engaged through the render API\n");
        mpv_free(cur);
        mpv_free(drops);
    }

    free(pix);
    ID3D11Texture2D_Release(staging);
    mpv_render_context_free(rctx);

    if (g_refcount) {
        // Probe point 2: after context free everything must be down too
        // (covers the destroy() teardown path).
        ULONG refs_post_free = tex_public_refs(rt);
        printf("refcount: post-render=%lu post-free=%lu (expect 1/1)\n",
               (unsigned long)refs_post_render, (unsigned long)refs_post_free);
        if (refs_post_render != 1) {
            fprintf(stderr, "FAIL: engine retained %lu reference(s) on the "
                    "host texture after mpv_render_context_render returned "
                    "(blocks IDXGISwapChain::ResizeBuffers for backbuffer "
                    "hosts)\n", (unsigned long)refs_post_render - 1);
            rc = 1;
        }
        if (refs_post_free != 1) {
            fprintf(stderr, "FAIL: %lu reference(s) leaked past "
                    "mpv_render_context_free\n",
                    (unsigned long)refs_post_free - 1);
            rc = 1;
        }
        if (rc == 0)
            printf("PASS: host is sole owner of the target after render\n");
    }

    ID3D11Texture2D_Release(rt);
    mpv_destroy(mpv);
    d3d_uninit();
    return rc;
}

// --- Phase 4b: on-rig HDR fidelity vs the windowed path --------------------
//
// Drives mpv through the render-API D3D11 backend on the real GPU (--hw) with
// the windowed run's HDR options, loads the SAME golden.lua capture script the
// windowed run-hdrval.ps1 uses, and lets it produce a screenshot PNG + JSON
// sidecar via the gpu_next_core_screenshot path -- so the artifacts are
// directly comparable to the windowed ref. A frame is rendered once into a
// throwaway HDR target (BT.2020/PQ TARGET_COLORSPACE) to drive the pipeline;
// golden.lua's screenshot is independent of that target.
static int run_shot(const char *clip, const char *pts, const char *script)
{
    d3d_init();

    mpv_handle *mpv = mpv_start();
    mpv_set_option_string(mpv, "audio", "no");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "pause", "yes");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "osd-level", "0");
    mpv_set_option_string(mpv, "interpolation", "no");
    // Match run-hdrval.ps1's HDR render options so the render-API screenshot is
    // comparable to the windowed ref (these drive gpu_next_core_screenshot).
    mpv_set_option_string(mpv, "tone-mapping", "bt.2390");
    mpv_set_option_string(mpv, "hdr-compute-peak", "no");
    mpv_set_option_string(mpv, "hr-seek", "yes");
    mpv_set_option_string(mpv, "start", pts);
    mpv_set_option_string(mpv, "hwdec", "no");
    if (script)
        mpv_set_option_string(mpv, "scripts", script);
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize failed");
    mpv_request_log_messages(mpv, "error");

    mpv_d3d11_init_params d3d11_init = { .device = (void *)d3d_dev };
    mpv_render_param cparams[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)API_TYPE},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &d3d11_init},
        {0},
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, cparams);
    if (err < 0) {
        drain_log(mpv);
        fprintf(stderr, "FATAL: mpv_render_context_create -> %d (%s)\n",
                err, mpv_error_string(err));
        return 1;
    }

    const char *cmd[] = {"loadfile", clip, NULL};
    if (mpv_command(mpv, cmd) < 0)
        die("loadfile failed");

    const int w = 1920, h = 1080;
    ID3D11Texture2D *rt = make_rendertarget(w, h, DXGI_FORMAT_R10G10B10A2_UNORM);

    bool restarted = false, rendered = false, shutdown = false;
    for (int i = 0; i < 4000 && !shutdown; i++) {
        mpv_event *ev = mpv_wait_event(mpv, 0.05);
        if (ev->event_id == MPV_EVENT_SHUTDOWN) {  // golden.lua captured + quit
            shutdown = true;
            break;
        }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            fprintf(stderr, "[mpv:%s] %s: %s", m->level, m->prefix, m->text);
        }
        if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART)
            restarted = true;

        uint64_t flags = mpv_render_context_update(rctx);
        if (restarted && !rendered && (flags & MPV_RENDER_UPDATE_FRAME)) {
            mpv_d3d11_tex d3dtex = { .tex = (void *)rt, .w = w, .h = h };
            mpv_render_param_target_colorspace tc;
            fill_target_colorspace(CSP_PQDISP, &tc);
            int block = 1;
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_D3D11_TEX, &d3dtex},
                {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                {MPV_RENDER_PARAM_TARGET_COLORSPACE, &tc},
                {0},
            };
            mpv_render_context_render(rctx, rp);
            rendered = true;
        }
    }

    printf("run_shot %s @ %s (rendered=%d, captured-and-quit=%d)\n",
           clip, pts, rendered, shutdown);

    ID3D11Texture2D_Release(rt);
    mpv_render_context_free(rctx);
    mpv_destroy(mpv);
    d3d_uninit();
    return shutdown ? 0 : 1;
}

int main(int argc, char **argv)
{
    // Optional leading flags: --hw (real GPU), --shot/--script for Phase 4b.
    const char *script = NULL;
    bool shot_mode = false;
    int a = 1;
    for (; a < argc; a++) {
        if (strcmp(argv[a], "--hw") == 0)
            g_hw_device = true;
        else if (strcmp(argv[a], "--hwdec") == 0)
            g_hwdec = g_hw_device = true;   // hardware decode needs the real GPU
        else if (strcmp(argv[a], "--refcount") == 0)
            g_refcount = true;
        else if (strcmp(argv[a], "--shot") == 0)
            shot_mode = true;
        else if (strcmp(argv[a], "--script") == 0 && a + 1 < argc)
            script = argv[++a];
        else
            break;
    }
    argc -= a; argv += a;   // argv[0..] now the positional args

    if (shot_mode) {
        if (argc != 2)
            die("usage: rapi_harness_d3d11 --hw --script <golden.lua> --shot <clip> <pts>");
        return run_shot(argv[0], argv[1], script);
    }

    if (argc >= 1 && strcmp(argv[0], "--probe") == 0)
        return run_probe();

    if (argc >= 5 && argc <= 7) {
        int w = atoi(argv[2]), h = atoi(argv[3]);
        if (w <= 0 || h <= 0)
            die("width/height must be positive");
        enum csp_mode csp = parse_csp(argc >= 6 ? argv[5] : NULL);
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (argc == 7) {
            if (strcmp(argv[6], "rgba8") == 0)
                fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (strcmp(argv[6], "rgb10a2") == 0)
                fmt = DXGI_FORMAT_R10G10B10A2_UNORM;
            else if (strcmp(argv[6], "rgba16f") == 0)
                fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
            else
                die("unknown fmt (want: rgba8 | rgb10a2 | rgba16f)");
        }
        return run_render(argv[0], argv[1], w, h, argv[4], csp, fmt);
    }

    fprintf(stderr,
            "usage: rapi_harness_d3d11 [--hw] --probe\n"
            "       rapi_harness_d3d11 [--hw] [--refcount] [--hwdec] <clip> <pts> <w> <h> <out.raw> "
            "[none|srgb|pq2020|pqdisp] [rgba8|rgb10a2|rgba16f]\n"
            "       rapi_harness_d3d11 --hw --script <golden.lua> --shot <clip> <pts>\n");
    return 1;
}
