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

#pragma once

#include <stdbool.h>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include "mpv/render.h"
#include "video/out/libmpv.h"

// Per-GPU-API surface-wrap layer, parallel to libmpv_gpu_context_fns. Manages
// the libplacebo-side interaction between libmpv and the host's render API
// (init / wrap target surface / present), which is not something the renderer
// core itself can do because the host's render-context handle is API-specific
// (an OpenGL function loader, a D3D11 device, a Vulkan handle, ...).
struct libmpv_pl_context {
    struct mpv_global *global;
    struct mp_log *log;
    const struct libmpv_pl_context_fns *fns;

    // Populated by ->init(); consumed by render_backend_gpu_next when it
    // creates the renderer core via gpu_next_core_create(gpu, log, pllog).
    pl_log pllog;
    pl_gpu gpu;

    void *priv;
};

struct libmpv_pl_context_fns {
    // The libmpv API type name (matched against MPV_RENDER_PARAM_API_TYPE);
    // same role as libmpv_gpu_context_fns.api_name.
    const char *api_name;

    // Like render_backend_fns.init, except the API type has already been
    // matched by the caller. Successful init must populate ctx->pllog and
    // ctx->gpu.
    int (*init)(struct libmpv_pl_context *ctx, mpv_render_param *params);

    // Wrap the host's render-target surface (described by params) as a
    // pl_tex. Returns a libmpv error code; on success *out is valid until
    // the next wrap_fbo() or done_frame() call.
    int (*wrap_fbo)(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out);

    // Signal end-of-render (the analogue of libmpv_gpu_context_fns.done_frame).
    void (*done_frame)(struct libmpv_pl_context *ctx, bool display_synced);

    // Free everything owned by the implementation, including ctx->gpu if the
    // impl created it.
    void (*destroy)(struct libmpv_pl_context *ctx);
};
