// Minimal bare-Win32 HDR present host for the libmpv render API, D3D11 backend
// (MPV_RENDER_API_TYPE_PL_D3D11) -- the Plan-2 Phase-5a verification host,
// amended per the Phase-5b host contract (hdr-phase5b-assessment.md §2-§4).
//
//   rapi_hdr_present <clip>                  [interactive visual check]
//   rapi_hdr_present --resize-test <clip>    [P5b.1 acceptance: programmatic
//                                             mid-playback resizes, auto-quit,
//                                             exit 0 iff every ResizeBuffers
//                                             succeeded]
//
// Framework-free on purpose: it proves the mpv render-API HDR path end-to-end
// in isolation (no Qt/WinUI3/Composition layers to hide a bug), and is the
// reference for the exact call sequence a production shell ports:
//
//   per frame:  GetBuffer(0) -> mpv_render_context_render(D3D11_TEX +
//               TARGET_COLORSPACE) -> Release(backbuffer) -> Present(1, 0)
//
// Phase-5b host contract demonstrated here:
//   - device on the HIGH-PERFORMANCE adapter (EnumAdapterByGpuPreference),
//     not adapter 0 = the iGPU on hybrid laptops (§3);
//   - display peak queried HMONITOR-based across all adapters, NOT via
//     GetContainingOutput (which fails silently cross-adapter) (§4);
//   - direct backbuffer rendering + ResizeBuffers between frames, legal since
//     P5b.1 (mpv holds no reference on the texture between render calls).
//
// It opens a real DXGI HDR10 swapchain (R10G10B10A2_UNORM +
// SetColorSpace1(BT.2020/PQ) + SetHDRMetaData) on the NVIDIA GPU and plays the
// clip full-window. Watch it side-by-side with windowed `mpv --vo=gpu-next
// --gpu-api=d3d11` on the HDR display -- they should look identical.
//
// Run with the display in Windows HDR mode. Esc / close window to quit.

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_d3d11.h>

static void die(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(1); }
#define HR(call) do { HRESULT _hr=(call); if (FAILED(_hr)) { \
    fprintf(stderr, "FATAL: %s -> 0x%08lx\n", #call, (unsigned long)_hr); exit(1);} } while (0)

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain3      *g_swap;
static HWND                 g_hwnd;
static UINT                 g_w = 1920, g_h = 1080;
static bool                 g_resize_pending;

// --resize-test state: drive SetWindowPos toggles from the render loop and
// count ResizeBuffers outcomes; any failure is a P5b.1 regression.
static bool g_resize_test;
static int  g_frames;          // frames rendered
static int  g_resize_ok, g_resize_fail, g_resize_sent;

static mpv_handle         *g_mpv;
static mpv_render_context *g_rctx;
static volatile LONG       g_mpv_events;   // wakeup callback fired
static volatile LONG       g_mpv_update;   // render-update callback fired
static bool                g_quit;

// Display HDR caps, queried from the output the swapchain is on. The render-API
// host must do this itself (mpv has no swapchain to query) and pass the
// display's real peak as the tone-map target -- the windowed path does the
// equivalent via --target-colorspace-hint=yes. Defaults are a safe 1000/0.005
// until the query succeeds.
static float g_disp_max_luma = 1000.0f;
static float g_disp_min_luma = 0.005f;

// --- D3D11 + DXGI HDR10 swapchain ------------------------------------------

static void create_device(void)
{
    static const D3D_FEATURE_LEVEL fl[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    // Phase-5b §3: D3D11CreateDevice(NULL, ...) = adapter 0 = the iGPU on
    // hybrid laptops, which cannot sustain gpu-next 4K60 tone-mapping
    // (measured ~46 VO drops/s vs 0 on the dGPU). Ask DXGI for the
    // high-performance adapter explicitly; the OS handles presenting across
    // adapters when the panel belongs to the other GPU.
    IDXGIAdapter1 *adapter = NULL;
    IDXGIFactory6 *f6 = NULL;
    if (SUCCEEDED(CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&f6))) {
        if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1,
                (void **)&adapter)))
            adapter = NULL;
        IDXGIFactory6_Release(f6);
    }
    if (adapter) {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
            printf("device: high-performance adapter \"%ls\"\n", ad.Description);
    } else {
        fprintf(stderr, "WARNING: no IDXGIFactory6/high-performance adapter; "
                        "falling back to adapter 0 (may be the iGPU on hybrid "
                        "systems).\n");
    }
    HR(D3D11CreateDevice((IDXGIAdapter *)adapter,
                         adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                 : D3D_DRIVER_TYPE_HARDWARE,
                         NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                         fl, (UINT)(sizeof(fl)/sizeof(fl[0])),
                         D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx));
    if (adapter)
        IDXGIAdapter1_Release(adapter);
}

