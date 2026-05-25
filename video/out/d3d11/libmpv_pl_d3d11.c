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
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_d3d11 pl_d3d11;
    pl_tex wrapped_fbo;
};

static int init(struct libmpv_pl_context *ctx, mpv_render_param *params)
{
    // P1.2 stub: the file compiles and the const struct exists, but the
    // backend is not yet registered in libmpv_gpu_next.c::context_backends[],
    // so this entry point is unreachable from libmpv. Phase 2 implements
    // the body (pl_d3d11_create against the host's ID3D11Device) and the
    // registry entry.
    (void)ctx;
    (void)params;
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static int wrap_fbo(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out)
{
    (void)ctx;
    (void)params;
    (void)out;
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void done_frame(struct libmpv_pl_context *ctx, bool display_synced)
{
    (void)ctx;
    (void)display_synced;
}

static void destroy(struct libmpv_pl_context *ctx)
{
    (void)ctx;
}

const struct libmpv_pl_context_fns libmpv_pl_context_d3d11 = {
    .api_name = MPV_RENDER_API_TYPE_PL_D3D11,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
