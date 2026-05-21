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

#include <string.h>

#include <libplacebo/config.h>

#include "config.h"
#include "libmpv_gpu_next.h"

#include "core.h"
#include "mpv/render.h"
#include "ta/ta_talloc.h"
#include "video/out/libmpv.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
extern const struct libmpv_pl_context_fns libmpv_pl_context_gl;
#endif

static const struct libmpv_pl_context_fns *context_backends[] = {
#if HAVE_GL && defined(PL_HAVE_OPENGL)
    &libmpv_pl_context_gl,
#endif
    NULL,
};

struct priv {
    struct libmpv_pl_context *context;
    struct gpu_next_core *core;
};

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;

    for (int n = 0; context_backends[n]; n++) {
        const struct libmpv_pl_context_fns *backend = context_backends[n];
        if (strcmp(backend->api_name, api) == 0) {
            p->context = talloc_zero(NULL, struct libmpv_pl_context);
            *p->context = (struct libmpv_pl_context){
                .global = ctx->global,
                .log = ctx->log,
                .fns = backend,
            };
            break;
        }
    }

    if (!p->context)
        return MPV_ERROR_NOT_IMPLEMENTED;

    int err = p->context->fns->init(p->context, params);
    if (err < 0)
        return err;

    p->core = gpu_next_core_create(p->context->gpu, ctx->log,
                                   p->context->pllog);
    if (!p->core)
        return MPV_ERROR_GENERIC;

    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    pl_gpu gpu = p->context->gpu;

    return gpu_next_core_format_supported(gpu, imgfmt, false) ||
           gpu_next_core_format_supported(gpu, imgfmt, true);
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
}

static void reset(struct render_backend *ctx)
{
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    // The core owns libplacebo objects living on context->gpu, so it must be
    // torn down before the context that owns the gpu.
    if (p->core)
        gpu_next_core_destroy(&p->core);

    if (p->context) {
        p->context->fns->destroy(p->context);
        talloc_free(p->context->priv);
        talloc_free(p->context);
    }
}

const struct render_backend_fns render_backend_gpu_next = {
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .destroy = destroy,
};
