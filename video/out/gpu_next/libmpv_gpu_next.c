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
#include "common/common.h"
#include "common/msg.h"
#include "core.h"
#include "libmpv_gpu_next.h"
#include "misc/bstr.h"
#include "mpv/render.h"
#include "mpv/render_gl.h"
#include "options/m_config.h"
#include "ta/ta_talloc.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/video.h"
#include "video/out/libmpv.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
extern const struct libmpv_pl_context_fns libmpv_pl_context_gl;
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
extern const struct libmpv_pl_context_fns libmpv_pl_context_d3d11;
#endif
#if HAVE_VULKAN && defined(PL_HAVE_VULKAN)
extern const struct libmpv_pl_context_fns libmpv_pl_context_vulkan;
#endif

static const struct libmpv_pl_context_fns *context_backends[] = {
#if HAVE_GL && defined(PL_HAVE_OPENGL)
    &libmpv_pl_context_gl,
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    &libmpv_pl_context_d3d11,
#endif
#if HAVE_VULKAN && defined(PL_HAVE_VULKAN)
    &libmpv_pl_context_vulkan,
#endif
    NULL,
};

// Native resources forwarded to the RA hardware-decoding interop.
struct native_resource_entry {
    const char *name;
    size_t size;
};

static const struct native_resource_entry native_resource_map[] = {
    [MPV_RENDER_PARAM_X11_DISPLAY] = {
        .name = "x11",
        .size = 0,
    },
    [MPV_RENDER_PARAM_WL_DISPLAY] = {
        .name = "wl",
        .size = 0,
    },
    [MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE] = {
        .name = "drm_draw_surface_size",
        .size = sizeof(mpv_opengl_drm_draw_surface_size),
    },
    [MPV_RENDER_PARAM_DRM_DISPLAY_V2] = {
        .name = "drm_params_v2",
        .size = sizeof(mpv_opengl_drm_params_v2),
    },
};

struct priv {
    struct libmpv_pl_context *context;
    struct gpu_next_core *core;

    // Option caches must exist before the API-specific GPU context.
    struct m_config_cache *opts_cache;
    struct m_config_cache *next_opts_cache;
    struct gl_next_opts *next_opts;

    struct gpu_next_osd_state osd_state;

    struct ra_hwdec_ctx hwdec_ctx;

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
};

static struct ra_hwdec *core_hwdec_get(void *ctx, int imgfmt)
{
    return ra_hwdec_get(ctx, imgfmt);
}

