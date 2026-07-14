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

struct ra;
struct ra_ctx;

// API-specific libplacebo context and target wrapper.
struct libmpv_pl_context {
    struct mpv_global *global;
    struct mp_log *log;
    const struct libmpv_pl_context_fns *fns;

    pl_log pllog;
    pl_gpu gpu;

    // Optional RA context used by hardware-decoding interop.
    struct ra_ctx *ra_ctx;
    struct ra *ra;

    void *priv;
};

struct libmpv_pl_context_fns {
    const char *api_name;

    // Populate pllog, gpu, and any hardware-decoding RA context.
    int (*init)(struct libmpv_pl_context *ctx, mpv_render_param *params);

    // Create a per-render target wrapper. The caller destroys it after a
    // successful done_frame; a failed done_frame transfers ownership back to
    // the backend.
    int (*wrap_fbo)(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out);

    // Optional ownership transfer from the host to libplacebo.
    int (*acquire_target)(struct libmpv_pl_context *ctx,
                          mpv_render_param *params, pl_tex target);

    // Optional ownership transfer back to the host. On failure, the backend
    // retains the wrapper for recovery.
    int (*done_frame)(struct libmpv_pl_context *ctx, bool display_synced);

    void (*destroy)(struct libmpv_pl_context *ctx);
};
