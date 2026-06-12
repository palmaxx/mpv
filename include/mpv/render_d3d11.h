/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_D3D11_H_
#define MPV_CLIENT_API_RENDER_D3D11_H_

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Direct3D 11 backend
 * -------------------
 *
 * This header contains definitions for using Direct3D 11 with the render.h
 * API. The backend renders through the libplacebo-based gpu-next pipeline
 * onto a host-provided ID3D11Texture2D, parallel to MPV_RENDER_API_TYPE_OPENGL
 * / MPV_RENDER_API_TYPE_PL_OPENGL but without the OpenGL interop hop and
 * with first-class HDR target surfaces.
 *
 * Use cases
 * ---------
 *
 * The OpenGL render API path (MPV_RENDER_API_TYPE_OPENGL or
 * MPV_RENDER_API_TYPE_PL_OPENGL) is structurally limited to SDR target
 * surfaces: the host's GL FBO is in practice RGBA8, no standard GL extension
 * surfaces HDR-capable target observability, and the GL/D3D interop hop adds
 * latency. The D3D11 backend supersedes the GL path for native Windows
 * hosts that want:
 *
 *   - HDR10 / scRGB target surfaces (R10G10B10A2_UNORM /
 *     R16G16B16A16_FLOAT swapchains with explicit colorspace negotiation),
 *   - reduced GPU-driver overhead vs the GL-on-D3D translation layers (ANGLE),
 *   - and the same libplacebo render pipeline as the windowed --vo=gpu-next
 *     path.
 *
 * Source-frame decode: decoded frames are currently uploaded to the render
 * device (software interop), like the other render-API backends. Zero-copy
 * import of d3d11va-decoded surfaces on the render API is not yet implemented
 * for this backend; only the windowed --vo=gpu-next path does d3d11va interop.
 *
 * On non-Windows platforms, this header is still installed but the
 * MPV_RENDER_API_TYPE_PL_D3D11 backend is absent — mpv_render_context_create()
 * will return MPV_ERROR_NOT_IMPLEMENTED.
 *
 * Threading
 * ---------
 *
 * The host's ID3D11Device's immediate context is not thread-safe by default.
 * libplacebo's D3D11 backend uses the immediate context for command
 * submission. Therefore: the host MUST call mpv_render_context_render() on
 * the same thread that performs its other ID3D11DeviceContext work, OR
 * serialize all ID3D11DeviceContext access (including mpv's) with a
 * host-owned mutex.
 *
 * Host-supplied texture state
 * ---------------------------
 *
 * The ID3D11Texture2D passed via MPV_RENDER_PARAM_D3D11_TEX must satisfy:
 *
 *   - D3D11_USAGE_DEFAULT (immutable / dynamic / staging are incompatible
 *     with libplacebo's render-target needs);
 *   - not mipmapped (MipLevels = 1);
 *   - not multisampled (SampleDesc.Count = 1); MSAA on the host swapchain
 *     must be resolved to a non-MSAA texture before being handed to mpv;
 *   - created by the same ID3D11Device passed via
 *     MPV_RENDER_PARAM_D3D11_INIT_PARAMS;
 *   - BindFlags containing at minimum D3D11_BIND_RENDER_TARGET. For
 *     libplacebo's compute paths (peak detection, FSR, certain scalers),
 *     D3D11_BIND_UNORDERED_ACCESS is also required; the standard recipe is
 *     to create the host's DXGI swapchain with
 *     DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS.
 *
 * Reference lifetime: the renderer takes one COM reference on the texture for
 * the duration of the render call and releases it before
 * mpv_render_context_render() returns — no mpv-held reference on the texture
 * survives between calls. Wrapping a DXGI swapchain backbuffer directly is
 * therefore supported: IDXGISwapChain::ResizeBuffers succeeds between render
 * calls provided the host has released its own GetBuffer() reference and
 * follows the standard DXGI resize rules for its other state. The host must
 * not modify the texture's state during a render call.
 *
 * Device feature level
 * --------------------
 *
 * The supplied device must report at least D3D_FEATURE_LEVEL_11_0 from
 * ID3D11Device::GetFeatureLevel(). Lower feature levels are not supported
 * (libplacebo can technically run on 10level9 with severe restrictions, but
 * the gpu-next renderer's compute-shader paths require full 11_0
 * functionality). mpv_render_context_create() returns MPV_ERROR_UNSUPPORTED
 * for lower feature levels.
 *
 * Device removal
 * --------------
 *
 * If the D3D11 device is lost (DXGI_ERROR_DEVICE_REMOVED, e.g. after a TDR
 * or driver upgrade), mpv_render_context_render() will return an error.
 * Recovery is the host's responsibility: destroy the mpv_render_context,
 * create a new ID3D11Device, and create a new mpv_render_context with the
 * fresh device.
 *
 * Colorspace and HDR
 * ------------------
 *
 * The host negotiates target colorspace and HDR metadata per frame via
 * MPV_RENDER_PARAM_TARGET_COLORSPACE (see render.h). When the host's DXGI
 * swapchain is configured for HDR10 (DXGI_FORMAT_R10G10B10A2_UNORM +
 * IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)),
 * pass:
 *
 *   mpv_render_param_target_colorspace tc = {
 *       .primaries = MPV_COLOR_PRIMARIES_BT_2020,
 *       .transfer  = MPV_COLOR_TRANSFER_PQ,
 *       .hdr       = { .max_luma = 1000, .max_cll = 1000, .max_fall = 400 },
 *   };
 *
 * For an SDR swapchain, omit the parameter entirely (renderer applies
 * sRGB defaults).
 *
 * The hdr metadata should describe the *display* the swapchain presents to,
 * not the content: query IDXGIOutput6::GetDesc1() for MaxLuminance /
 * MinLuminance and pass those, so the tone-mapper compresses to what the
 * screen can actually show (hardcoding e.g. 1000 nits over-brightens on a
 * dimmer panel). Caution on hybrid-GPU systems:
 * IDXGISwapChain::GetContainingOutput fails when the device lives on a
 * different adapter than the one owning the panel; the robust pattern is
 * MonitorFromWindow(), then searching every adapter's outputs for the
 * matching DXGI_OUTPUT_DESC.Monitor. Re-query when the window moves between
 * monitors.
 *
 * Adapter choice and border clearing
 * ----------------------------------
 *
 * On hybrid-GPU systems, D3D11CreateDevice(NULL, ...) selects adapter 0 —
 * typically the integrated GPU, which may be too slow for gpu-next's
 * tone-mapping at high resolutions. Create the device on the
 * high-performance adapter (IDXGIFactory6::EnumAdapterByGpuPreference with
 * DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE); the OS handles presenting across
 * adapters when the panel is owned by the other GPU.
 *
 * When the video does not cover the full target (letterboxing), mpv clears
 * the borders via libplacebo's pl_tex_clear, which requires a blittable
 * target; R10G10B10A2_UNORM is not blittable on some drivers (e.g. NVIDIA),
 * producing a per-frame clear failure. Such hosts should set
 * --border-background=none and clear the target themselves before each
 * render call.
 */

/**
 * For initializing the libplacebo D3D11 backend via
 * MPV_RENDER_PARAM_D3D11_INIT_PARAMS.
 */
typedef struct mpv_d3d11_init_params {
    /**
     * The host's ID3D11Device, as a void* for header-portability (cast from
     * ID3D11Device*). Must be non-NULL and report at least
     * D3D_FEATURE_LEVEL_11_0. libplacebo takes a reference to the device on
     * mpv_render_context_create() and releases it on
     * mpv_render_context_free().
     */
    void *device;
} mpv_d3d11_init_params;

/**
 * For MPV_RENDER_PARAM_D3D11_TEX.
 */
typedef struct mpv_d3d11_tex {
    /**
     * The render target texture, as a void* for header-portability (cast
     * from ID3D11Texture2D*). Must be a non-NULL 2D texture satisfying the
     * "Host-supplied texture state" requirements documented at the top of
     * this header.
     *
     * The host owns the texture's lifetime; the renderer takes a reference
     * only for the duration of the render call and releases it before the
     * call returns (see "Host-supplied texture state" above).
     */
    void *tex;
    /**
     * Texture dimensions in pixels. libplacebo introspects the actual size
     * from the ID3D11Texture2D, so these are an optional host contract check:
     * if non-zero, they are validated against the wrapped texture and a
     * mismatch fails the render call with MPV_ERROR_INVALID_PARAMETER. Leave
     * them 0 to skip the check and use the introspected dimensions as-is.
     */
    int w, h;
} mpv_d3d11_tex;

#ifdef __cplusplus
}
#endif

#endif
