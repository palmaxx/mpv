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

#include "core.h"

#include <math.h>

#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/libav.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/path.h"
#include "osdep/threads.h"
#include "stream/stream.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/utils.h"
#include "video/out/gpu/video.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/vo.h"

#if HAVE_GL && defined(PL_HAVE_OPENGL)
#include <libplacebo/opengl.h>
#include "video/out/opengl/ra_gl.h"
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
#include <libplacebo/d3d11.h>
#include "video/out/d3d11/ra_d3d11.h"
#include "osdep/windows_utils.h"
#endif

struct scaler_params {
    struct pl_filter_config config;
};

struct user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct user_lut_cache_entry {
    char *path;
    struct pl_custom_lut *lut;
};

struct frame_info {
    int count;
    struct pl_dispatch_info info[VO_PASS_PERF_MAX];
};

struct gpu_next_core {
    pl_gpu gpu;
    pl_log pllog;
    struct mp_log *log;

    pl_options pars;
    pl_renderer rr;
    pl_queue queue;

    // pl_queue lifecycle state. want_reset/flush_cache are deferred
    // requests honoured at the next gpu_next_core_queue_accept(); last_id
    // deduplicates already-pushed source frames; last_pts is the last
    // virtual PTS handed to pl_queue_update (read back by the screenshot
    // path).
    uint64_t last_id;
    double last_pts;
    bool want_reset;
    bool flush_cache;

    // Hwdec interop (mapper created lazily on the first hwdec frame and
    // refreshed when params change; destroyed in gpu_next_core_destroy
    // AFTER the queue so in-flight frames' release callbacks still see a
    // valid mapper).
    struct ra_hwdec_mapper *hwdec_mapper;
    struct timer_pool *hwdec_timer;
    struct mp_pass_perf hwdec_perf;

    // SW-upload perf: timer created lazily on the first software frame
    // (gpu_next_core_upload_sw_planes), destroyed in gpu_next_core_destroy.
    struct timer_pool *sw_upload_timer;
    struct mp_pass_perf sw_upload_perf;

    // Render-pass perf for the last frame, collected by info_callback
    // (installed by gpu_next_core_render_mix). perf_fresh = fresh-frame
    // passes, perf_redraw = blend/redraw passes; consumed by
    // gpu_next_core_get_perf_data. The pl_dispatch_info shader refs are
    // released in gpu_next_core_destroy.
    struct frame_info perf_fresh;
    struct frame_info perf_redraw;

    // Allocated DR buffers
    mp_mutex dr_lock;
    pl_buf *dr_buffers;
    int num_dr_buffers;

    struct scaler_params scalers[SCALER_COUNT];

    // Cached shaders, preserved across options updates
    struct user_hook *user_hooks;
    int num_user_hooks;

    // Cached LUTs (one entry per path; pl_lut shared across the
    // image/lut/target_lut call sites). Cache failures (path -> NULL)
    // to avoid re-trying a broken file. Freed in gpu_next_core_destroy.
    struct user_lut_cache_entry *lut_cache;
    int num_lut_cache;

    // Pool of OSD overlay textures freed by the front-end's unmap callback
    // (one entry per frame_priv.subs slot) and re-acquired by the front-end
    // when the next frame's overlays are built. The textures themselves are
    // recreated to fit on each acquire so the pool stores them by size
    // hint, not strictly by content; destroyed in gpu_next_core_destroy.
    pl_tex *sub_tex;
    int num_sub_tex;

    // Front-end interface (see gpu_next_core_set_frontend), reached by the
    // core's frame-ingest callbacks for genuinely front-end-specific
    // resources (opts, ra, instrumentation, hwdec lookup).
    struct gpu_next_core_frontend fe;

    // Resolved image LUT, set by the front-end's options-update path
    // (gpu_next_core_set_image_lut) and mapped onto each source frame.
    struct pl_custom_lut *image_lut;
    enum pl_lut_type image_lut_type;
};

struct gpu_next_core *gpu_next_core_create(pl_gpu gpu, struct mp_log *log,
                                           pl_log pllog)
{
    struct gpu_next_core *core = talloc_zero(NULL, struct gpu_next_core);
    core->gpu = gpu;
    core->pllog = pllog;
    core->log = log;
    core->pars = pl_options_alloc(pllog);
    core->rr = pl_renderer_create(pllog, gpu);
    core->queue = pl_queue_create(gpu);
    mp_mutex_init(&core->dr_lock);
    return core;
}

pl_options gpu_next_core_options(struct gpu_next_core *core)
{
    return core->pars;
}

pl_renderer gpu_next_core_renderer(struct gpu_next_core *core)
{
    return core->rr;
}

pl_queue gpu_next_core_queue(struct gpu_next_core *core)
{
    return core->queue;
}

void gpu_next_core_set_frontend(struct gpu_next_core *core,
                                const struct gpu_next_core_frontend *fe)
{
    core->fe = *fe;
}

void gpu_next_core_set_image_lut(struct gpu_next_core *core,
                                 struct pl_custom_lut *lut,
                                 enum pl_lut_type type)
{
    core->image_lut = lut;
    core->image_lut_type = type;
}

static void info_callback(void *priv, const struct pl_render_info *info)
{
    struct gpu_next_core *core = priv;
    if (info->index >= VO_PASS_PERF_MAX)
        return; // silently ignore clipped passes, whatever

    struct frame_info *frame;
    switch (info->stage) {
    case PL_RENDER_STAGE_FRAME: frame = &core->perf_fresh; break;
    case PL_RENDER_STAGE_BLEND: frame = &core->perf_redraw; break;
    default: abort();
    }

    frame->count = info->index + 1;
    pl_dispatch_info_move(&frame->info[info->index], info->pass);
}