static void create_swapchain(HWND hwnd)
{
    IDXGIFactory2 *factory = NULL;
    HR(CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void **)&factory));

    DXGI_SWAP_CHAIN_DESC1 scd = {
        .Width = g_w, .Height = g_h,
        .Format = DXGI_FORMAT_R10G10B10A2_UNORM,   // HDR10
        .SampleDesc = { .Count = 1 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS,
        .BufferCount = 2,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
    };
    IDXGISwapChain1 *sc1 = NULL;
    HR(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, hwnd,
                                            &scd, NULL, NULL, &sc1));
    IDXGIFactory2_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    HR(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&g_swap));
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory2_Release(factory);

    // Declare the backbuffer BT.2020 / PQ (HDR10).
    UINT support = 0;
    if (SUCCEEDED(IDXGISwapChain3_CheckColorSpaceSupport(g_swap,
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        HR(IDXGISwapChain3_SetColorSpace1(g_swap,
               DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    } else {
        fprintf(stderr, "WARNING: HDR10 colorspace not supported by this "
                        "output (display HDR off?). Presenting anyway.\n");
    }

    // Mastering / content light level (same metadata the windowed swapchain
    // negotiates: 1000 nit, MaxCLL 1000, MaxFALL 400, BT.2020 primaries).
    IDXGISwapChain4 *sc4 = NULL;
    if (SUCCEEDED(IDXGISwapChain3_QueryInterface(g_swap, &IID_IDXGISwapChain4,
                                                 (void **)&sc4))) {
        DXGI_HDR_METADATA_HDR10 md = {
            .RedPrimary   = { 34000, 16000 },   // BT.2020, in 0.00002 units
            .GreenPrimary = { 13250, 34500 },
            .BluePrimary  = {  7500,  3000 },
            .WhitePoint   = { 15635, 16450 },
            .MaxMasteringLuminance = 1000 * 10000,   // in 0.0001 nit
            .MinMasteringLuminance = 50,             // 0.005 nit
            .MaxContentLightLevel = 1000,
            .MaxFrameAverageLightLevel = 400,
        };
        IDXGISwapChain4_SetHDRMetaData(sc4, DXGI_HDR_METADATA_TYPE_HDR10,
                                       sizeof(md), &md);
        IDXGISwapChain4_Release(sc4);
    }
}

// Query the HDR luminance characteristics of the display the WINDOW is on,
// via IDXGIOutput6::GetDesc1(). This is the value the tone-mapper must target
// so the render matches what the display can actually show (the windowed path
// gets it from --target-colorspace-hint=yes + the swapchain).
//
// Phase-5b §4: do NOT use IDXGISwapChain::GetContainingOutput for this. With
// the device on the dGPU and the panel owned by the iGPU (hybrid laptops,
// once §3 is applied) it fails silently and the host keeps defaults --
// observed as tone-mapping to 1000 nits on a 271-nit panel, visibly wrong
// with no error anywhere. Robust pattern: HMONITOR from the window, then
// search every adapter's outputs for it. Re-queried after each resize (the
// window may have moved to another monitor).
static void query_display_hdr(HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory2 *factory = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void **)&factory)))
        return;
    bool found = false;
    IDXGIAdapter1 *ad = NULL;
    for (UINT i = 0; !found && IDXGIFactory2_EnumAdapters1(factory, i, &ad)
                     != DXGI_ERROR_NOT_FOUND; i++) {
        IDXGIOutput *out = NULL;
        for (UINT j = 0; !found && IDXGIAdapter1_EnumOutputs(ad, j, &out)
                         != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od)) && od.Monitor == mon) {
                IDXGIOutput6 *out6 = NULL;
                if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput6,
                                                         (void **)&out6))) {
                    DXGI_OUTPUT_DESC1 d;
                    if (SUCCEEDED(IDXGIOutput6_GetDesc1(out6, &d))) {
                        g_disp_max_luma = d.MaxLuminance;
                        g_disp_min_luma = d.MinLuminance;
                        printf("display HDR: peak=%.0f nits, min=%.4f nits, "
                               "MaxFullFrame=%.0f nits, colorspace=%d\n",
                               d.MaxLuminance, d.MinLuminance,
                               d.MaxFullFrameLuminance, (int)d.ColorSpace);
                        found = true;
                    }
                    IDXGIOutput6_Release(out6);
                }
            }
            IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory2_Release(factory);
    if (!found)
        fprintf(stderr, "WARNING: no DXGI output matches the window's monitor; "
                        "keeping %.0f/%.4f-nit defaults.\n",
                g_disp_max_luma, g_disp_min_luma);
}

