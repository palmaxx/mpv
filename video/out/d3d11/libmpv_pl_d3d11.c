/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libplacebo/d3d11.h>

#include "common/common.h"
#include "common/msg.h"
#include "mpv/render_d3d11.h"
#include "ta/ta_talloc.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu/spirv.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/utils.h"
#include "ra_d3d11.h"

struct priv {
    pl_d3d11 pl_d3d11;
};

static void uninit_ra(struct libmpv_pl_context *ctx)
{
    if (!ctx->ra_ctx)
        return;

    if (ctx->ra_ctx->spirv && ctx->ra_ctx->spirv->fns->uninit)
        ctx->ra_ctx->spirv->fns->uninit(ctx->ra_ctx);
    if (ctx->ra_ctx->ra) {
        ctx->ra_ctx->ra->fns->destroy(ctx->ra_ctx->ra);
        ctx->ra_ctx->ra = NULL;
    }
    talloc_free(ctx->ra_ctx);
    ctx->ra_ctx = NULL;
    ctx->ra = NULL;
}

static int init(struct libmpv_pl_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_d3d11_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_INIT_PARAMS, NULL);
    if (!init_params || !init_params->device)
        return MPV_ERROR_INVALID_PARAMETER;

    ID3D11Device *device = (ID3D11Device *)init_params->device;
    // The gpu-next renderer's compute-shader paths (peak detection, FSR,
    // certain scalers) require D3D_FEATURE_LEVEL_11_0 functionality. Refuse
    // 10level9 devices up-front rather than letting libplacebo fail late
    // with a confusing capability error.
    D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(device);
    if (fl < D3D_FEATURE_LEVEL_11_0) {
        MP_FATAL(ctx, "D3D11 device feature level 0x%x is too low (need at "
                      "least 0x%x for the gpu-next renderer).\n",
                 (unsigned)fl, (unsigned)D3D_FEATURE_LEVEL_11_0);
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->pllog = mppl_log_create(p, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_GENERIC;

    // Pass the host's device through; libplacebo takes its own COM reference
    // on create and releases it in pl_d3d11_destroy. All other fields stay
    // at PL_D3D11_DEFAULTS (allow_software = true, identical to the GL
    // backend's policy of trusting the host's choice of GPU).
    struct pl_d3d11_params pl_params = *pl_d3d11_params(
        .device = device,
    );
    p->pl_d3d11 = pl_d3d11_create(ctx->pllog, &pl_params);
    if (!p->pl_d3d11) {
        MP_FATAL(ctx, "libplacebo D3D11 context creation failed.\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    ctx->gpu = p->pl_d3d11->gpu;

    // The hwdec interop needs an ra_d3d11 using the host's device. Rendering
    // continues through libplacebo; this RA is only used for decoded surfaces.
    ctx->ra_ctx = talloc_zero(p, struct ra_ctx);
    ctx->ra_ctx->log = ctx->log;
    ctx->ra_ctx->global = ctx->global;
    if (!spirv_compiler_init(ctx->ra_ctx)) {
        MP_WARN(ctx, "D3D11 hardware decoding is unavailable.\n");
        uninit_ra(ctx);
        return 0;
    }

    ctx->ra_ctx->ra = ra_d3d11_create(device, ctx->log, ctx->ra_ctx->spirv);
    if (!ctx->ra_ctx->ra) {
        MP_WARN(ctx, "D3D11 hardware decoding is unavailable.\n");
        uninit_ra(ctx);
        return 0;
    }
    ctx->ra = ctx->ra_ctx->ra;
    return 0;
}

static int wrap_fbo(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out)
{
    mpv_d3d11_tex *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_D3D11_TEX, NULL);
    if (!fbo || !fbo->tex)
        return MPV_ERROR_INVALID_PARAMETER;

    // The wrap holds one COM reference on the host texture (released by
    // pl_tex_destroy, the documented teardown for pl_d3d11_wrap). The caller
    // owns the wrap and destroys it by end-of-render, so the reference never
    // survives past mpv_render_context_render() -- required for hosts that
    // wrap a DXGI backbuffer directly, since ResizeBuffers demands zero
    // outstanding references (the render_d3d11.h lifetime contract).
    //
    // libplacebo introspects the ID3D11Texture2D for format / dimensions /
    // bind flags; the wrap_params w/h field is only honoured for video
    // resources like NV12/P010, which are not valid render targets, so it is
    // left unset and the host-supplied w/h are validated post-wrap below.
    pl_tex wrap = pl_d3d11_wrap(ctx->gpu, pl_d3d11_wrap_params(
        .tex = (ID3D11Resource *)fbo->tex,
    ));
    if (!wrap) {
        MP_ERR(ctx, "Failed to wrap host D3D11 texture (%dx%d) as pl_tex.\n",
               fbo->w, fbo->h);
        return MPV_ERROR_UNSUPPORTED;
    }

    // Host contract check: if the host declared dimensions, they must match
    // what libplacebo introspected. A mismatch means the host is rendering to
    // a different surface than it thinks, so reject rather than render wrong.
    if ((fbo->w && wrap->params.w != fbo->w) ||
        (fbo->h && wrap->params.h != fbo->h))
    {
        MP_ERR(ctx, "Host D3D11 texture dimensions (%dx%d) do not match the "
                    "wrapped texture (%dx%d).\n", fbo->w, fbo->h,
               wrap->params.w, wrap->params.h);
        pl_tex_destroy(ctx->gpu, &wrap);
        return MPV_ERROR_INVALID_PARAMETER;
    }

    *out = wrap;
    return 0;
}

static int done_frame(struct libmpv_pl_context *ctx, bool display_synced)
{
    // The libmpv render API has no swapchain: the host owns surface
    // presentation, so there is nothing to submit here. D3D11 command
    // submission happens through libplacebo's immediate-context recording
    // and is flushed by render_backend_gpu_next via pl_gpu_flush().
    return 0;
}

static void destroy(struct libmpv_pl_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    uninit_ra(ctx);
    if (p->pl_d3d11)
        pl_d3d11_destroy(&p->pl_d3d11);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_pl_context_fns libmpv_pl_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_PL_D3D11,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