bool gpu_next_core_render_mix(struct gpu_next_core *core,
                              const struct pl_frame_mix *mix,
                              struct pl_frame *target,
                              const struct pl_render_params *params)
{
    // The draw path always wants render-pass timings, so install the
    // core's perf callback here -- both front-ends get it for free. The
    // screenshot path uses gpu_next_core_render_image() instead, which
    // deliberately has no info callback, matching mainline.
    struct pl_render_params rparams = *params;
    rparams.info_callback = info_callback;
    rparams.info_priv = core;

    if (!pl_render_image_mix(core->rr, mix, target, &rparams))
        return false;

    // mpv calls pl_frames_infer_mix purely for its side effects: it updates
    // the renderer's internal HDR-metadata state (queried afterwards via
    // gpu_next_core_get_hdr_metadata) and adjusts target.color/repr, which
    // the caller reads back. The inferred reference frame itself is unused,
    // matching mainline.
    struct pl_frame ref_frame;
    pl_frames_infer_mix(core->rr, mix, target, &ref_frame);
    return true;
}

bool gpu_next_core_render_image(struct gpu_next_core *core,
                                const struct pl_frame *image,
                                const struct pl_frame *target,
                                const struct pl_render_params *params)
{
    return pl_render_image(core->rr, image, target, params);
}

bool gpu_next_core_get_hdr_metadata(struct gpu_next_core *core,
                                    struct pl_hdr_metadata *metadata)
{
    return pl_renderer_get_hdr_metadata(core->rr, metadata);
}

static void copy_frame_info_to_mp(struct frame_info *pl,
                                  struct mp_frame_perf *mp,
                                  struct mp_pass_perf *hwdec_perf,
                                  struct mp_pass_perf *sw_upload_perf)
{
    static_assert(MP_ARRAY_SIZE(pl->info) == MP_ARRAY_SIZE(mp->perf), "");
    mp_assert(pl->count <= VO_PASS_PERF_MAX);

    struct mp_pass_perf *perf = mp->perf;
    char (*desc)[VO_PASS_DESC_MAX_LEN] = mp->desc;
    struct mp_pass_perf *perf_end = perf + VO_PASS_PERF_MAX;

    if (hwdec_perf && hwdec_perf->count > 0) {
        *perf++ = *hwdec_perf;
        snprintf(*desc, sizeof(*desc), "map frame (hwdec)");
        desc++;
    }

    if (sw_upload_perf && sw_upload_perf->count > 0) {
        *perf++ = *sw_upload_perf;
        snprintf(*desc, sizeof(*desc), "upload frame");
        desc++;
    }

    for (int i = 0; i < pl->count && perf < perf_end; ++i) {
        const struct pl_dispatch_info *pass = &pl->info[i];

        static_assert(VO_PERF_SAMPLE_COUNT >= MP_ARRAY_SIZE(pass->samples), "");
        mp_assert(pass->num_samples <= MP_ARRAY_SIZE(pass->samples));

        perf->count = MPMIN(pass->num_samples, VO_PERF_SAMPLE_COUNT);
        memcpy(perf->samples, pass->samples, perf->count * sizeof(pass->samples[0]));
        perf->last = pass->last;
        perf->peak = pass->peak;
        perf->avg = pass->average;

        strncpy(*desc, pass->shader->description, sizeof(*desc) - 1);
        (*desc)[sizeof(*desc) - 1] = '\0';
        perf++;
        desc++;
    }

    mp->count = perf - mp->perf;
}

void gpu_next_core_get_perf_data(struct gpu_next_core *core,
                                 struct voctrl_performance_data *perf)
{
    copy_frame_info_to_mp(&core->perf_fresh, &perf->fresh,
                          &core->hwdec_perf, &core->sw_upload_perf);
    copy_frame_info_to_mp(&core->perf_redraw, &perf->redraw, NULL, NULL);
}

void gpu_next_core_flush_cache(struct gpu_next_core *core)
{
    pl_renderer_flush_cache(core->rr);
}

void gpu_next_core_queue_check_refill(struct gpu_next_core *core,
                                      double current_pts, double pts_offset,
                                      double ideal_frame_vsync_duration)
{
    if (core->want_reset)
        return;

    // pl_queue advances its internal virtual PTS and culls available frames
    // based on this value and the VPS/FPS ratio. Requesting a non-monotonic PTS
    // is an invalid use of pl_queue. Reset it if this happens in an attempt to
    // recover as much as possible. Ideally, this should never occur, and if it
    // does, it should be corrected. The ideal_frame_vsync may be negative if
    // the last draw did not align perfectly with the vsync. In this case, we
    // should have the previous frame available in pl_queue, or a reset is
    // already requested. Clamp the check to 0, as we don't have the previous
    // frame in vo_frame anyway.
    struct pl_source_frame vpts;
    if (pl_queue_peek(core->queue, 0, &vpts) &&
        current_pts + MPMAX(0, pts_offset) < vpts.pts)
    {
        MP_VERBOSE(core, "Forcing queue refill, PTS(%f + %f | %f) < VPTS(%f)\n",
                   current_pts, pts_offset, ideal_frame_vsync_duration, vpts.pts);
        core->want_reset = true;
    }
}

bool gpu_next_core_queue_accept(struct gpu_next_core *core, int id)
{
    if (core->want_reset) {
        pl_queue_reset(core->queue);
        core->last_pts = 0.0;
        core->last_id = 0;
        core->want_reset = false;
        core->flush_cache = true;
    }

    if (core->flush_cache) {
        gpu_next_core_flush_cache(core);
        core->flush_cache = false;
    }

    if (id <= core->last_id)
        return false; // ignore already seen frames

    core->last_id = id;
    return true;
}

void gpu_next_core_queue_request_reset(struct gpu_next_core *core)
{
    core->want_reset = true;
}

void gpu_next_core_queue_set_flush(struct gpu_next_core *core, bool flush)
{
    core->flush_cache = flush;
}