// --- mpv render API --------------------------------------------------------

static void on_mpv_update(void *unused) { InterlockedExchange(&g_mpv_update, 1); }
static void on_mpv_wakeup(void *unused) { InterlockedExchange(&g_mpv_events, 1); }

static void pump_mpv_events(void)
{
    while (1) {
        mpv_event *ev = mpv_wait_event(g_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            g_quit = true;
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            fprintf(stderr, "[mpv:%s] %s: %s", m->level, m->prefix, m->text);
        }
    }
}

static void render_frame(void)
{
    uint64_t flags = mpv_render_context_update(g_rctx);
    if (!(flags & MPV_RENDER_UPDATE_FRAME))
        return;

    if (g_resize_pending) {
        // P5b.1 contract (render_d3d11.h "Reference lifetime"): mpv releases
        // its wrap of the backbuffer before mpv_render_context_render()
        // returns, and this host drops its own GetBuffer() reference right
        // after the render call -- so between frames there are zero
        // outstanding backbuffer references and ResizeBuffers is legal with
        // no intermediate-texture indirection. (Before P5b.1 the engine kept
        // one COM reference across calls and this failed every time with
        // DXGI_ERROR_INVALID_CALL.)
        HRESULT hr = IDXGISwapChain3_ResizeBuffers(g_swap, 0, g_w, g_h,
                                                   DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            g_resize_fail++;
            fprintf(stderr, "ResizeBuffers(%ux%u) -> 0x%08lx  *** P5b.1 "
                    "REGRESSION ***\n", (unsigned)g_w, (unsigned)g_h,
                    (unsigned long)hr);
            if (g_resize_test) { g_quit = true; return; }
        } else {
            g_resize_ok++;
            printf("ResizeBuffers -> %ux%u OK (#%d)\n",
                   (unsigned)g_w, (unsigned)g_h, g_resize_ok);
        }
        g_resize_pending = false;
        // The window may have moved monitors; the tone-map target must follow
        // the display it is actually on (Phase-5b §4).
        query_display_hdr(g_hwnd);
    }

    ID3D11Texture2D *bb = NULL;
    HR(IDXGISwapChain3_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&bb));

    mpv_d3d11_tex tex = { .tex = bb, .w = (int)g_w, .h = (int)g_h };
    // Target = this display's real BT.2020/PQ capabilities (peak luminance from
    // IDXGIOutput6), so the tone-map compresses to what the screen can show --
    // matching the windowed --target-colorspace-hint path. max_cll/max_fall are
    // content-side (mpv reads them from the clip), so they are left unset here.
    mpv_render_param_target_colorspace tc = {
        .primaries = MPV_COLOR_PRIMARIES_BT_2020,
        .transfer  = MPV_COLOR_TRANSFER_PQ,
        .hdr = { .max_luma = g_disp_max_luma, .min_luma = g_disp_min_luma },
    };
    int block = 0;   // don't block the UI thread on frame timing
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_D3D11_TEX, &tex},
        {MPV_RENDER_PARAM_TARGET_COLORSPACE, &tc},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {0},
    };
    int err = mpv_render_context_render(g_rctx, rp);
    ID3D11Texture2D_Release(bb);
    if (err < 0)
        fprintf(stderr, "render -> %d (%s)\n", err, mpv_error_string(err));

    IDXGISwapChain3_Present(g_swap, 1, 0);

    g_frames++;
    // --resize-test: every 60 rendered frames, toggle the client size between
    // 1280x720 and 1600x900 via SetWindowPos; WM_SIZE marks g_resize_pending
    // and the checked ResizeBuffers above runs before the next GetBuffer.
    // This is the mid-playback resize acceptance Phase 5a lacked (the trigger
    // ARCA's automated resize test used, ported per Phase-5b §2.3).
    if (g_resize_test && g_resize_sent < 6 && g_frames % 60 == 0) {
        int cw = (g_resize_sent & 1) ? 1600 : 1280;
        int ch = (g_resize_sent & 1) ? 900 : 720;
        RECT r = { 0, 0, cw, ch };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_resize_sent++;
    }
    if (g_resize_test && g_resize_sent >= 6 && !g_resize_pending &&
        g_resize_ok + g_resize_fail >= 6)
        g_quit = true;   // all toggles processed
}