static void update_options(struct priv *p)
{
    bool changed = m_config_cache_update(p->opts_cache);
    changed = m_config_cache_update(p->next_opts_cache) || changed;
    if (changed) {
        gpu_next_core_update_render_options(p->core, p->opts_cache->opts,
                                           p->next_opts, false);
    }
    gpu_next_core_update_options(p->core, p->opts_cache->opts, p->next_opts);
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    p->opts_cache = m_config_cache_alloc(p, ctx->global, &gl_video_conf);
    p->next_opts_cache = m_config_cache_alloc(p, ctx->global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;

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
                                   p->context->pllog, ctx->global,
                                   p->opts_cache->opts);
    if (!p->core)
        return MPV_ERROR_GENERIC;

    // Backends without an RA context use software decoding.
    if (p->context->ra_ctx) {
        ctx->hwdec_devs = hwdec_devices_create();

        // GL hardware-decoding interops resolve these resources by name.
        for (int n = 0; params && params[n].type; n++) {
            if (params[n].type > 0 &&
                params[n].type < MP_ARRAY_SIZE(native_resource_map) &&
                native_resource_map[params[n].type].name)
            {
                const struct native_resource_entry *entry =
                    &native_resource_map[params[n].type];
                void *data = params[n].data;
                if (entry->size)
                    data = talloc_memdup(p, data, entry->size);
                ra_add_native_resource(p->context->ra_ctx->ra, entry->name, data);
            }
        }

        p->hwdec_ctx = (struct ra_hwdec_ctx){
            .log = ctx->log,
            .global = ctx->global,
            .ra_ctx = p->context->ra_ctx,
        };
        const struct gl_video_opts *gl_opts = p->opts_cache->opts;
        ra_hwdec_ctx_init(&p->hwdec_ctx, ctx->hwdec_devs,
                          gl_opts->hwdec_interop, true);
    }

    gpu_next_core_set_frontend(p->core, &(struct gpu_next_core_frontend) {
        .opts = p->opts_cache->opts,
        .next_opts = p->next_opts,
        .ra = p->context->ra,
        .hwdec_get = core_hwdec_get,
        .hwdec_ctx = &p->hwdec_ctx,
    });

    gpu_next_core_update_render_options(p->core, p->opts_cache->opts,
                                        p->next_opts, false);

    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    pl_gpu gpu = p->context->gpu;

    if (ra_hwdec_get(&p->hwdec_ctx, imgfmt))
        return true;

    return gpu_next_core_format_supported(gpu, imgfmt, false) ||
           gpu_next_core_format_supported(gpu, imgfmt, true);
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct priv *p = ctx->priv;

    switch (param.type) {
    case MPV_RENDER_PARAM_ICC_PROFILE: {
        mpv_byte_array *data = param.data;
        gpu_next_core_update_icc(p->core,
                                 bstrdup(NULL, (bstr){data->data, data->size}));
        return 0;
    }
    case MPV_RENDER_PARAM_AMBIENT_LIGHT:
        return MPV_ERROR_NOT_IMPLEMENTED;
    default:
        return MPV_ERROR_NOT_IMPLEMENTED;
    }
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    // Source parameters are resolved per frame.
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;

    gpu_next_core_queue_request_reset(p->core);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;

    gpu_next_core_set_osd(p->core, vo ? vo->osd : NULL);
    if (!vo)
        return;

    bool changed = m_config_cache_update(p->opts_cache);
    changed = m_config_cache_update(p->next_opts_cache) || changed;
    if (changed) {
        gpu_next_core_update_render_options(p->core, p->opts_cache->opts,
                                           p->next_opts, false);
    }
    vo_set_queue_params(vo, 0, MPMIN(VO_MAX_REQ_FRAMES,
                                     gpu_next_core_required_frames(p->core)));
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;

    p->src = *src;
    p->dst = *dst;
    p->osd_res = *osd;
    gpu_next_core_osd_changed(p->core);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    struct priv *p = ctx->priv;

    pl_tex fbo;
    int err = p->context->fns->wrap_fbo(p->context, params, &fbo);
    if (err < 0)
        return err;
    *out_w = fbo->params.w;
    *out_h = fbo->params.h;
    // The wrapper was never released to libplacebo.
    pl_tex_destroy(p->context->gpu, &fbo);
    return 0;
}