enum pl_queue_status gpu_next_core_queue_update(struct gpu_next_core *core,
                                                struct pl_frame_mix *mix,
                                                struct pl_queue_params *qparams,
                                                double current_pts)
{
    // Depending on the vsync ratio, we may be up to half of the vsync
    // duration before the current frame time. This works fine because
    // pl_queue will have this frame, unless it's after a reset event. In
    // this case, start from the first available frame.
    struct pl_source_frame first;
    if (pl_queue_peek(core->queue, 0, &first) && qparams->pts < first.pts) {
        if (first.pts != current_pts)
            MP_VERBOSE(core, "Current PTS(%f) != VPTS(%f)\n", current_pts, first.pts);
        MP_VERBOSE(core, "Clamping first frame PTS from %f to %f\n",
                   qparams->pts, first.pts);
        qparams->pts = first.pts;
    }
    core->last_pts = qparams->pts;

    return pl_queue_update(core->queue, mix, qparams);
}

double gpu_next_core_queue_last_pts(struct gpu_next_core *core)
{
    return core->last_pts;
}

int gpu_next_core_required_frames(struct gpu_next_core *core)
{
    int req = 2;
    const struct pl_filter_config *fm = core->pars->params.frame_mixer;
    if (fm) {
        req += ceilf(fm->kernel->radius) *
               (core->pars->params.skip_anti_aliasing ? 1 : 2);
    }
    return req;
}

void gpu_next_core_destroy(struct gpu_next_core **core_ptr)
{
    struct gpu_next_core *core = *core_ptr;
    if (!core)
        return;

    // Release any in-flight frames first: their unmap returns DR buffers
    // (so the assert below holds) and runs hwdec_release on hwdec frames
    // (so the mapper must still exist). The front-end must call
    // gpu_next_core_destroy() before tearing down its ra_hwdec_ctx so
    // ra_hwdec_mapper_free() below stays valid.
    pl_queue_destroy(&core->queue);

    ra_hwdec_mapper_free(&core->hwdec_mapper);
    timer_pool_destroy(core->hwdec_timer);
    core->hwdec_timer = NULL;
    timer_pool_destroy(core->sw_upload_timer);
    core->sw_upload_timer = NULL;

    for (int i = 0; i < core->num_user_hooks; i++)
        pl_mpv_user_shader_destroy(&core->user_hooks[i].hook);

    for (int i = 0; i < core->num_lut_cache; i++)
        pl_lut_free(&core->lut_cache[i].lut);

    for (int i = 0; i < core->num_sub_tex; i++)
        pl_tex_destroy(core->gpu, &core->sub_tex[i]);

    for (int i = 0; i < VO_PASS_PERF_MAX; ++i) {
        pl_shader_info_deref(&core->perf_fresh.info[i].shader);
        pl_shader_info_deref(&core->perf_redraw.info[i].shader);
    }

    mp_assert(core->num_dr_buffers == 0);
    mp_mutex_destroy(&core->dr_lock);

    pl_renderer_destroy(&core->rr);
    pl_options_free(&core->pars);

    talloc_free(core);
    *core_ptr = NULL;
}

pl_buf gpu_next_core_get_dr_buf(struct gpu_next_core *core, const uint8_t *ptr)
{
    mp_mutex_lock(&core->dr_lock);

    for (int i = 0; i < core->num_dr_buffers; i++) {
        pl_buf buf = core->dr_buffers[i];
        if (ptr >= buf->data && ptr < buf->data + buf->params.size) {
            mp_mutex_unlock(&core->dr_lock);
            return buf;
        }
    }

    mp_mutex_unlock(&core->dr_lock);
    return NULL;
}

static void free_dr_buf(void *opaque, uint8_t *data)
{
    struct gpu_next_core *core = opaque;
    mp_mutex_lock(&core->dr_lock);

    for (int i = 0; i < core->num_dr_buffers; i++) {
        if (core->dr_buffers[i]->data == data) {
            pl_buf_destroy(core->gpu, &core->dr_buffers[i]);
            MP_TARRAY_REMOVE_AT(core->dr_buffers, core->num_dr_buffers, i);
            mp_mutex_unlock(&core->dr_lock);
            return;
        }
    }

    MP_ASSERT_UNREACHABLE();
}

struct mp_image *gpu_next_core_get_image(struct gpu_next_core *core,
                                         int imgfmt, int w, int h,
                                         int stride_align, int flags)
{
    pl_gpu gpu = core->gpu;
    if (!gpu->limits.thread_safe || !gpu->limits.max_mapped_size)
        return NULL;

    if ((flags & VO_DR_FLAG_HOST_CACHED) && !gpu->limits.host_cached)
        return NULL;

    stride_align = mp_lcm(stride_align, gpu->limits.align_tex_xfer_pitch);
    stride_align = mp_lcm(stride_align, gpu->limits.align_tex_xfer_offset);
    int size = mp_image_get_alloc_size(imgfmt, w, h, stride_align);
    if (size < 0)
        return NULL;

    pl_buf buf = pl_buf_create(gpu, &(struct pl_buf_params) {
        .memory_type = PL_BUF_MEM_HOST,
        .host_mapped = true,
        .size = size + stride_align,
    });

    if (!buf)
        return NULL;

    struct mp_image *mpi = mp_image_from_buffer(imgfmt, w, h, stride_align,
                                                buf->data, buf->params.size,
                                                core, free_dr_buf);
    if (!mpi) {
        pl_buf_destroy(gpu, &buf);
        return NULL;
    }

    mp_mutex_lock(&core->dr_lock);
    MP_TARRAY_APPEND(core, core->dr_buffers, core->num_dr_buffers, buf);
    mp_mutex_unlock(&core->dr_lock);

    return mpi;
}

