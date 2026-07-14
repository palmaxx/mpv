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
#include "ta/ta_talloc.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_vulkan pl_vulkan;

    // cur_fbo is borrowed. orphaned_fbo owns a wrapper after a failed hold.
    pl_tex cur_fbo;
    pl_tex orphaned_fbo;

    bool released;
    VkImageLayout final_layout;
    pl_vulkan_sem release_sem;
};

static int init(struct libmpv_pl_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_vulkan_init_params *ip =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!ip || !ip->instance || !ip->phys_device || !ip->device)
        return MPV_ERROR_INVALID_PARAMETER;

    ctx->pllog = mppl_log_create(p, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_GENERIC;

    // libplacebo does not take ownership of the host Vulkan objects.
    struct pl_vulkan_import_params ipar = *pl_vulkan_import_params(
        .instance       = (VkInstance)ip->instance,
        .get_proc_addr  = ip->get_proc_addr,
        .phys_device    = (VkPhysicalDevice)ip->phys_device,
        .device         = (VkDevice)ip->device,
        .extensions     = ip->extensions,
        .num_extensions = ip->num_extensions,
        .features       = ip->features,
        .queue_graphics = { ip->queue_graphics_index, ip->queue_graphics_count },
        .queue_compute  = { ip->queue_compute_index,  ip->queue_compute_count },
        .queue_transfer = { ip->queue_transfer_index, ip->queue_transfer_count },
    );
    p->pl_vulkan = pl_vulkan_import(ctx->pllog, &ipar);
    if (!p->pl_vulkan) {
        MP_FATAL(ctx, "libplacebo Vulkan device import failed (check that the "
                      "host enabled pl_vulkan_required_features).\n");
        return MPV_ERROR_UNSUPPORTED;
    }
    ctx->gpu = p->pl_vulkan->gpu;
    ctx->ra_ctx = talloc_zero(p, struct ra_ctx);
    ctx->ra_ctx->log = ctx->log;
    ctx->ra_ctx->global = ctx->global;
    ctx->ra = ctx->ra_ctx->ra = ra_create_pl(ctx->gpu, ctx->log);
    return 0;
}

// Reclaim a released wrapper before destroying it.
static bool ensure_held(struct priv *p, pl_gpu gpu, pl_tex tex)
{
    if (!p->released)
        return true;
    if (!pl_vulkan_hold_ex(gpu, pl_vulkan_hold_params(
            .tex       = tex,
            .layout    = p->final_layout,
            .qf        = VK_QUEUE_FAMILY_IGNORED,
            .semaphore = p->release_sem,
        )))
        return false;
    p->released = false;
    return true;
}

static int wrap_fbo(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_tex *vt =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_TEX, NULL);
    if (!vt || !vt->image || vt->w <= 0 || vt->h <= 0 ||
        vt->format == VK_FORMAT_UNDEFINED || !vt->usage)
    {
        MP_ERR(ctx, "Invalid MPV_RENDER_PARAM_VULKAN_TEX: image=%p w=%d h=%d "
                    "format=%d usage=0x%x.\n",
               vt ? (void *)vt->image : NULL, vt ? vt->w : 0, vt ? vt->h : 0,
               vt ? (int)vt->format : 0, vt ? (unsigned)vt->usage : 0);
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Retry recovery from a previous failed handoff before accepting a target.
    if (p->orphaned_fbo) {
        if (!ensure_held(p, ctx->gpu, p->orphaned_fbo)) {
            MP_ERR(ctx, "Cannot reclaim the previously released Vulkan target "
                        "(pl_vulkan_hold_ex still failing); refusing to render "
                        "until it can be handed back.\n");
            return MPV_ERROR_GENERIC;
        }
        pl_tex_destroy(ctx->gpu, &p->orphaned_fbo);
    }

    pl_tex wrap = pl_vulkan_wrap(ctx->gpu, pl_vulkan_wrap_params(
        .image  = (VkImage)vt->image,
        .width  = vt->w,
        .height = vt->h,
        .format = vt->format,
        .usage  = vt->usage,
    ));
    if (!wrap) {
        MP_ERR(ctx, "Failed to wrap host VkImage (%dx%d, format %d) as pl_tex.\n",
               vt->w, vt->h, (int)vt->format);
        return MPV_ERROR_UNSUPPORTED;
    }
    *out = wrap;
    return 0;
}

static int acquire_target(struct libmpv_pl_context *ctx,
                          mpv_render_param *params, pl_tex target)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_tex *vt =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_TEX, NULL);
    if (!vt || !vt->release_sem) {
        MP_ERR(ctx, "MPV_RENDER_PARAM_VULKAN_TEX is missing the required "
                    "release_sem.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Queue-family ownership remains with the host's sharing configuration.
    pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
        .tex       = target,
        .layout    = vt->current_layout,
        .qf        = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { (VkSemaphore)vt->acquire_sem, vt->acquire_value },
    ));

    p->cur_fbo = target;   // non-owning; consumed by done_frame's hold
    p->released = true;
    p->final_layout = vt->final_layout;
    p->release_sem = (pl_vulkan_sem){ (VkSemaphore)vt->release_sem,
                                      vt->release_value };
    return 0;
}

static int done_frame(struct libmpv_pl_context *ctx, bool display_synced)
{
    struct priv *p = ctx->priv;

    // A failed hold leaves the wrapper released to libplacebo. Keep it for a
    // later recovery attempt instead of destroying it.
    if (!ensure_held(p, ctx->gpu, p->cur_fbo)) {
        MP_ERR(ctx, "pl_vulkan_hold_ex failed to hand the target back to the "
                    "host; release_sem was not signalled.\n");
        p->orphaned_fbo = p->cur_fbo;
        p->cur_fbo = NULL;
        return MPV_ERROR_GENERIC;
    }
    p->cur_fbo = NULL;
    return 0;
}

static void destroy(struct libmpv_pl_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    // Destroy only wrappers reclaimed from libplacebo. Leaking a failed wrapper
    // is safer than invalidating the host image.
    if (p->orphaned_fbo) {
        if (ensure_held(p, ctx->gpu, p->orphaned_fbo)) {
            pl_tex_destroy(ctx->gpu, &p->orphaned_fbo);
        } else {
            MP_WARN(ctx, "pl_vulkan_hold_ex failed at teardown; leaking the "
                         "pl_tex wrapper to avoid corrupting the host image.\n");
            p->orphaned_fbo = NULL; // drop our ref without destroying
        }
    }
    if (ctx->ra_ctx && ctx->ra_ctx->ra) {
        ctx->ra_ctx->ra->fns->destroy(ctx->ra_ctx->ra);
        ctx->ra_ctx->ra = NULL;
    }
    if (p->pl_vulkan)
        pl_vulkan_destroy(&p->pl_vulkan);
    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);
}

const struct libmpv_pl_context_fns libmpv_pl_context_vulkan = {
    .api_name = MPV_RENDER_API_TYPE_PL_VULKAN,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .acquire_target = acquire_target,
    .done_frame = done_frame,
    .destroy = destroy,
};
