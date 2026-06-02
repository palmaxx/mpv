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

#include <libplacebo/vulkan.h>

#include "common/common.h"
#include "common/msg.h"
#include "mpv/render_vulkan.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"

// Plan-2 Phase-6 scaffolding stub, parallel to the P1.2 D3D11 stub. The bodies
// (pl_vulkan_import / pl_vulkan_wrap + the per-frame acquire/release sync) and
// the context_backends[] registration land together in the impl commit, so the
// failure surface stays "init returns error and is destroyed cleanly" rather
// than a half-initialised context.

static int init(struct libmpv_pl_context *ctx, mpv_render_param *params)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static int wrap_fbo(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void done_frame(struct libmpv_pl_context *ctx, bool display_synced)
{
}

static void destroy(struct libmpv_pl_context *ctx)
{
}

const struct libmpv_pl_context_fns libmpv_pl_context_vulkan = {
    .api_name = MPV_RENDER_API_TYPE_PL_VULKAN,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