int gpu_next_core_plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                         struct pl_bit_encoding *out_bits,
                                         enum mp_imgfmt imgfmt, bool use_uint)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;

    if (desc.flags & MP_IMGFLAG_HWACCEL)
        return 0; // HW-accelerated frames need to be mapped differently

    if (!(desc.flags & MP_IMGFLAG_NE))
        return 0; // GPU endianness follows the host's

    if (desc.flags & MP_IMGFLAG_PAL)
        return 0; // Palette formats (currently) not supported in libplacebo

    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return 0; // Floating-point YUV (currently) unsupported

    bool has_bits = false;
    bool any_padded = false;

    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        if (desc.bpp[p] % 8)
            return 0; // Pixel size is not byte-aligned

        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;

            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;

            // Sort components by offset order, while keeping track of the
            // semantic mapping in `data->component_map`
            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        uint64_t total_bits = 0;

        // Fill in the pl_plane_data fields for each component
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];
            any_padded |= sorted[c].pad;

            // Ignore bit encoding of alpha channel
            if (!out_bits || data->component_map[c] == PL_CHANNEL_A)
                continue;

            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };

            if (!has_bits) {
                *out_bits = bits;
                has_bits = true;
            } else {
                if (!pl_bit_encoding_equal(out_bits, &bits)) {
                    // Bit encoding differs between components/planes,
                    // cannot handle this
                    *out_bits = (struct pl_bit_encoding) {0};
                    out_bits = NULL;
                }
            }
        }

        data->pixel_stride = desc.bpp[p] / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT)
                            ? PL_FMT_FLOAT
                            : (use_uint ? PL_FMT_UINT : PL_FMT_UNORM);
    }

    if (any_padded && !out_bits)
        return 0; // can't handle padded components without `pl_bit_encoding`

    return desc.num_planes;
}

bool gpu_next_core_format_supported(pl_gpu gpu, int format, bool use_uint)
{
    struct pl_bit_encoding bits;
    struct pl_plane_data data[4] = {0};
    int planes = gpu_next_core_plane_data_from_imgfmt(data, &bits, format, use_uint);
    if (!planes)
        return false;

    for (int i = 0; i < planes; i++) {
        if (!pl_plane_find_fmt(gpu, NULL, &data[i]))
            return false;
    }

    return true;
}

bool gpu_next_core_upload_sw_planes(struct gpu_next_core *core,
                                    struct ra *ra,
                                    struct mp_image *mpi,
                                    pl_tex *tex,
                                    struct pl_frame *frame)
{
    pl_gpu gpu = core->gpu;

    if (!core->sw_upload_timer)
        core->sw_upload_timer = timer_pool_create(ra);
    timer_pool_start(core->sw_upload_timer);

    struct pl_plane_data data[4] = {0};
    bool use_uint = false;

    // At this point, query_format() has already accepted the format; this
    // only picks UINT as a fallback when the UNORM path is unsupported.
    if (!gpu_next_core_format_supported(gpu, mpi->imgfmt, false))
        use_uint = true;

    frame->num_planes = gpu_next_core_plane_data_from_imgfmt(
        data, &frame->repr.bits, mpi->imgfmt, use_uint);

    for (int n = 0; n < frame->num_planes; n++) {
        struct pl_plane *plane = &frame->planes[n];
        data[n].width = mp_image_plane_w(mpi, n);
        data[n].height = mp_image_plane_h(mpi, n);
        if (mpi->stride[n] < 0) {
            data[n].pixels = mpi->planes[n] + (data[n].height - 1) * mpi->stride[n];
            data[n].row_stride = -mpi->stride[n];
            plane->flipped = true;
        } else {
            data[n].pixels = mpi->planes[n];
            data[n].row_stride = mpi->stride[n];
        }

        pl_buf buf = gpu_next_core_get_dr_buf(core, data[n].pixels);
        if (buf) {
            data[n].buf = buf;
            data[n].buf_offset = (uint8_t *) data[n].pixels - buf->data;
            data[n].pixels = NULL;
        }
        // Keep the image alive until libplacebo is done reading it.
        if (gpu->limits.callbacks) {
            mp_assert(!data[n].callback);
            data[n].callback = talloc_free;
            mp_assert(!data[n].priv);
            data[n].priv = mp_image_new_ref(mpi);
        }

        if (!pl_upload_plane(gpu, plane, &tex[n], &data[n])) {
            MP_ERR(core, "Failed uploading frame!\n");
            talloc_free(data[n].priv);
            talloc_free(mpi);
            timer_pool_stop(core->sw_upload_timer);
            return false;
        }

        // Without async callback support, we have to poll...
        if (!gpu->limits.callbacks && data[n].buf)
            while (pl_buf_poll(gpu, data[n].buf, UINT64_MAX));
    }

    timer_pool_stop(core->sw_upload_timer);
    core->sw_upload_perf = timer_pool_measure(core->sw_upload_timer);
    return true;
}

void gpu_next_core_sw_upload_perf_reset(struct gpu_next_core *core)
{
    core->sw_upload_perf.count = 0;
}

bool gpu_next_core_hwdec_reconfig(struct gpu_next_core *core, struct ra *ra,
                                  struct ra_hwdec *hwdec,
                                  const struct mp_image_params *par)
{
    if (core->hwdec_mapper) {
        if (mp_image_params_static_equal(par, &core->hwdec_mapper->src_params)) {
            core->hwdec_mapper->src_params.repr.dovi = par->repr.dovi;
            core->hwdec_mapper->dst_params.repr.dovi = par->repr.dovi;
            core->hwdec_mapper->src_params.color.hdr = par->color.hdr;
            core->hwdec_mapper->dst_params.color.hdr = par->color.hdr;
            return true;
        }
        ra_hwdec_mapper_free(&core->hwdec_mapper);
        timer_pool_destroy(core->hwdec_timer);
        core->hwdec_timer = NULL;
    }

    core->hwdec_mapper = ra_hwdec_mapper_create(hwdec, par);
    if (!core->hwdec_mapper) {
        MP_ERR(core, "Initializing texture for hardware decoding failed.\n");
        return false;
    }
    core->hwdec_timer = timer_pool_create(ra);
    return true;
}

