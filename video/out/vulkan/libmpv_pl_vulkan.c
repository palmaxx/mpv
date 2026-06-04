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
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_vulkan pl_vulkan;
    pl_tex wrapped_fbo;

    // Stashed by acquire_target for the paired done_frame "hold": the layout
    // the host wants the image back in, and the semaphore to fire once it is.
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

    // Import the host's existing VkDevice (never create our own). libplacebo
    // takes no ownership: pl_vulkan_destroy tears down only libplacebo state
    // and leaves the host's VkInstance/VkDevice intact. The host is
    // responsible for having created the device with libplacebo's required
    // features + the extensions it lists here.
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
    return 0;
}

static int wrap_fbo(struct libmpv_pl_context *ctx, mpv_render_param *params,
                    pl_tex *out)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_tex *vt =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_TEX, NULL);
    // Validate the import tuple up front so a bad descriptor fails
    // deterministically here rather than as an opaque pl_vulkan_wrap failure
    // (or worse, undefined behaviour) later. The sync semaphores are checked
    // separately in acquire_target (they are not needed by get_target_size).
    if (!vt || !vt->image || vt->w <= 0 || vt->h <= 0 ||
        vt->format == VK_FORMAT_UNDEFINED || !vt->usage)
    {
        MP_ERR(ctx, "Invalid MPV_RENDER_PARAM_VULKAN_TEX: image=%p w=%d h=%d "
                    "format=%d usage=0x%x.\n",
               vt ? (void *)vt->image : NULL, vt ? vt->w : 0, vt ? vt->h : 0,
               vt ? (int)vt->format : 0, vt ? (unsigned)vt->usage : 0);
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // A wrapped image starts "held by the user"; the previous wrap is always
    // handed back to us by done_frame before the next wrap_fbo, so destroying
    // it here is the held-by-user case (pl_tex_destroy leaves the VkImage).
    if (p->wrapped_fbo)
        pl_tex_destroy(ctx->gpu, &p->wrapped_fbo);

    p->wrapped_fbo = pl_vulkan_wrap(ctx->gpu, pl_vulkan_wrap_params(
        .image  = (VkImage)vt->image,
        .width  = vt->w,
        .height = vt->h,
        .format = vt->format,
        .usage  = vt->usage,
    ));
    if (!p->wrapped_fbo) {
        MP_ERR(ctx, "Failed to wrap host VkImage (%dx%d, format %d) as pl_tex.\n",
               vt->w, vt->h, (int)vt->format);
        return MPV_ERROR_UNSUPPORTED;
    }
    *out = p->wrapped_fbo;
    return 0;
}

static int acquire_target(struct libmpv_pl_context *ctx,
                          mpv_render_param *params, pl_tex target)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_tex *vt =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_TEX, NULL);
    // release_sem is required: it is the only way the host learns mpv is done
    // with the image. Reject a missing one before handing the image to
    // libplacebo, so we never enter the released state without a way out of it.
    if (!vt || !vt->release_sem) {
        MP_ERR(ctx, "MPV_RENDER_PARAM_VULKAN_TEX is missing the required "
                    "release_sem.\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // "Release" hands control to libplacebo: it waits on acquire_sem (if any),
    // treating the image as currently in current_layout. qf = IGNORED skips a
    // queue-family transition (the wrapped image is concurrent across
    // libplacebo's queues, and the host shares the same imported device).
    pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
        .tex       = target,
        .layout    = vt->current_layout,
        .qf        = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = { (VkSemaphore)vt->acquire_sem, vt->acquire_value },
    ));

    p->released = true;
    p->final_layout = vt->final_layout;
    p->release_sem = (pl_vulkan_sem){ (VkSemaphore)vt->release_sem,
                                      vt->release_value };
    return 0;
}

static int done_frame(struct libmpv_pl_context *ctx, bool display_synced)
{
    struct priv *p = ctx->priv;
    if (!p->released)
        return 0;

    // "Hold" hands control back to the host: libplacebo transitions the image
    // to final_layout and fires release_sem when it is ready for presentation.
    // On failure libplacebo did NOT regain/return ownership, so keep released
    // set: the host gets an error from mpv_render_context_render() and the
    // teardown path (or a host-driven recovery) re-holds before destroying.
    if (!pl_vulkan_hold_ex(ctx->gpu, pl_vulkan_hold_params(
            .tex       = p->wrapped_fbo,
            .layout    = p->final_layout,
            .qf        = VK_QUEUE_FAMILY_IGNORED,
            .semaphore = p->release_sem,
        )))
    {
        MP_ERR(ctx, "pl_vulkan_hold_ex failed to hand the target back to the "
                    "host; release_sem was not signalled.\n");
        return MPV_ERROR_GENERIC;
    }
    p->released = false;
    return 0;
}

static void destroy(struct libmpv_pl_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    // If a render aborted between acquire and done_frame (or done_frame's hold
    // failed), hand the image back so libplacebo isn't left owning a wrap we're
    // about to destroy. Best-effort at teardown: log a failure but proceed.
    if (p->released && p->wrapped_fbo) {
        if (!pl_vulkan_hold_ex(ctx->gpu, pl_vulkan_hold_params(
                .tex       = p->wrapped_fbo,
                .layout    = p->final_layout,
                .qf        = VK_QUEUE_FAMILY_IGNORED,
                .semaphore = p->release_sem,
            )))
        {
            MP_WARN(ctx, "pl_vulkan_hold_ex failed during teardown; destroying "
                         "a still-held wrap.\n");
        }
        p->released = false;
    }
    if (p->wrapped_fbo)
        pl_tex_destroy(ctx->gpu, &p->wrapped_fbo);
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
