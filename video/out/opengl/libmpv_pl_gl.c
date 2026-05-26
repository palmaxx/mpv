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

#include <libplacebo/opengl.h>

#include "common.h"
#include "context.h"
#include "mpv/render_gl.h"
#include "options/m_config.h"
#include "ra_gl.h"
#include "ta/ta_talloc.h"
#include "video/out/gpu/ra.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/placebo/utils.h"

struct priv {
    GL *gl;
    pl_opengl pl_gl;
    pl_tex wrapped_fbo;
};

static int init(struct libmpv_pl_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_opengl_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, NULL);
    if (!init_params)
        return MPV_ERROR_INVALID_PARAMETER;

    p->gl = talloc_zero(p, GL);
    mpgl_load_functions2(p->gl, init_params->get_proc_address,
                         init_params->get_proc_address_ctx, NULL, ctx->log);
    if (!p->gl->version && !p->gl->es) {
        MP_FATAL(ctx, "OpenGL not initialized.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    // Build a blank ra_ctx alongside the libplacebo pl_opengl so the render
    // backend can drive ra_hwdec_ctx (hwdec interop). Mirrors libmpv_gl.c's
    // setup; the ra_ctx's swapchain is dummy (we render via pl_opengl_wrap,
    // not the ra swapchain), so the ra_ctx_params{0} is intentional.
    ctx->ra_ctx = talloc_zero(p, struct ra_ctx);
    ctx->ra_ctx->log = ctx->log;
    ctx->ra_ctx->global = ctx->global;
    ctx->ra_ctx->opts = (struct ra_ctx_opts){ .allow_sw = true };
    struct ra_ctx_params gl_params = {0};
    p->gl->SwapInterval = NULL; // don't perturb host's swap-interval state
    if (!ra_gl_ctx_init(ctx->ra_ctx, p->gl, gl_params))
        return MPV_ERROR_UNSUPPORTED;

    struct ra_ctx_opts *ctx_opts = mp_get_config_group(ctx, ctx->global, &ra_ctx_conf);
    ctx->ra_ctx->opts.debug = ctx_opts->debug;
    p->gl->debug_context = ctx_opts->debug;
    ra_gl_set_debug(ctx->ra_ctx->ra, ctx_opts->debug);
    talloc_free(ctx_opts);

    ctx->ra = ctx->ra_ctx->ra;

    ctx->pllog = mppl_log_create(p, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_GENERIC;

    struct pl_opengl_params pl_params = *pl_opengl_params(
        .get_proc_addr_ex = (void *)p->gl->get_fn,
        .proc_ctx = p->gl->fn_ctx,
        // The render-API host owns the GL context; honour its choice rather
        // than rejecting a software rasterizer, as libmpv_gl.c does for the
        // gpu backend (ra_ctx_opts.allow_sw = true).
        .allow_software = true,
    );
    p->pl_gl = pl_opengl_create(ctx->pllog, &pl_params);
    if (!p->pl_gl) {
        MP_FATAL(ctx, "libplacebo OpenGL context creation failed.\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    ctx->gpu = p->pl_gl->gpu;
    return 0;
}

static int wrap_fbo(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out)
{
    struct priv *p = ctx->priv;

    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    // Free the previous wrap before producing a new one; pl_tex_destroy()
    // is the documented teardown for pl_opengl_wrap() (see libplacebo
    // opengl.h: "This wrapper can be destroyed by simply calling
    // pl_tex_destroy on it").
    if (p->wrapped_fbo)
        pl_tex_destroy(ctx->gpu, &p->wrapped_fbo);

    p->wrapped_fbo = pl_opengl_wrap(ctx->gpu, pl_opengl_wrap_params(
        .framebuffer = fbo->fbo,
        .width = fbo->w,
        .height = fbo->h,
        .iformat = fbo->internal_format,
    ));
    if (!p->wrapped_fbo) {
        MP_ERR(ctx, "Failed to wrap host FBO %d (%dx%d) as pl_tex.\n",
               fbo->fbo, fbo->w, fbo->h);
        return MPV_ERROR_UNSUPPORTED;
    }
    *out = p->wrapped_fbo;
    return 0;
}

static void done_frame(struct libmpv_pl_context *ctx, bool display_synced)
{
    // The libmpv render API has no swapchain: the host owns surface
    // presentation, so there is nothing to submit here. GL command flush
    // happens implicitly when the host swaps its own surface.
}

static void destroy(struct libmpv_pl_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    if (p->wrapped_fbo)
        pl_tex_destroy(ctx->gpu, &p->wrapped_fbo);
    if (p->pl_gl)
        pl_opengl_destroy(&p->pl_gl);
    if (ctx->ra_ctx)
        ra_gl_ctx_uninit(ctx->ra_ctx);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_pl_context_fns libmpv_pl_context_gl = {
    .api_name = MPV_RENDER_API_TYPE_PL_OPENGL,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