const struct mp_image_params *gpu_next_core_hwdec_dst_params(
    const struct gpu_next_core *core)
{
    return &core->hwdec_mapper->dst_params;
}

// For RAs not based on ra_pl, this creates a new pl_tex wrapper.
static pl_tex hwdec_plane_tex(struct gpu_next_core *core, int n)
{
    struct ra_tex *ratex = core->hwdec_mapper->tex[n];
    struct ra *ra = core->hwdec_mapper->ra;
    if (ra_pl_get(ra))
        return (pl_tex) ratex->priv;

#if HAVE_GL && defined(PL_HAVE_OPENGL)
    if (ra_is_gl(ra) && pl_opengl_get(core->gpu)) {
        struct pl_opengl_wrap_params par = {
            .width = ratex->params.w,
            .height = ratex->params.h,
        };

        ra_gl_get_format(ratex->params.format, &par.iformat,
                         &(GLenum){0}, &(GLenum){0});
        ra_gl_get_raw_tex(ra, ratex, &par.texture, &par.target);
        return pl_opengl_wrap(core->gpu, &par);
    }
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (ra_is_d3d11(ra)) {
        int array_slice = 0;
        ID3D11Resource *res = ra_d3d11_get_raw_tex(ra, ratex, &array_slice);
        pl_tex tex = pl_d3d11_wrap(core->gpu, pl_d3d11_wrap_params(
            .tex = res,
            .array_slice = array_slice,
            .fmt = ra_d3d11_get_format(ratex->params.format),
            .w = ratex->params.w,
            .h = ratex->params.h,
        ));
        SAFE_RELEASE(res);
        return tex;
    }
#endif

    MP_ERR(core, "Failed mapping hwdec frame? Open a bug!\n");
    return NULL;
}

bool gpu_next_core_hwdec_acquire(struct gpu_next_core *core,
                                 struct mp_image *mpi,
                                 struct pl_frame *frame)
{
    timer_pool_start(core->hwdec_timer);
    if (ra_hwdec_mapper_map(core->hwdec_mapper, mpi) < 0) {
        MP_ERR(core, "Mapping hardware decoded surface failed.\n");
        timer_pool_stop(core->hwdec_timer);
        return false;
    }

    for (int n = 0; n < frame->num_planes; n++) {
        if (!(frame->planes[n].texture = hwdec_plane_tex(core, n))) {
            timer_pool_stop(core->hwdec_timer);
            return false;
        }
    }

    timer_pool_stop(core->hwdec_timer);
    core->hwdec_perf = timer_pool_measure(core->hwdec_timer);
    return true;
}

void gpu_next_core_hwdec_release(struct gpu_next_core *core,
                                 struct pl_frame *frame)
{
    if (!ra_pl_get(core->hwdec_mapper->ra)) {
        for (int n = 0; n < frame->num_planes; n++)
            pl_tex_destroy(core->gpu, &frame->planes[n].texture);
    }

    ra_hwdec_mapper_unmap(core->hwdec_mapper);
}

void gpu_next_core_hwdec_perf_reset(struct gpu_next_core *core)
{
    core->hwdec_perf.count = 0;
}

const struct pl_filter_config *gpu_next_core_map_scaler(
    struct gpu_next_core *core, const struct gl_video_opts *opts,
    enum scaler_unit unit)
{
    const struct pl_filter_preset fixed_scalers[] = {
        { "bilinear",       &pl_filter_bilinear },
        { "bicubic_fast",   &pl_filter_bicubic },
        { "nearest",        &pl_filter_nearest },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset fixed_frame_mixers[] = {
        { "linear",         &pl_filter_bilinear },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset *fixed_presets =
        unit == SCALER_TSCALE ? fixed_frame_mixers : fixed_scalers;

    const struct scaler_config *cfg = &opts->scaler[unit];
    struct scaler_config tmp;
    if (cfg->kernel.function == SCALER_INHERIT) {
        tmp = *cfg;
        scaler_conf_merge(&tmp, &opts->scaler[SCALER_SCALE], unit);
        cfg = &tmp;
    }

    const char *kernel_name = m_opt_choice_str(cfg->kernel.functions,
                                               cfg->kernel.function);

    for (int i = 0; fixed_presets[i].name; i++) {
        if (strcmp(kernel_name, fixed_presets[i].name) == 0)
            return fixed_presets[i].filter;
    }

    // Attempt loading filter preset first, fall back to raw filter function
    struct scaler_params *par = &core->scalers[unit];
    const struct pl_filter_preset *preset;
    const struct pl_filter_function_preset *fpreset;
    if ((preset = pl_find_filter_preset(kernel_name))) {
        par->config = *preset->filter;
    } else if ((fpreset = pl_find_filter_function_preset(kernel_name))) {
        par->config = (struct pl_filter_config) {
            .kernel = fpreset->function,
            .params[0] = fpreset->function->params[0],
            .params[1] = fpreset->function->params[1],
        };
    } else {
        MP_ERR(core, "Failed mapping filter function '%s', no libplacebo analog?\n",
               kernel_name);
        return &pl_filter_bilinear;
    }

    const struct pl_filter_function_preset *wpreset;
    if ((wpreset = pl_find_filter_function_preset(
             m_opt_choice_str(cfg->window.functions, cfg->window.function)))) {
        par->config.window = wpreset->function;
        par->config.wparams[0] = wpreset->function->params[0];
        par->config.wparams[1] = wpreset->function->params[1];
    }

    for (int i = 0; i < 2; i++) {
        if (!isnan(cfg->kernel.params[i]))
            par->config.params[i] = cfg->kernel.params[i];
        if (!isnan(cfg->window.params[i]))
            par->config.wparams[i] = cfg->window.params[i];
    }

    par->config.clamp = cfg->clamp;
    if (cfg->antiring > 0.0)
        par->config.antiring = cfg->antiring;
    if (cfg->kernel.blur > 0.0)
        par->config.blur = cfg->kernel.blur;
    if (cfg->kernel.taper > 0.0)
        par->config.taper = cfg->kernel.taper;
    if (cfg->radius > 0.0) {
        if (par->config.kernel->resizable) {
            par->config.radius = cfg->radius;
        } else {
            MP_WARN(core, "Filter radius specified but filter '%s' is not "
                    "resizable, ignoring\n", kernel_name);
        }
    }

    return &par->config;
}

const struct pl_hook *gpu_next_core_load_hook(struct gpu_next_core *core,
                                              struct mpv_global *global,
                                              const char *path)
{
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < core->num_user_hooks; i++) {
        if (strcmp(core->user_hooks[i].path, path) == 0)
            return core->user_hooks[i].hook;
    }

    char *fname = mp_get_user_path(NULL, global, path);
    bstr shader = stream_read_file(fname, core, global, 1000000000); // 1GB
    talloc_free(fname);

    const struct pl_hook *hook = NULL;
    if (shader.len)
        hook = pl_mpv_user_shader_parse(core->gpu, shader.start, shader.len);

    MP_TARRAY_APPEND(core, core->user_hooks, core->num_user_hooks, (struct user_hook) {
        .path = talloc_strdup(core, path),
        .hook = hook,
    });

    return hook;
}

struct pl_custom_lut *gpu_next_core_load_lut(struct gpu_next_core *core,
                                             struct mpv_global *global,
                                             const char *path)
{
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < core->num_lut_cache; i++) {
        if (strcmp(core->lut_cache[i].path, path) == 0)
            return core->lut_cache[i].lut;
    }