// Keep the public ABI independent from libplacebo's enum ordering.
static enum pl_color_primaries map_primaries(mpv_color_primaries prim)
{
    switch (prim) {
    case MPV_COLOR_PRIMARIES_BT_601_525: return PL_COLOR_PRIM_BT_601_525;
    case MPV_COLOR_PRIMARIES_BT_601_625: return PL_COLOR_PRIM_BT_601_625;
    case MPV_COLOR_PRIMARIES_BT_709:     return PL_COLOR_PRIM_BT_709;
    case MPV_COLOR_PRIMARIES_BT_470M:    return PL_COLOR_PRIM_BT_470M;
    case MPV_COLOR_PRIMARIES_EBU_3213:   return PL_COLOR_PRIM_EBU_3213;
    case MPV_COLOR_PRIMARIES_BT_2020:    return PL_COLOR_PRIM_BT_2020;
    case MPV_COLOR_PRIMARIES_APPLE:      return PL_COLOR_PRIM_APPLE;
    case MPV_COLOR_PRIMARIES_ADOBE:      return PL_COLOR_PRIM_ADOBE;
    case MPV_COLOR_PRIMARIES_PRO_PHOTO:  return PL_COLOR_PRIM_PRO_PHOTO;
    case MPV_COLOR_PRIMARIES_CIE_1931:   return PL_COLOR_PRIM_CIE_1931;
    case MPV_COLOR_PRIMARIES_DCI_P3:     return PL_COLOR_PRIM_DCI_P3;
    case MPV_COLOR_PRIMARIES_DISPLAY_P3: return PL_COLOR_PRIM_DISPLAY_P3;
    case MPV_COLOR_PRIMARIES_V_GAMUT:    return PL_COLOR_PRIM_V_GAMUT;
    case MPV_COLOR_PRIMARIES_S_GAMUT:    return PL_COLOR_PRIM_S_GAMUT;
    case MPV_COLOR_PRIMARIES_FILM_C:     return PL_COLOR_PRIM_FILM_C;
    case MPV_COLOR_PRIMARIES_ACES_AP0:   return PL_COLOR_PRIM_ACES_AP0;
    case MPV_COLOR_PRIMARIES_ACES_AP1:   return PL_COLOR_PRIM_ACES_AP1;
    case MPV_COLOR_PRIMARIES_AUTO:       break;
    }
    return PL_COLOR_PRIM_UNKNOWN;
}

static enum pl_color_transfer map_transfer(mpv_color_transfer trc)
{
    switch (trc) {
    case MPV_COLOR_TRANSFER_BT_1886:   return PL_COLOR_TRC_BT_1886;
    case MPV_COLOR_TRANSFER_SRGB:      return PL_COLOR_TRC_SRGB;
    case MPV_COLOR_TRANSFER_LINEAR:    return PL_COLOR_TRC_LINEAR;
    case MPV_COLOR_TRANSFER_GAMMA18:   return PL_COLOR_TRC_GAMMA18;
    case MPV_COLOR_TRANSFER_GAMMA20:   return PL_COLOR_TRC_GAMMA20;
    case MPV_COLOR_TRANSFER_GAMMA22:   return PL_COLOR_TRC_GAMMA22;
    case MPV_COLOR_TRANSFER_GAMMA24:   return PL_COLOR_TRC_GAMMA24;
    case MPV_COLOR_TRANSFER_GAMMA26:   return PL_COLOR_TRC_GAMMA26;
    case MPV_COLOR_TRANSFER_GAMMA28:   return PL_COLOR_TRC_GAMMA28;
    case MPV_COLOR_TRANSFER_PRO_PHOTO: return PL_COLOR_TRC_PRO_PHOTO;
    case MPV_COLOR_TRANSFER_ST428:     return PL_COLOR_TRC_ST428;
    case MPV_COLOR_TRANSFER_PQ:        return PL_COLOR_TRC_PQ;
    case MPV_COLOR_TRANSFER_HLG:       return PL_COLOR_TRC_HLG;
    case MPV_COLOR_TRANSFER_V_LOG:     return PL_COLOR_TRC_V_LOG;
    case MPV_COLOR_TRANSFER_S_LOG1:    return PL_COLOR_TRC_S_LOG1;
    case MPV_COLOR_TRANSFER_S_LOG2:    return PL_COLOR_TRC_S_LOG2;
    case MPV_COLOR_TRANSFER_AUTO:      break;
    }
    return PL_COLOR_TRC_UNKNOWN;
}