// --- window ----------------------------------------------------------------

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            UINT nw = LOWORD(lp), nh = HIWORD(lp);
            if (nw && nh && (nw != g_w || nh != g_h)) {
                g_w = nw; g_h = nh; g_resize_pending = true;
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { g_quit = true; PostQuitMessage(0); }
        return 0;
    case WM_CLOSE:
        g_quit = true; PostQuitMessage(0); return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int main(int argc, char **argv)
{
    int a = 1;
    if (a < argc && strcmp(argv[a], "--resize-test") == 0) {
        g_resize_test = true;
        a++;
    }
    if (a >= argc)
        die("usage: rapi_hdr_present [--resize-test] <clip>");
    const char *clip = argv[a];

    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSEX wc = {
        .cbSize = sizeof(wc), .lpfnWndProc = wnd_proc, .hInstance = inst,
        .hCursor = LoadCursor(NULL, IDC_ARROW),
        .lpszClassName = "rapi_hdr_present",
    };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindowEx(0, wc.lpszClassName,
                               "mpv render-API HDR (pl-d3d11)  [Esc to quit]",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1920, 1080,
                               NULL, NULL, inst, NULL);
    if (!hwnd)
        die("CreateWindow failed");
    g_hwnd = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    g_w = rc.right - rc.left; g_h = rc.bottom - rc.top;
    g_resize_pending = false;   // creation-time WM_SIZE is absorbed above

    create_device();
    create_swapchain(hwnd);
    query_display_hdr(hwnd);

    g_mpv = mpv_create();
    if (!g_mpv)
        die("mpv_create failed");
    mpv_set_option_string(g_mpv, "vo", "libmpv");
    mpv_set_option_string(g_mpv, "terminal", "no");
    mpv_set_option_string(g_mpv, "config", "no");
    mpv_set_option_string(g_mpv, "osc", "no");
    mpv_set_option_string(g_mpv, "input-default-bindings", "no");
    mpv_set_option_string(g_mpv, "audio", "no");
    mpv_set_option_string(g_mpv, "tone-mapping", "bt.2390");
    mpv_set_option_string(g_mpv, "hdr-compute-peak", "no");
    mpv_set_option_string(g_mpv, "loop", "inf");
    if (mpv_initialize(g_mpv) < 0)
        die("mpv_initialize failed");
    mpv_request_log_messages(g_mpv, "info");
    mpv_set_wakeup_callback(g_mpv, on_mpv_wakeup, NULL);

    mpv_d3d11_init_params d3d11_init = { .device = (void *)g_dev };
    mpv_render_param cparams[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_PL_D3D11},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &d3d11_init},
        {0},
    };
    if (mpv_render_context_create(&g_rctx, g_mpv, cparams) < 0)
        die("mpv_render_context_create(pl-d3d11) failed");
    mpv_render_context_set_update_callback(g_rctx, on_mpv_update, NULL);

    const char *cmd[] = {"loadfile", clip, NULL};
    if (mpv_command(g_mpv, cmd) < 0)
        die("loadfile failed");

    printf("Playing %s via MPV_RENDER_API_TYPE_PL_D3D11 into a BT.2020/PQ "
           "HDR10 swapchain. Esc to quit.\n", clip);

    // Single-threaded UI + render loop: messages, mpv events, then render when
    // mpv signals an update. mpv_render_context_render runs on this (the
    // immediate-context) thread, as the render API requires.
    ULONGLONG t0 = GetTickCount64();
    while (!g_quit) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit)
            break;
        if (g_resize_test && GetTickCount64() - t0 > 90000) {
            fprintf(stderr, "resize-test: 90s watchdog hit (frames=%d, "
                    "resizes=%d/%d)\n", g_frames, g_resize_ok, g_resize_sent);
            break;
        }
        if (InterlockedExchange(&g_mpv_events, 0))
            pump_mpv_events();
        if (InterlockedExchange(&g_mpv_update, 0))
            render_frame();
        else
            Sleep(1);   // idle a touch so we don't spin the CPU
    }

    mpv_render_context_free(g_rctx);
    mpv_destroy(g_mpv);
    if (g_swap) IDXGISwapChain3_Release(g_swap);
    if (g_ctx)  ID3D11DeviceContext_Release(g_ctx);
    if (g_dev)  ID3D11Device_Release(g_dev);
    DestroyWindow(hwnd);

    int ret = 0;
    if (g_resize_test) {
        // Acceptance: every programmatic mid-playback resize must have gone
        // through ResizeBuffers successfully while wrapping the backbuffer
        // directly (>= 4 guards against a stall producing a hollow pass).
        bool pass = g_resize_fail == 0 && g_resize_sent >= 4 &&
                    g_resize_ok >= g_resize_sent;
        printf("resize-test: %d sent, %d ResizeBuffers OK, %d failed, "
               "%d frames -> %s\n", g_resize_sent, g_resize_ok, g_resize_fail,
               g_frames, pass ? "PASS" : "FAIL");
        ret = pass ? 0 : 1;
    }
    return ret;
}