    char *fname = mp_get_user_path(NULL, global, path);
    MP_VERBOSE(core, "Loading custom LUT '%s'\n", fname);
    // 1.5 GiB, matches libplacebo's internal lut cache limit
    const int lut_max_size = 1536 << 20;
    struct bstr lutdata = stream_read_file(fname, NULL, global, lut_max_size);

    struct pl_custom_lut *lut = NULL;
    if (!lutdata.len) {
        MP_ERR(core, "Failed to read LUT data from %s, make sure it's a valid "
                     "file and smaller or equal to %d bytes\n", fname, lut_max_size);
    } else {
        lut = pl_lut_parse_cube(core->pllog, lutdata.start, lutdata.len);
    }
    talloc_free(fname);
    talloc_free(lutdata.start);

    MP_TARRAY_APPEND(core, core->lut_cache, core->num_lut_cache,
                     (struct user_lut_cache_entry) {
        .path = talloc_strdup(core, path),
        .lut = lut,
    });

    return lut;
}

void gpu_next_core_sub_tex_push(struct gpu_next_core *core, pl_tex tex)
{
    if (!tex)
        return;
    MP_TARRAY_APPEND(core, core->sub_tex, core->num_sub_tex, tex);
}

pl_tex gpu_next_core_sub_tex_pop(struct gpu_next_core *core)
{
    pl_tex tex = NULL;
    MP_TARRAY_POP(core->sub_tex, core->num_sub_tex, &tex);
    return tex;
}

// pl_frame acquire/release callbacks for hwdec source frames, installed
// on the pl_frame by gpu_next_core_map_frame. They wrap the core's hwdec
// interop in the front-end's optional "hwdec-map" timer (the windowed VO
// has a stats_ctx; the libmpv backend leaves the timer hooks NULL).
static bool hwdec_acquire(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct frame_priv *fp = mpi->priv;
    struct gpu_next_core *core = fp->core;
    if (!gpu_next_core_hwdec_reconfig(core, core->fe.ra, fp->hwdec,
                                      &mpi->params))
        return false;

    if (core->fe.timer_start)
        core->fe.timer_start(core->fe.timer_ctx, "hwdec-map");
    bool ok = gpu_next_core_hwdec_acquire(core, mpi, frame);
    if (core->fe.timer_end)
        core->fe.timer_end(core->fe.timer_ctx, "hwdec-map");
    return ok;
}

static void hwdec_release(pl_gpu gpu, struct pl_frame *frame)
{
    struct mp_image *mpi = frame->user_data;
    struct frame_priv *fp = mpi->priv;
    gpu_next_core_hwdec_release(fp->core, frame);
}

