/*
 * Copyright (C) 2021 Niklas Haas
 *
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
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libplacebo/colorspace.h>
#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/frame_queue.h>

#include "config.h"
#include "common/common.h"
#include "common/stats.h"
#include "options/m_config.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/threads.h"
#include "stream/stream.h"
#include "video/fmt-conversion.h"
#include "video/mp_image.h"
#include "video/out/placebo/ra_pl.h"
#include "placebo/utils.h"
#include "gpu/context.h"
#include "gpu/hwdec.h"
#include "gpu/utils.h"
#include "gpu/video.h"
#include "gpu/video_shaders.h"
#include "sub/osd.h"
#include "gpu_next/context.h"
#include "gpu_next/core.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
#include <libplacebo/opengl.h>
#include "video/out/opengl/ra_gl.h"
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
#include <libplacebo/d3d11.h>
#include "video/out/d3d11/ra_d3d11.h"
#include "osdep/windows_utils.h"
#endif


struct priv {
    struct mp_log *log;
    struct mpv_global *global;
    struct stats_ctx *stats;
    struct ra_ctx *ra_ctx;
    struct gpu_ctx *context;
    struct ra_hwdec_ctx hwdec_ctx;

    struct gpu_next_core *core;

    pl_log pllog;
    pl_gpu gpu;
    pl_swapchain sw;

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    struct gpu_next_osd_state osd_state;

    bool is_interpolated;
    bool frame_pending;
    bool paused;

    struct m_config_cache *opts_cache;
    struct m_config_cache *next_opts_cache;
    struct gl_next_opts *next_opts;

    struct mp_image_params target_params;
};

static void update_render_options(struct vo *vo);

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align, int flags)
{
    struct priv *p = vo->priv;
    return gpu_next_core_get_image(p->core, imgfmt, w, h, stride_align, flags);
}

static void update_options(struct vo *vo)
{
    struct priv *p = vo->priv;
    bool changed = m_config_cache_update(p->opts_cache);
    changed = m_config_cache_update(p->next_opts_cache) || changed;
    if (changed)
        update_render_options(vo);

    gpu_next_core_update_options(p->core, p->opts_cache->opts, p->next_opts);
}

static bool set_colorspace_hint(struct priv *p, struct pl_color_space *hint)
{
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    enum pl_color_levels output_levels = gpu_next_core_output_levels(p->core);

    struct mp_image_params params = {
        .color = hint ? *hint : pl_color_space_srgb,
        .repr = {
            .sys = PL_COLOR_SYSTEM_RGB,
            .levels = output_levels ? output_levels : PL_COLOR_LEVELS_FULL,
            .alpha = p->ra_ctx->opts.want_alpha ? PL_ALPHA_INDEPENDENT : PL_ALPHA_NONE,
        },
    };

    if (sw->fns->set_color && sw->fns->set_color(sw, hint ? &params : NULL)) {
        if (hint) {
            *hint = params.color;
            return true;
        }
    }
    pl_swapchain_colorspace_hint(p->sw, hint);
    return false;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    pl_options pars = gpu_next_core_options(p->core);
    pl_gpu gpu = p->gpu;
    update_options(vo);

    struct pl_render_params params = pars->params;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    bool will_redraw = frame->display_synced && frame->num_vsyncs > 1;
    bool cache_frame = will_redraw || frame->still || p->paused;
    bool can_interpolate = opts->interpolation && frame->display_synced &&
                           !frame->still && frame->num_frames > 1 && !p->paused;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;
    params.skip_caching_single_frame = !cache_frame;
    params.preserve_mixing_cache = p->next_opts->inter_preserve && !frame->still;
    if (frame->still || p->paused)
        params.frame_mixer = NULL;

    if (frame->current && frame->current->params.vflip) {
        pl_matrix2x2 m = { .m = {{1, 0}, {0, -1}}, };
        pars->distort_params.transform.mat = m;
        params.distort_params = &pars->distort_params;
    } else {
        params.distort_params = NULL;
    }

    if (frame->current) {
        gpu_next_core_queue_check_refill(p->core, frame->current->pts,
                                         pts_offset,
                                         frame->ideal_frame_vsync_duration);
    }

    // Push all incoming frames into the frame queue
    for (int n = 0; n < frame->num_frames; n++) {
        int id = frame->frame_id + n;
        if (!gpu_next_core_queue_accept(p->core, id))
            continue;

        gpu_next_core_queue_push(p->core, frame->frames[n],
                                 can_interpolate ? frame->approx_duration : 0);
    }

    struct ra_swapchain *sw = p->ra_ctx->swapchain;

    // Swapchain touchpoint 1/2: read the backend-reported target colorspace.
    // The hint math itself is swapchain-free (gpu_next_core_target_hint).
    struct pl_color_space target_csp = {0};
    // TODO: Implement this for all backends
    if (sw->fns->target_csp)
        target_csp = sw->fns->target_csp(sw);
    float target_ref_luma =
        target_csp.transfer == PL_COLOR_TRC_UNKNOWN ? 0 : get_ref_luma(p);

    struct pl_color_space hint;
    bool target_hint, target_unknown;
    const struct pl_color_space *source =
        frame->current ? &frame->current->params.color : NULL;
    enum target_hint_action hint_action = gpu_next_core_target_hint(
        p->core, opts, p->next_opts->target_hint, p->next_opts->target_hint_mode,
        source, target_ref_luma, &target_csp, &hint, &target_hint,
        &target_unknown);

    // Swapchain touchpoint 2/2: push the computed hint and read back the
    // colorspace the swapchain negotiated (external_params/hint).
    bool external_params = false;
    if (hint_action == TARGET_HINT_SET) {
        external_params = set_colorspace_hint(p, &hint);
    } else if (hint_action == TARGET_HINT_CLEAR) {
        external_params = set_colorspace_hint(p, NULL);
    }

    struct pl_swapchain_frame swframe;
    bool should_draw = sw->fns->start_frame(sw, NULL); // for wayland logic
    if (!should_draw || !pl_swapchain_start_frame(p->sw, &swframe)) {
        if (frame->current) {
            // Advance the queue state to the current PTS to discard unused frames
            struct pl_queue_params qparams = *pl_queue_params(
                .pts = frame->current->pts + pts_offset,
                .radius = pl_frame_mix_radius(&params),
                .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
                .drift_compensation = 0,
            );
            pl_queue_update(gpu_next_core_queue(p->core), NULL, &qparams);
        }
        return VO_FALSE;
    }

    bool valid = false;
    p->is_interpolated = false;

    // Calculate target
    struct pl_frame target;
    pl_frame_from_swapchain(&target, &swframe);
    if (external_params)
        target.color = hint;
    bool strict_sw_params = target_hint && p->next_opts->target_hint_strict;
    int fallback_depth = sw->fns->color_depth ? sw->fns->color_depth(sw) : 0;
    gpu_next_core_apply_target_options(p->core, &target, hint.hdr.min_luma,
                                       strict_sw_params, target_ref_luma,
                                       &target_csp, fallback_depth);
    gpu_next_core_finalize_target_csp(p->core, &target, frame->current,
                                      &target_csp, target_unknown);
    stats_time_start(p->stats, "osd-update");
    gpu_next_core_update_overlays(p->core, p->osd_res,
                                 (frame->current && opts->blend_subs) ? OSD_DRAW_OSD_ONLY : 0,
                                 PL_OVERLAY_COORDS_DST_FRAME, &p->osd_state, &target, frame->current,
                                 frame->current ? frame->current->params.stereo3d : 0);
    stats_time_end(p->stats, "osd-update");
    gpu_next_core_apply_crop(&target, p->dst, swframe.fbo->params.w,
                             swframe.fbo->params.h);
    gpu_next_core_update_tm_viz(p->core, &target);

    struct pl_frame_mix mix = {0};
    if (frame->current) {
        // Update queue state
        struct pl_queue_params qparams = *pl_queue_params(
            .pts = frame->current->pts + pts_offset,
            .radius = pl_frame_mix_radius(&params),
            .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
            .interpolation_threshold = opts->interpolation_threshold,
            .drift_compensation = 0,
        );

        switch (gpu_next_core_queue_update(p->core, &mix, &qparams,
                                           frame->current->pts)) {
        case PL_QUEUE_ERR:
            MP_ERR(vo, "Failed updating frames!\n");
            goto done;
        case PL_QUEUE_EOF:
            abort(); // we never signal EOF
        case PL_QUEUE_MORE:
            // This is expected to happen semi-frequently near the start and
            // end of a file, so only log it at high verbosity and move on.
            if (!frame->still)
                MP_DBG(vo, "Render queue underrun.\n");
            break;
        case PL_QUEUE_OK:
            break;
        }

        // Apply source crop + blended-subtitle overlays to every frame in
        // the mix.
        gpu_next_core_update_frames(p->core, &mix, &target, p->src,
                                    vo->params->w, vo->params->h,
                                    frame->redraw);

        // Update dynamic hook parameters
        gpu_next_core_update_hooks_dynamic(p->core, frame->current);
    }

    // Render frame
    stats_time_start(p->stats, "render");
    bool render_ok = gpu_next_core_render_mix(p->core, &mix, &target, &params);
    stats_time_end(p->stats, "render");
    if (!render_ok) {
        MP_ERR(vo, "Failed rendering frame!\n");
        goto done;
    }

    mp_mutex_lock(&vo->params_mutex);
    p->target_params = (struct mp_image_params){
        .imgfmt_name = swframe.fbo->params.format
                        ? swframe.fbo->params.format->name : NULL,
        .w = mp_rect_w(p->dst),
        .h = mp_rect_h(p->dst),
        .color = target.color,
        .repr = target.repr,
        .rotate = target.rotation,
    };
    vo->target_params = &p->target_params;

    if (vo->params) {
        // Augment metadata with peak detection max_pq_y / avg_pq_y
        vo->has_peak_detect_values = gpu_next_core_get_hdr_metadata(p->core, &vo->params->color.hdr);
    }
    mp_mutex_unlock(&vo->params_mutex);

    p->is_interpolated = pts_offset != 0 && mix.num_frames > 1;
    valid = true;
    // fall through

done:
    if (!valid) // clear with purple to indicate error
        pl_tex_clear(gpu, swframe.fbo, (float[4]){ 0.5, 0.0, 1.0, 1.0 });

    pl_gpu_flush(gpu);
    p->frame_pending = true;
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;

    if (p->frame_pending) {
        if (!pl_swapchain_submit_frame(p->sw))
            MP_ERR(vo, "Failed presenting frame!\n");
        p->frame_pending = false;
    }

    sw->fns->swap_buffers(sw);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    if (sw->fns->get_vsync)
        sw->fns->get_vsync(sw, info);
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    if (ra_hwdec_get(&p->hwdec_ctx, format))
        return true;

    bool supported = gpu_next_core_format_supported(p->gpu, format, false);
    if (!supported)
        supported = gpu_next_core_format_supported(p->gpu, format, true);

    return supported;
}

static void resize(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    if (vo->dwidth && vo->dheight) {
        gpu_ctx_resize(p->context, vo->dwidth, vo->dheight);
        vo->want_redraw = true;
    }

    if (mp_rect_equals(&p->src, &src) &&
        mp_rect_equals(&p->dst, &dst) &&
        osd_res_equals(p->osd_res, osd))
        return;

    gpu_next_core_osd_changed(p->core);
    p->osd_res = osd;
    p->src = src;
    p->dst = dst;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    if (!p->ra_ctx->fns->reconfig(p->ra_ctx))
        return -1;

    resize(vo);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = NULL;
    mp_mutex_unlock(&vo->params_mutex);
    return 0;
}

// Returns whether the ICC profile was updated (even on failure)
static bool update_auto_profile(struct priv *p, int *events)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (!opts->icc_opts || !opts->icc_opts->profile_auto ||
        gpu_next_core_icc_has_manual_profile(p->core))
        return false;

    MP_VERBOSE(p, "Querying ICC profile...\n");
    bstr icc = {0};
    int r = p->ra_ctx->fns->control(p->ra_ctx, events, VOCTRL_GET_ICC_PROFILE, &icc);

    if (r != VO_NOTAVAIL) {
        if (r == VO_FALSE) {
            MP_WARN(p, "Could not retrieve an ICC profile.\n");
        } else if (r == VO_NOTIMPL) {
            MP_ERR(p, "icc-profile-auto not implemented on this platform.\n");
        }

        gpu_next_core_update_icc(p->core, icc);
        return true;
    }

    return false;
}

static void video_screenshot(struct vo *vo, struct voctrl_screenshot *args)
{
    struct priv *p = vo->priv;
    pl_options pars = gpu_next_core_options(p->core);
    pl_gpu gpu = p->gpu;
    pl_tex fbo = NULL;
    args->res = NULL;

    update_options(vo);
    struct pl_render_params params = pars->params;
    params.info_callback = NULL;
    params.skip_caching_single_frame = true;
    params.preserve_mixing_cache = false;
    params.frame_mixer = NULL;

    struct pl_peak_detect_params peak_params;
    if (params.peak_detect_params) {
        peak_params = *params.peak_detect_params;
        params.peak_detect_params = &peak_params;
        peak_params.allow_delayed = false;
    }

    // Retrieve the current frame from the frame queue
    struct pl_frame_mix mix;
    enum pl_queue_status status;
    struct pl_queue_params qparams = *pl_queue_params(
        .pts = gpu_next_core_queue_last_pts(p->core),
        .drift_compensation = 0,
    );
    status = pl_queue_update(gpu_next_core_queue(p->core), &mix, &qparams);
    mp_assert(status != PL_QUEUE_EOF);
    if (status == PL_QUEUE_ERR) {
        MP_ERR(vo, "Unknown error occurred while trying to take screenshot!\n");
        return;
    }
    if (!mix.num_frames) {
        MP_ERR(vo, "No frames available to take screenshot of, is a file loaded?\n");
        return;
    }

    // Passing an interpolation radius of 0 guarantees that the first frame in
    // the resulting mix is the correct frame for this PTS
    struct pl_frame image = *(struct pl_frame *) mix.frames[0];
    struct mp_image *mpi = image.user_data;
    struct mp_rect src = p->src, dst = p->dst;
    struct mp_osd_res osd = p->osd_res;
    if (!args->scaled) {
        int w, h;
        mp_image_params_get_dsize(&mpi->params, &w, &h);
        if (w < 1 || h < 1)
            return;

        int src_w = mpi->params.w;
        int src_h = mpi->params.h;
        src = (struct mp_rect) {0, 0, src_w, src_h};
        dst = (struct mp_rect) {0, 0, w, h};

        if (mp_image_crop_valid(&mpi->params))
            src = mpi->params.crop;

        if (mpi->params.rotate % 180 == 90) {
            MPSWAP(int, w, h);
            MPSWAP(int, src_w, src_h);
        }
        mp_rect_rotate(&src, src_w, src_h, mpi->params.rotate);
        mp_rect_rotate(&dst, w, h, mpi->params.rotate);

        osd = (struct mp_osd_res) {
            .display_par = 1.0,
            .w = mp_rect_w(dst),
            .h = mp_rect_h(dst),
        };
    }

    // Create target FBO, try high bit depth first
    int mpfmt;
    for (int depth = args->high_bit_depth ? 16 : 8; depth; depth -= 8) {
        if (depth == 16) {
            mpfmt = IMGFMT_RGBA64;
        } else {
            mpfmt = p->ra_ctx->opts.want_alpha ? IMGFMT_RGBA : IMGFMT_RGB0;
        }
        pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, depth, depth,
                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
        if (!fmt)
            continue;

        fbo = pl_tex_create(gpu, pl_tex_params(
            .w = osd.w,
            .h = osd.h,
            .format = fmt,
            .blit_dst = true,
            .renderable = true,
            .host_readable = true,
            .storable = fmt->caps & PL_FMT_CAP_STORABLE,
        ));
        if (fbo)
            break;
    }

    if (!fbo) {
        MP_ERR(vo, "Failed creating target FBO for screenshot!\n");
        return;
    }

    struct pl_frame target = {
        .repr = pl_color_repr_rgb,
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = 4,
            .component_mapping = {0, 1, 2, 3},
        },
    };

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (args->scaled) {
        // Apply target LUT, ICC profile and CSP override only in window mode
        struct ra_swapchain *sw = p->ra_ctx->swapchain;
        int fallback_depth = sw->fns->color_depth ? sw->fns->color_depth(sw) : 0;
        gpu_next_core_apply_target_options(p->core, &target, 0, false, 0,
                                           NULL, fallback_depth);
    } else if (args->native_csp) {
        target.color = image.color;
    } else {
        target.color = pl_color_space_srgb;
    }

    // sRGB reference display is pure 2.2 power function, see IEC 61966-2-1-1999.
    // Round-trip back to sRGB if the source is also sRGB. In other cases, we
    // use piecewise sRGB transfer function, as this is likely the be expected
    // for file encoding.
    if (opts->treat_srgb_as_power22 & 1 &&
        target.color.transfer == PL_COLOR_TRC_SRGB &&
        mpi->params.color.transfer == PL_COLOR_TRC_SRGB)
    {
        target.color.transfer = PL_COLOR_TRC_GAMMA22;
    }

    gpu_next_core_apply_crop(&image, src, mpi->params.w, mpi->params.h);
    gpu_next_core_apply_crop(&target, dst, fbo->params.w, fbo->params.h);
    gpu_next_core_update_tm_viz(p->core, &target);

    int osd_flags = 0;
    if (!args->subs)
        osd_flags |= OSD_DRAW_OSD_ONLY;
    if (!args->osd)
        osd_flags |= OSD_DRAW_SUB_ONLY;

    struct frame_priv *fp = mpi->priv;
    if (opts->blend_subs) {
        float w = pl_rect_w(opts->blend_subs == BLEND_SUBS_VIDEO ? image.crop : target.crop);
        float h = pl_rect_h(opts->blend_subs == BLEND_SUBS_VIDEO ? image.crop : target.crop);
        float rx = w / pl_rect_w(image.crop);
        float ry = h / pl_rect_h(image.crop);
        struct mp_osd_res res = {
            .w = w,
            .h = h,
            .ml = -image.crop.x0 * rx,
            .mr = (image.crop.x1 - vo->params->w) * rx,
            .mt = -image.crop.y0 * ry,
            .mb = (image.crop.y1 - vo->params->h) * ry,
            .display_par = 1.0,
        };
        enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
            ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
        gpu_next_core_update_overlays(p->core, res, osd_flags,
                                     rel, &fp->subs, &image, mpi,
                                     mpi->params.stereo3d);
    } else {
        // Disable overlays when blend_subs is disabled
        gpu_next_core_update_overlays(p->core, osd, osd_flags,
                                     PL_OVERLAY_COORDS_DST_FRAME,
                                     &p->osd_state, &target, mpi,
                                     mpi->params.stereo3d);
        image.num_overlays = 0;
    }

    if (!gpu_next_core_render_image(p->core, &image, &target, &params)) {
        MP_ERR(vo, "Failed rendering frame!\n");
        goto done;
    }

    args->res = mp_image_alloc(mpfmt, fbo->params.w, fbo->params.h);
    if (!args->res)
        goto done;

    args->res->params.color.primaries = target.color.primaries;
    args->res->params.color.transfer = target.color.transfer;
    args->res->params.repr.levels = target.repr.levels;
    args->res->params.color.hdr = target.color.hdr;
    if (args->scaled)
        args->res->params.p_w = args->res->params.p_h = 1;

    bool ok = pl_tex_download(gpu, pl_tex_transfer_params(
        .tex = fbo,
        .ptr = args->res->planes[0],
        .row_pitch = args->res->stride[0],
    ));

    if (!ok)
        TA_FREEP(&args->res);

    // fall through
done:
    pl_tex_destroy(gpu, &fbo);
}

static void update_ra_ctx_options(struct vo *vo, struct ra_ctx_opts *ctx_opts)
{
    struct priv *p = vo->priv;
    struct gl_video_opts *gl_opts = p->opts_cache->opts;
    bool border_alpha = (p->next_opts->border_background == BACKGROUND_COLOR &&
                         gl_opts->background_color.a != 255) ||
                         p->next_opts->border_background == BACKGROUND_NONE;
    ctx_opts->want_alpha = (gl_opts->background == BACKGROUND_COLOR &&
                            gl_opts->background_color.a != 255) ||
                            gl_opts->background == BACKGROUND_NONE ||
                            border_alpha;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_SET_PANSCAN:
        resize(vo);
        return VO_TRUE;
    case VOCTRL_PAUSE:
        if (p->is_interpolated)
            vo->want_redraw = true;
        p->paused = true;
        return VO_TRUE;
    case VOCTRL_RESUME:
        p->paused = false;
        return VO_TRUE;

    case VOCTRL_UPDATE_RENDER_OPTS: {
        update_ra_ctx_options(vo, &p->ra_ctx->opts);
        if (p->ra_ctx->fns->update_render_opts)
            p->ra_ctx->fns->update_render_opts(p->ra_ctx);
        vo->want_redraw = true;

        // Special case for --image-lut which requires a full reset.
        // update_options() now resolves the image LUT, updating its path
        // tracker in place, so capture the pre-update path/type to detect
        // a change.
        char *old_image_lut = talloc_strdup(NULL, p->next_opts->image_lut.path);
        int old_type = p->next_opts->image_lut.type;
        update_options(vo);
        struct user_lut image_lut = p->next_opts->image_lut;
        if (image_lut.opt && ((!old_image_lut && image_lut.opt) ||
            (old_image_lut && strcmp(old_image_lut, image_lut.opt)) ||
            (old_type != image_lut.type)))
        {
            gpu_next_core_queue_request_reset(p->core);
        }
        talloc_free(old_image_lut);

        // Also re-query the auto profile, in case `update_render_options`
        // unloaded a manually specified icc profile in favor of
        // icc-profile-auto
        int events = 0;
        update_auto_profile(p, &events);
        vo_event(vo, events);
        return VO_TRUE;
    }

    case VOCTRL_RESET:
        // Defer until the first new frame (unique ID) actually arrives
        gpu_next_core_queue_request_reset(p->core);
        return VO_TRUE;

    case VOCTRL_PERFORMANCE_DATA:
        gpu_next_core_get_perf_data(p->core, data);
        return true;

    case VOCTRL_SCREENSHOT:
        video_screenshot(vo, data);
        return true;

    case VOCTRL_EXTERNAL_RESIZE:
        reconfig(vo, NULL);
        return true;

    case VOCTRL_LOAD_HWDEC_API:
        ra_hwdec_ctx_load_fmt(&p->hwdec_ctx, vo->hwdec_devs, data);
        return true;
    }

    int events = 0;
    int r = p->ra_ctx->fns->control(p->ra_ctx, &events, request, data);
    if (events & VO_EVENT_ICC_PROFILE_CHANGED) {
        if (update_auto_profile(p, &events))
            vo->want_redraw = true;
    }
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return r;
}

static void wakeup(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wakeup)
        p->ra_ctx->fns->wakeup(p->ra_ctx);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wait_events) {
        p->ra_ctx->fns->wait_events(p->ra_ctx, until_time_ns);
    } else {
        vo_wait_default(vo, until_time_ns);
    }
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    // Drain any in-flight uploads.
    if (p->gpu)
        pl_gpu_finish(p->gpu);

    // The core owns the frame queue and destroys it first internally, so
    // this must run before the hwdec teardown below (queued frames unmap
    // hwdec textures on release).
    gpu_next_core_destroy(&p->core);
    for (int i = 0; i < MP_ARRAY_SIZE(p->osd_state.entries); i++)
        pl_tex_destroy(p->gpu, &p->osd_state.entries[i].tex);
    // sub_tex recycle pool is owned by the core and freed in
    // gpu_next_core_destroy() above.

    if (vo->hwdec_devs) {
        // hwdec mappers + timers are owned by core, destroyed above.
        ra_hwdec_ctx_uninit(&p->hwdec_ctx);
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
    }

    // image_lut/lut/target_lut pl_lut pointers are not owned here -- they
    // are borrowed from the core's LUT cache (freed by core_destroy above).
    // The ICC profile is owned and closed by gpu_next_core_destroy().

    p->ra_ctx = NULL;
    p->pllog = NULL;
    p->gpu = NULL;
    p->sw = NULL;
    gpu_ctx_destroy(&p->context);
}

static void load_hwdec_api(void *ctx, struct hwdec_imgfmt_request *params)
{
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, params);
}

// Thin front-end glue for gpu_next_core_frontend: the core's timer hooks
// are stats_time_start/_end bound to this VO's stats_ctx, and its hwdec
// lookup is ra_hwdec_get over this VO's ra_hwdec_ctx registry.
static void core_timer_start(void *ctx, const char *name)
{
    stats_time_start(ctx, name);
}

static void core_timer_end(void *ctx, const char *name)
{
    stats_time_end(ctx, name);
}

static struct ra_hwdec *core_hwdec_get(void *ctx, int imgfmt)
{
    return ra_hwdec_get(ctx, imgfmt);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->opts_cache = m_config_cache_alloc(p, vo->global, &gl_video_conf);
    p->next_opts_cache = m_config_cache_alloc(p, vo->global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;
    p->global = vo->global;
    p->log = vo->log;
    p->stats = stats_ctx_create(p, vo->global, "vo/gpu-next");

    struct gl_video_opts *gl_opts = p->opts_cache->opts;
    struct ra_ctx_opts *ctx_opts = mp_get_config_group(vo, vo->global, &ra_ctx_conf);
    update_ra_ctx_options(vo, ctx_opts);
    p->context = gpu_ctx_create(vo, ctx_opts);
    talloc_free(ctx_opts);
    if (!p->context)
        goto err_out;
    // For the time being
    p->ra_ctx = p->context->ra_ctx;
    p->pllog = p->context->pllog;
    p->gpu = p->context->gpu;
    p->sw = p->context->swapchain;
    p->hwdec_ctx = (struct ra_hwdec_ctx) {
        .log = p->log,
        .global = p->global,
        .ra_ctx = p->ra_ctx,
    };

    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_set_loader(vo->hwdec_devs, load_hwdec_api, vo);
    ra_hwdec_ctx_init(&p->hwdec_ctx, vo->hwdec_devs, gl_opts->hwdec_interop, false);

    p->core = gpu_next_core_create(p->gpu, p->log, p->pllog, p->global,
                                   gl_opts);
    gpu_next_core_set_frontend(p->core, &(struct gpu_next_core_frontend) {
        .opts = p->opts_cache->opts,
        .next_opts = p->next_opts,
        .ra = p->ra_ctx->ra,
        .timer_ctx = p->stats,
        .timer_start = core_timer_start,
        .timer_end = core_timer_end,
        .hwdec_get = core_hwdec_get,
        .hwdec_ctx = &p->hwdec_ctx,
    });
    gpu_next_core_set_osd(p->core, vo->osd);

    update_render_options(vo);
    return 0;

err_out:
    uninit(vo);
    return -1;
}

static void update_render_options(struct vo *vo)
{
    struct priv *p = vo->priv;
    gpu_next_core_update_render_options(p->core, p->opts_cache->opts,
                                       p->next_opts, p->paused);

    // Request as many frames as required from the decoder, depending on the
    // speed VPS/FPS ratio libplacebo may need more frames. Request frames up to
    // ratio of 1/2, but only if anti aliasing is enabled. Formula owned by
    // the core so the request stays in sync with the resolved frame mixer.
    vo_set_queue_params(vo, 0, MPMIN(VO_MAX_REQ_FRAMES,
                                     gpu_next_core_required_frames(p->core)));
}

const struct vo_driver video_out_gpu_next = {
    .description = "Video output based on libplacebo",
    .name = "gpu-next",
    .caps = VO_CAP_ROTATE90 |
            VO_CAP_FILM_GRAIN |
            VO_CAP_VFLIP |
            0x0,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