// An all-zero target colorspace is equivalent to omitting the parameter.
static bool target_colorspace_present(
    const mpv_render_param_target_colorspace *cs)
{
    return cs->primaries != MPV_COLOR_PRIMARIES_AUTO ||
           cs->transfer  != MPV_COLOR_TRANSFER_AUTO ||
           cs->hdr.max_luma || cs->hdr.min_luma ||
           cs->hdr.max_cll  || cs->hdr.max_fall ||
           cs->hdr.prim_red.x    || cs->hdr.prim_red.y ||
           cs->hdr.prim_green.x  || cs->hdr.prim_green.y ||
           cs->hdr.prim_blue.x   || cs->hdr.prim_blue.y ||
           cs->hdr.white_point.x || cs->hdr.white_point.y;
}

// Zero metadata remains unknown to libplacebo.
static struct pl_color_space map_target_colorspace(
    const mpv_render_param_target_colorspace *cs)
{
    struct pl_color_space out = {
        .primaries = map_primaries(cs->primaries),
        .transfer  = map_transfer(cs->transfer),
        .hdr = {
            .min_luma = cs->hdr.min_luma,
            .max_luma = cs->hdr.max_luma,
            .max_cll  = cs->hdr.max_cll,
            .max_fall = cs->hdr.max_fall,
        },
    };
    if (cs->hdr.prim_red.x || cs->hdr.prim_green.x || cs->hdr.prim_blue.x ||
        cs->hdr.white_point.x)
    {
        out.hdr.prim = (struct pl_raw_primaries){
            .red   = { cs->hdr.prim_red.x,    cs->hdr.prim_red.y },
            .green = { cs->hdr.prim_green.x,  cs->hdr.prim_green.y },
            .blue  = { cs->hdr.prim_blue.x,   cs->hdr.prim_blue.y },
            .white = { cs->hdr.white_point.x, cs->hdr.white_point.y },
        };
    }
    return out;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    pl_options pars = gpu_next_core_options(p->core);
    pl_gpu gpu = p->context->gpu;

    pl_tex fbo;
    int err = p->context->fns->wrap_fbo(p->context, params, &fbo);
    if (err < 0)
        return err;

    // Externally synchronized backends transfer ownership before rendering.
    if (p->context->fns->acquire_target) {
        err = p->context->fns->acquire_target(p->context, params, fbo);
        if (err < 0) {
            pl_tex_destroy(gpu, &fbo);
            return err;
        }
    }

    int depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH, int, 0);
    bool flip = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_FLIP_Y, int, 0);
    mpv_render_param_target_colorspace *target_cs =
        get_mpv_render_param(params, MPV_RENDER_PARAM_TARGET_COLORSPACE, NULL);
    bool target_known = target_cs && target_colorspace_present(target_cs);

    update_options(p);

    struct pl_render_params rparams = pars->params;
    const struct gl_video_opts *opts = p->opts_cache->opts;
    bool will_redraw = frame->display_synced && frame->num_vsyncs > 1;
    bool cache_frame = will_redraw || frame->still;
    bool can_interpolate = opts->interpolation && frame->display_synced &&
                           !frame->still && frame->num_frames > 1;
    double pts_offset = can_interpolate ? frame->ideal_frame_vsync : 0;
    rparams.skip_caching_single_frame = !cache_frame;
    rparams.preserve_mixing_cache = p->next_opts->inter_preserve && !frame->still;
    if (frame->still)
        rparams.frame_mixer = NULL;

    if (frame->current && frame->current->params.vflip) {
        pl_matrix2x2 m = { .m = {{1, 0}, {0, -1}}, };
        pars->distort_params.transform.mat = m;
        rparams.distort_params = &pars->distort_params;
    } else {
        rparams.distort_params = NULL;
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

    // Render-API targets default to sRGB unless the host supplies a colorspace.
    struct pl_color_space host_csp =
        target_known ? map_target_colorspace(target_cs) : pl_color_space_srgb;
    struct pl_frame target = {
        .num_planes = 1,
        .planes[0] = {
            .texture = fbo,
            .components = fbo->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        },
        .repr = pl_color_repr_rgb,
        .color = host_csp,
    };

    struct pl_color_space target_csp = target_known ? host_csp
                                                    : (struct pl_color_space){0};
    gpu_next_core_apply_target_options(p->core, &target, 0, target_known, 0,
                                       &target_csp, depth);
    gpu_next_core_finalize_target_csp(p->core, &target, frame->current,
                                      &target_csp, !target_known);

    gpu_next_core_update_overlays(p->core, p->osd_res,
                                  (frame->current && opts->blend_subs) ? OSD_DRAW_OSD_ONLY : 0,
                                  PL_OVERLAY_COORDS_DST_FRAME, &p->osd_state,
                                  &target, frame->current,
                                  frame->current ? frame->current->params.stereo3d : 0, 0);

    gpu_next_core_apply_crop(&target, p->dst, fbo->params.w, fbo->params.h);
    if (flip)
        MPSWAP(float, target.crop.y0, target.crop.y1);
    gpu_next_core_update_tm_viz(p->core, &target);

    bool valid = false;
    struct pl_frame_mix mix = {0};
    if (frame->current) {
        struct pl_queue_params qparams = *pl_queue_params(
            .pts = frame->current->pts + pts_offset,
            .radius = pl_frame_mix_radius(&rparams),
            .vsync_duration = can_interpolate ? frame->ideal_frame_vsync_duration : 0,
            .interpolation_threshold = opts->interpolation_threshold,
            .drift_compensation = 0,
        );

        switch (gpu_next_core_queue_update(p->core, &mix, &qparams,
                                           frame->current->pts)) {
        case PL_QUEUE_ERR:
            MP_ERR(ctx, "Failed updating frames!\n");
            goto done;
        case PL_QUEUE_EOF:
            abort(); // we never signal EOF
        case PL_QUEUE_MORE:
        case PL_QUEUE_OK:
            break;
        }

        gpu_next_core_update_frames(p->core, &mix, &target, p->src,
                                    frame->current->params.w,
                                    frame->current->params.h, frame->redraw, 0);
        gpu_next_core_update_hooks_dynamic(p->core, frame->current);
    }

    if (!gpu_next_core_render_mix(p->core, &mix, &target, &rparams)) {
        MP_ERR(ctx, "Failed rendering frame!\n");
        goto done;
    }
    valid = true;

done:
    // Some wrapped formats cannot be cleared.
    if (!valid && fbo->params.blit_dst)
        pl_tex_clear(gpu, fbo, (float[4]){ 0.5, 0.0, 1.0, 1.0 });

    pl_gpu_flush(gpu);
    int present_err = p->context->fns->done_frame
        ? p->context->fns->done_frame(p->context, frame->display_synced)
        : 0;

    // A failed done_frame transfers wrapper ownership back to the backend.
    if (present_err >= 0)
        pl_tex_destroy(gpu, &fbo);
    return present_err < 0 ? present_err : 0;
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    struct priv *p = ctx->priv;
    return gpu_next_core_get_image(p->core, imgfmt, w, h, stride_align, flags);
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    struct priv *p = ctx->priv;
    update_options(p);
    gpu_next_core_screenshot(p->core, args, p->src, p->dst, p->osd_res,
                             0, false, &p->osd_state);
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    struct priv *p = ctx->priv;
    gpu_next_core_get_perf_data(p->core, out);
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->context && p->context->gpu)
        pl_gpu_finish(p->context->gpu);

    if (p->core)
        gpu_next_core_destroy(&p->core);
    if (p->context && p->context->gpu) {
        for (int i = 0; i < MP_ARRAY_SIZE(p->osd_state.entries); i++)
            pl_tex_destroy(p->context->gpu, &p->osd_state.entries[i].tex);
    }

    ra_hwdec_ctx_uninit(&p->hwdec_ctx);
    if (ctx->hwdec_devs) {
        hwdec_devices_destroy(ctx->hwdec_devs);
        ctx->hwdec_devs = NULL;
    }

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
    .get_image = get_image,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .destroy = destroy,
};