bool gpu_next_core_map_frame(pl_gpu gpu, pl_tex *tex,
                             const struct pl_source_frame *src,
                             struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct mp_image_params par = mpi->params;
    struct frame_priv *fp = mpi->priv;
    struct gpu_next_core *core = fp->core;
    const struct gl_video_opts *opts = core->fe.opts;

    fp->hwdec = core->fe.hwdec_get
        ? core->fe.hwdec_get(core->fe.hwdec_ctx, mpi->imgfmt)
        : NULL;
    if (fp->hwdec) {
        // Note: We don't actually need the mapper to map the frame yet, we
        // only reconfig the mapper here (potentially creating it) to access
        // `dst_params`. In practice, though, this should not matter unless the
        // image format changes mid-stream.
        if (!gpu_next_core_hwdec_reconfig(core, core->fe.ra, fp->hwdec,
                                          &mpi->params)) {
            talloc_free(mpi);
            return false;
        }

        par = *gpu_next_core_hwdec_dst_params(core);
    }

    mp_image_params_guess_csp(&par);

    *frame = (struct pl_frame) {
        .color = par.color,
        .repr = par.repr,
        .profile = {
            .data = mpi->icc_profile ? mpi->icc_profile->data : NULL,
            .len = mpi->icc_profile ? mpi->icc_profile->size : 0,
        },
        .rotation = par.rotate / 90,
        .user_data = mpi,
    };

    if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(frame->color.transfer))
        frame->color.hdr.max_luma = opts->hdr_reference_white;

    if (opts->treat_srgb_as_power22 & 1 && frame->color.transfer == PL_COLOR_TRC_SRGB) {
        // The sRGB EOTF is a pure gamma 2.2 function. See reference display in
        // IEC 61966-2-1-1999. Linearize sRGB to display light.
        frame->color.transfer = PL_COLOR_TRC_GAMMA22;
    }

    if (fp->hwdec) {
        gpu_next_core_sw_upload_perf_reset(core);

        struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(par.imgfmt);
        frame->acquire = hwdec_acquire;
        frame->release = hwdec_release;
        frame->num_planes = desc.num_planes;
        for (int n = 0; n < frame->num_planes; n++) {
            struct pl_plane *plane = &frame->planes[n];
            int *map = plane->component_mapping;
            for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
                if (desc.comps[c].plane != n)
                    continue;

                // Sort by component offset
                uint8_t offset = desc.comps[c].offset;
                int index = plane->components++;
                while (index > 0 && desc.comps[map[index - 1]].offset > offset) {
                    map[index] = map[index - 1];
                    index--;
                }
                map[index] = c;
            }
        }

    } else { // swdec
        gpu_next_core_hwdec_perf_reset(core);

        if (core->fe.timer_start)
            core->fe.timer_start(core->fe.timer_ctx, "swdec-upload");
        bool ok = gpu_next_core_upload_sw_planes(core, core->fe.ra, mpi, tex,
                                                 frame);
        if (core->fe.timer_end)
            core->fe.timer_end(core->fe.timer_ctx, "swdec-upload");
        if (!ok)
            return false;
    }

    // Update chroma location, must be done after initializing planes
    pl_frame_set_chroma_location(frame, par.chroma_location);

    if (mpi->film_grain)
        pl_film_grain_from_av(&frame->film_grain, (AVFilmGrainParams *) mpi->film_grain->data);

    // Compute a unique signature for any attached ICC profile. Wasteful in
    // theory if the ICC profile is the same for multiple frames, but in
    // practice ICC profiles are overwhelmingly going to be attached to
    // still images so it shouldn't matter.
    pl_icc_profile_compute_signature(&frame->profile);

    // Image LUT resolved by the front-end's options-update path
    // (gpu_next_core_set_image_lut); an --image-lut change forces a queue
    // reset, so no in-flight frame straddles a change.
    frame->lut = core->image_lut;
    frame->lut_type = core->image_lut_type;
    return true;
}

void gpu_next_core_unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                               const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    for (int i = 0; i < MP_ARRAY_SIZE(fp->subs.entries); i++)
        gpu_next_core_sub_tex_push(fp->core, fp->subs.entries[i].tex);
    talloc_free(mpi);
}

void gpu_next_core_discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

void gpu_next_core_update_hook_opts_dynamic(const struct pl_hook *hook,
                                            const struct mp_image *mpi)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        double val;
        const struct pl_hook_par *hp = &hook->parameters[i];
        if (!gpu_get_auto_param(mpi, bstr0(hp->name), &val))
            continue;

        switch (hp->type) {
        case PL_VAR_FLOAT: hp->data->f = val; break;
        case PL_VAR_SINT:  hp->data->i = lrint(val); break;
        case PL_VAR_UINT:  hp->data->u = lrint(val); break;
        }
    }
}

void gpu_next_core_apply_crop(struct pl_frame *frame, struct mp_rect crop,
                              int width, int height)
{
    frame->crop = (struct pl_rect2df) {
        .x0 = crop.x0,
        .y0 = crop.y0,
        .x1 = crop.x1,
        .y1 = crop.y1,
    };

    // mpv gives us rotated/flipped rects, libplacebo expects unrotated
    pl_rect2df_rotate(&frame->crop, -frame->rotation);
    if (frame->crop.x1 < frame->crop.x0) {
        frame->crop.x0 = width - frame->crop.x0;
        frame->crop.x1 = width - frame->crop.x1;
    }

    if (frame->crop.y1 < frame->crop.y0) {
        frame->crop.y0 = height - frame->crop.y0;
        frame->crop.y1 = height - frame->crop.y1;
    }
}

void gpu_next_core_apply_target_contrast(const struct gl_video_opts *opts,
                                         struct pl_color_space *color,
                                         float min_luma)
{
    // Auto mode, use target value if available
    if (!opts->target_contrast) {
        color->hdr.min_luma = min_luma;
        return;
    }

    // Infinite contrast
    if (opts->target_contrast == -1) {
        color->hdr.min_luma = 1e-7;
        mp_assert(color->hdr.min_luma > 0);
        return;
    }

    // Infer max_luma for current pl_color_space
    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color = color,
        // with HDR10 meta to respect value if already set
        .metadata = PL_HDR_METADATA_HDR10,
        .scaling = PL_HDR_NITS,
        .out_max = &color->hdr.max_luma
    ));

    color->hdr.min_luma = color->hdr.max_luma / opts->target_contrast;
}

enum target_hint_action gpu_next_core_target_hint(
    const struct gl_video_opts *opts, int target_hint, int target_hint_mode,
    const struct pl_color_space *source, pl_log pllog,
    pl_icc_object *icc_profile, struct pl_icc_params *icc_params,
    struct pl_color_space *target_csp, struct pl_color_space *out_hint,
    bool *out_target_hint, bool *out_target_unknown)
{
    if (target_csp->primaries == PL_COLOR_PRIM_UNKNOWN)
        target_csp->primaries = mp_get_best_prim_container(&target_csp->hdr.prim);
    if (!pl_color_transfer_is_hdr(target_csp->transfer)) {
        // limit min_luma to 1000:1 contrast ratio in SDR mode
        if (target_csp->hdr.min_luma > PL_COLOR_SDR_WHITE / PL_COLOR_SDR_CONTRAST)
            target_csp->hdr.min_luma = 0;
    }
    // maxFALL in display metadata is in fact MaxFullFrameLuminance. Wayland
    // reports it as maxFALL directly, but this doesn't mean the same thing.
    target_csp->hdr.max_fall = 0;

    struct pl_color_space hint = {0};
    bool do_hint = target_hint == 1 ||
                   (target_hint == -1 &&
                    target_csp->transfer != PL_COLOR_TRC_UNKNOWN);
    // Assume HDR is supported, if target_csp() is not available
    // TODO: Remove this fallback when all backends support target_csp()
    bool target_unknown = target_csp->transfer == PL_COLOR_TRC_UNKNOWN;
    if (target_unknown) {
        *target_csp = (struct pl_color_space){
            .transfer = opts->target_trc ? opts->target_trc : pl_color_space_hdr10.transfer };
    }
    enum target_hint_action action = TARGET_HINT_NONE;
    if (do_hint && source) {
        const struct pl_color_space *target = target_csp;
        hint = *source;
        // Apply target contrast to the hint, this is important for SDR, because
        // libplacebo defaults to 1000:1 contrast ratio otherwise.
        if (!hint.hdr.min_luma)
            hint.hdr.min_luma = target->hdr.min_luma;
        if (target_hint_mode == 0) {
            hint = *target;
            if (pl_color_transfer_is_hdr(hint.transfer) && !pl_primaries_valid(&hint.hdr.prim))
                pl_color_space_merge(&hint, source);
            if (target_unknown && !opts->target_trc && !pl_color_transfer_is_hdr(source->transfer))
                hint = *source;
            // Restore target luminance if it was present, note that we check
            // max_luma only, this make sure that max_cll/max_fall is not take
            // from source.
            if (target->hdr.max_luma) {
                hint.hdr.max_luma = target->hdr.max_luma;
                hint.hdr.min_luma = target->hdr.min_luma;
                hint.hdr.max_cll  = target->hdr.max_cll;
                hint.hdr.max_fall = target->hdr.max_fall;
            }
        }
        if (target_hint_mode == 2) { // source-dynamic
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color      = &hint,
                .metadata   = PL_HDR_METADATA_ANY,
                .scaling    = PL_HDR_NITS,
                .out_min    = !hint.hdr.min_luma ? &hint.hdr.min_luma : NULL,
                .out_max    = &hint.hdr.max_luma,
            ));
            // Set maxCLL to dynamic max luminance. Note that libplacebo uses
            // max luminace as maxCLL in practice.
            hint.hdr.max_cll = hint.hdr.max_luma;
            // Keep maxFALL from static metadata, unless its value is too high.
            // Could be set to 0, but let's keep it for now.
            if (hint.hdr.max_fall > hint.hdr.max_cll)
                hint.hdr.max_fall = 0;
        }
        // Infer missing bits now. This is important so that we don't lose
        // information after user option overrides. For example, if the user
        // sets target_trc to PQ, but the hint(source) is SDR, we want to fill
        // in SDR luminance values instead of the default PQ range.
        struct pl_color_space source_csp = *source;
        pl_color_space_infer_map(&source_csp, &hint);
        // Always prefer target luminance and transfer for inverse tone mapping
        if (pl_color_transfer_is_hdr(target->transfer) && opts->tone_map.inverse) {
            hint.transfer     = target->transfer;
            hint.hdr.max_luma = target->hdr.max_luma;
            hint.hdr.min_luma = target->hdr.min_luma;
            hint.hdr.max_cll  = target->hdr.max_cll;
            hint.hdr.max_fall = target->hdr.max_fall;
        }
        if (opts->target_prim)
            hint.primaries = opts->target_prim;
        if (opts->target_gamut)
            mp_parse_raw_primaries(mp_null_log, opts->target_gamut, &hint.hdr.prim);
        if (opts->target_trc)
            hint.transfer = opts->target_trc;
        if (opts->target_peak)
            hint.hdr.max_luma = opts->target_peak;
        if (opts->hdr_reference_white && !pl_color_transfer_is_hdr(hint.transfer))
            hint.hdr.max_luma = opts->hdr_reference_white;
        // Always set maxCLL, display uses this metadata and we shouldn't let it
        // fallback to default value.
        if (!hint.hdr.max_cll)
            hint.hdr.max_cll = hint.hdr.max_luma;
        // If tone mapping is required, adjust maxCLL and maxFALL
        if (source->hdr.max_luma > hint.hdr.max_luma || opts->tone_map.inverse) {
            // Set maxCLL to the target luminance if it's not already lower
            if (!hint.hdr.max_cll || hint.hdr.max_luma < hint.hdr.max_cll || opts->tone_map.inverse)
                hint.hdr.max_cll = hint.hdr.max_luma;
            // There's no reliable way to estimate maxFALL here
            hint.hdr.max_fall = 0;
        }
        if (hint.hdr.max_cll && hint.hdr.max_fall > hint.hdr.max_cll)
            hint.hdr.max_fall = 0;
        gpu_next_core_apply_target_contrast(opts, &hint, hint.hdr.min_luma);
        if (*icc_profile)
            hint = (*icc_profile)->csp;
        if (opts->icc_opts->icc_use_luma) {
            icc_params->max_luma = 0.0f;
        } else {
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color    = &hint,
                .metadata = PL_HDR_METADATA_HDR10, // use only static HDR nits
                .scaling  = PL_HDR_NITS,
                .out_max  = &icc_params->max_luma,
            ));
        }
        pl_icc_update(pllog, icc_profile, NULL, icc_params);
        // Update again after possible max_luma change
        if (*icc_profile)
            hint = (*icc_profile)->csp;
        action = TARGET_HINT_SET;
    } else if (!do_hint) {
        if (!hint.hdr.min_luma)
            hint.hdr.min_luma = target_csp->hdr.min_luma;
        action = TARGET_HINT_CLEAR;
    }

    *out_hint = hint;
    *out_target_hint = do_hint;
    *out_target_unknown = target_unknown;
    return action;
}
