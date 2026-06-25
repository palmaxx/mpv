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
#include <sys/stat.h>
#include <time.h>

#include <libplacebo/cache.h>
#include <libplacebo/filters.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/libav.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/io_utils.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "osdep/timer.h"
#include "stream/stream.h"
#include "sub/draw_bmp.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/utils.h"
#include "video/out/gpu/video.h"
#include "video/out/gpu/video_shaders.h"
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

static const struct m_opt_choice_alternatives lut_types[] = {
    {"auto",        PL_LUT_UNKNOWN},
    {"native",      PL_LUT_NATIVE},
    {"normalized",  PL_LUT_NORMALIZED},
    {"conversion",  PL_LUT_CONVERSION},
    {0}
};

#define OPT_BASE_STRUCT struct gl_next_opts
const struct m_sub_options gl_next_conf = {
    .opts = (const struct m_option[]) {
        {"sub-hdr-peak", OPT_CHOICE(sub_hdr_peak, {"sdr", PL_COLOR_SDR_WHITE}),
            M_RANGE(10, 10000)},
        {"image-subs-hdr-peak", OPT_CHOICE(image_subs_hdr_peak, {"sdr", PL_COLOR_SDR_WHITE},
            {"video", -1}, {"video-static", -2}, {"video-dynamic", -3}),  M_RANGE(10, 10000)},
        {"allow-delayed-peak-detect", OPT_BOOL(delayed_peak)},
        {"border-background", OPT_CHOICE(border_background,
            {"none",  BACKGROUND_NONE},
            {"color", BACKGROUND_COLOR},
            {"tiles", BACKGROUND_TILES}
            ,{"blur", BACKGROUND_BLUR})},
        {"background-blur-radius", OPT_FLOAT(background_blur_radius)},
        {"corner-rounding", OPT_FLOAT(corner_rounding), M_RANGE(0, 1)},
        {"interpolation-preserve", OPT_BOOL(inter_preserve)},
        {"lut", OPT_STRING(lut.opt), .flags = M_OPT_FILE},
        {"lut-type", OPT_CHOICE_C(lut.type, lut_types)},
        {"image-lut", OPT_STRING(image_lut.opt), .flags = M_OPT_FILE},
        {"image-lut-type", OPT_CHOICE_C(image_lut.type, lut_types)},
        {"target-lut", OPT_STRING(target_lut.opt), .flags = M_OPT_FILE},
        {"target-colorspace-hint", OPT_CHOICE(target_hint, {"auto", -1}, {"no", 0}, {"yes", 1})},
        {"target-colorspace-hint-mode", OPT_CHOICE(target_hint_mode, {"target", 0}, {"source", 1}, {"source-dynamic", 2})},
        {"target-colorspace-hint-strict", OPT_BOOL(target_hint_strict)},
        // No `target-lut-type` because we don't support non-RGB targets
        {"libplacebo-opts", OPT_KEYVALUELIST(raw_opts)},
        {0},
    },
    .defaults = &(struct gl_next_opts) {
        .border_background = BACKGROUND_COLOR,
        .background_blur_radius = 16.0f,
        .inter_preserve = true,
        .sub_hdr_peak = PL_COLOR_SDR_WHITE,
        .image_subs_hdr_peak = 1000,
        .target_hint = -1,
        .target_hint_strict = true,
    },
    .size = sizeof(struct gl_next_opts),
    .change_flags = UPDATE_VIDEO,
};
#undef OPT_BASE_STRUCT

struct scaler_params {
    struct pl_filter_config config;
};

struct user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct gpu_next_hwdec_state {
    struct ra_hwdec_mapper *mapper;
    struct timer_pool *timer;
    struct mp_pass_perf perf;
};

struct user_lut_cache_entry {
    char *path;
    struct pl_custom_lut *lut;
};
};

struct frame_info {
    int count;
    struct pl_dispatch_info info[VO_PASS_PERF_MAX];
};

// On-disk object cache for one libplacebo cache (the compiled-shader
// cache or the ICC-profile cache): dir is the cache directory, name the
// per-cache filename prefix, size_limit the cleanup threshold applied at
// teardown.
struct cache {
    struct mp_log *log;
    struct mpv_global *global;
    char *dir;
    const char *name;
    size_t size_limit;
    pl_cache cache;
};

struct gpu_next_core {
    pl_gpu gpu;
    pl_log pllog;
    struct mp_log *log;
    struct mpv_global *global;

    // On-disk libplacebo object caches. shader_cache is installed on the
    // gpu (pl_gpu_set_cache) so the renderer's compiled shaders persist
    // across runs; icc_cache backs pl_icc_params.cache. Both are created
    // in gpu_next_core_create from the gl_video_opts cache options (when
    // enabled) and cleaned up in gpu_next_core_destroy.
    struct cache shader_cache, icc_cache;

    // ICC profile state. icc_path tracks the manually-configured
    // --icc-profile (NULL = none / auto), icc_profile is the resolved
    // libplacebo object (cached in icc_cache) and icc_params the params
    // built by gpu_next_core_update_icc_opts.
    struct pl_icc_params icc_params;
    char *icc_path;
    pl_icc_object icc_profile;

    // Software-side colour adjustment: the equalizer state (brightness /
    // contrast / hue / saturation / gamma, watched off the option system)
    // and the output colour levels last resolved from it.
    struct mp_csp_equalizer_state *video_eq;
    enum pl_color_levels output_levels;

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
    struct gpu_next_hwdec_state hwdec;
    struct gpu_next_hwdec_state el_hwdec;

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

    // Active hook list handed to pl_render_params.hooks, rebuilt from
    // user_hooks by gpu_next_core_update_render_options on each options
    // update (a talloc child of the core; count is pars->params.num_hooks).
    const struct pl_hook **hooks;

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

    // OSD blended-subtitle sync counter, bumped by gpu_next_core_osd_changed
    // (a front-end OSD-layout change) and by every redrawn frame; compared
    // against frame_priv.osd_sync to decide whether a queued frame's cached
    // blended-subs overlay has gone stale and must be re-rendered.
    uint64_t osd_sync;

    // OSD source and overlay texture formats for gpu_next_core_update_overlays.
    // osd is the front-end's osd_state, installed with gpu_next_core_set_osd;
    // osd_fmt[] is resolved from the gpu in gpu_next_core_create.
    struct osd_state *osd;
    pl_fmt osd_fmt[SUBBITMAP_COUNT];

    // Front-end interface (see gpu_next_core_set_frontend), reached by the
    // core's frame-ingest callbacks for genuinely front-end-specific
    // resources (opts, ra, instrumentation, hwdec lookup).
    struct gpu_next_core_frontend fe;

    // Resolved image LUT, set by the front-end's options-update path
    // (gpu_next_core_set_image_lut) and mapped onto each source frame.
    struct pl_custom_lut *image_lut;
    enum pl_lut_type image_lut_type;
};

struct frame_priv {
    struct gpu_next_core *core;
    struct ra_hwdec *hwdec;
    struct gpu_next_osd_state subs;
    uint64_t osd_sync;
};

static bool gpu_next_core_map_frame(pl_gpu gpu, pl_tex *tex,
                                    const struct pl_source_frame *src,
                                    struct pl_frame *frame);
static void gpu_next_core_unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                                      const struct pl_source_frame *src);
static void gpu_next_core_discard_frame(const struct pl_source_frame *src);

static char *cache_filepath(void *ta_ctx, char *dir, const char *prefix, uint64_t key)
{
    bstr filename = {0};
    bstr_xappend_asprintf(ta_ctx, &filename, "%s_%016" PRIx64, prefix, key);
    return mp_path_join_bstr(ta_ctx, bstr0(dir), filename);
}

static pl_cache_obj cache_load_obj(void *p, uint64_t key)
{
    struct cache *c = p;
    void *ta_ctx = talloc_new(NULL);
    pl_cache_obj obj = {0};

    if (!c->dir)
        goto done;

    char *filepath = cache_filepath(ta_ctx, c->dir, c->name, key);
    if (!filepath)
        goto done;

    if (stat(filepath, &(struct stat){0}))
        goto done;

    int64_t load_start = mp_time_ns();
    struct bstr data = stream_read_file(filepath, ta_ctx, c->global, STREAM_MAX_READ_SIZE);
    int64_t load_end = mp_time_ns();
    MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu), load time(%.3f ms)\n",
           __func__, key, data.len,
           MP_TIME_NS_TO_MS(load_end - load_start));

    obj = (pl_cache_obj){
        .key = key,
        .data = talloc_steal(NULL, data.start),
        .size = data.len,
        .free = talloc_free,
    };

done:
    talloc_free(ta_ctx);
    return obj;
}

static void cache_save_obj(void *p, pl_cache_obj obj)
{
    const struct cache *c = p;
    void *ta_ctx = talloc_new(NULL);

    if (!c->dir)
        goto done;

    char *filepath = cache_filepath(ta_ctx, c->dir, c->name, obj.key);
    if (!filepath)
        goto done;

    if (!obj.data || !obj.size) {
        unlink(filepath);
        goto done;
    }

    // Don't save if already exists
    struct stat st;
    if (!stat(filepath, &st) && st.st_size == obj.size) {
        MP_DBG(c, "%s: key(%"PRIx64"), size(%zu)\n", __func__, obj.key, obj.size);
        goto done;
    }

    int64_t save_start = mp_time_ns();
    mp_save_to_file(filepath, obj.data, obj.size);
    int64_t save_end = mp_time_ns();
    MP_DBG(c, "%s: key(%" PRIx64 "), size(%zu), save time(%.3f ms)\n",
           __func__, obj.key, obj.size,
           MP_TIME_NS_TO_MS(save_end - save_start));

done:
    talloc_free(ta_ctx);
}

static void cache_init(struct gpu_next_core *core, struct cache *cache,
                       const char *dir_opt)
{
    const char *name = cache == &core->shader_cache ? "shader" : "icc";
    const size_t limit = cache == &core->shader_cache ? 128 << 20 : 1536 << 20;

    char *dir;
    if (dir_opt && dir_opt[0]) {
        dir = mp_get_user_path(core, core->global, dir_opt);
    } else {
        dir = mp_find_user_file(core, core->global, "cache", "");
    }
    if (!dir || !dir[0])
        return;

    mp_mkdirp(dir);
    *cache = (struct cache){
        .log        = core->log,
        .global     = core->global,
        .dir        = dir,
        .name       = name,
        .size_limit = limit,
        .cache = pl_cache_create(pl_cache_params(
            .log = core->pllog,
            .get = cache_load_obj,
            .set = cache_save_obj,
            .priv = cache
        )),
    };
}

struct file_entry {
    char *filepath;
    size_t size;
    time_t atime;
};

static int compare_atime(const void *a, const void *b)
{
    return (((struct file_entry *)b)->atime - ((struct file_entry *)a)->atime);
}

static void cache_uninit(struct cache *cache)
{
    if (!cache->cache)
        return;

    void *ta_ctx = talloc_new(NULL);
    struct file_entry *files = NULL;
    size_t num_files = 0;
    mp_assert(cache->dir);
    mp_assert(cache->name);

    DIR *d = opendir(cache->dir);
    if (!d)
        goto done;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        char *filepath = mp_path_join(ta_ctx, cache->dir, dir->d_name);
        if (!filepath)
            continue;
        struct stat filestat;
        if (stat(filepath, &filestat))
            continue;
        if (!S_ISREG(filestat.st_mode))
            continue;
        bstr fname = bstr0(dir->d_name);
        if (!bstr_eatstart0(&fname, cache->name))
            continue;
        if (!bstr_eatstart0(&fname, "_"))
            continue;
        if (fname.len != 16) // %016x
            continue;
        MP_TARRAY_APPEND(ta_ctx, files, num_files,
                         (struct file_entry){
                             .filepath = filepath,
                             .size     = filestat.st_size,
                             .atime    = filestat.st_atime,
                         });
    }
    closedir(d);

    if (!num_files)
        goto done;

    qsort(files, num_files, sizeof(struct file_entry), compare_atime);

    time_t t = time(NULL);
    size_t cache_size = 0;
    size_t cache_limit = cache->size_limit ? cache->size_limit : SIZE_MAX;
    for (int i = 0; i < num_files; i++) {
        // Remove files that exceed the size limit but are older than one day.
        // This allows for temporary maintaining a larger cache size while
        // adjusting the configuration. The cache will be cleared the next day
        // for unused entries. We don't need to be overly aggressive with cache
        // cleaning; in most cases, it will not grow much, and in others, it may
        // actually be useful to cache more.
        cache_size += files[i].size;
        double rel_use = difftime(t, files[i].atime);
        if (cache_size > cache_limit && rel_use > 60 * 60 * 24) {
            MP_VERBOSE(cache, "Removing %s | size: %9zu bytes | last used: %9d seconds ago\n",
                       files[i].filepath, files[i].size, (int)rel_use);
            unlink(files[i].filepath);
        }
    }

done:
    talloc_free(ta_ctx);
    pl_cache_destroy(&cache->cache);
}

struct gpu_next_core *gpu_next_core_create(pl_gpu gpu, struct mp_log *log,
                                           pl_log pllog,
                                           struct mpv_global *global,
                                           const struct gl_video_opts *opts)
{
    struct gpu_next_core *core = talloc_zero(NULL, struct gpu_next_core);
    core->gpu = gpu;
    core->pllog = pllog;
    core->log = log;
    core->global = global;
    core->video_eq = mp_csp_equalizer_create(core, global);
    core->pars = pl_options_alloc(pllog);

    // Set up the on-disk object caches and install the shader cache on the
    // gpu before creating the renderer, mirroring the windowed VO's preinit.
    if (opts) {
        if (opts->shader_cache)
            cache_init(core, &core->shader_cache, opts->shader_cache_dir);
        if (opts->icc_opts->cache)
            cache_init(core, &core->icc_cache, opts->icc_opts->cache_dir);
    }
    pl_gpu_set_cache(gpu, core->shader_cache.cache);

    core->rr = pl_renderer_create(pllog, gpu);
    core->queue = pl_queue_create(gpu);
    core->osd_sync = 1;
    core->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(gpu, "r8");
    core->osd_fmt[SUBBITMAP_BGRA] = pl_find_named_fmt(gpu, "bgra8");
    mp_mutex_init(&core->dr_lock);
    return core;
}

pl_options gpu_next_core_options(struct gpu_next_core *core)
{
    return core->pars;
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

void gpu_next_core_set_osd(struct gpu_next_core *core, struct osd_state *osd)
{
    core->osd = osd;
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

static bool gpu_next_core_render_image(struct gpu_next_core *core,
                                       const struct pl_frame *image,
                                       const struct pl_frame *target,
                                       const struct pl_render_params *params)
{
    return pl_render_image(core->rr, image, target, params);
}

void gpu_next_core_screenshot(struct gpu_next_core *core,
                              struct voctrl_screenshot *args,
                              struct mp_rect src, struct mp_rect dst,
                              struct mp_osd_res osd_res,
                              int fallback_depth, bool want_alpha,
                              struct gpu_next_osd_state *osd_state)
{
    const struct gl_video_opts *opts = core->fe.opts;
    pl_options pars = core->pars;
    pl_gpu gpu = core->gpu;
    pl_tex fbo = NULL;
    args->res = NULL;

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
        .pts = gpu_next_core_queue_last_pts(core),
        .drift_compensation = 0,
    );
    status = pl_queue_update(core->queue, &mix, &qparams);
    mp_assert(status != PL_QUEUE_EOF);
    if (status == PL_QUEUE_ERR) {
        MP_ERR(core, "Unknown error occurred while trying to take screenshot!\n");
        return;
    }
    if (!mix.num_frames) {
        MP_ERR(core, "No frames available to take screenshot of, is a file loaded?\n");
        return;
    }

    // Passing an interpolation radius of 0 guarantees that the first frame in
    // the resulting mix is the correct frame for this PTS
    struct pl_frame image = *(struct pl_frame *) mix.frames[0];
    struct mp_image *mpi = image.user_data;
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

        osd_res = (struct mp_osd_res) {
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
            mpfmt = want_alpha ? IMGFMT_RGBA : IMGFMT_RGB0;
        }
        pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, depth, depth,
                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
        if (!fmt)
            continue;

        fbo = pl_tex_create(gpu, pl_tex_params(
            .w = osd_res.w,
            .h = osd_res.h,
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
        MP_ERR(core, "Failed creating target FBO for screenshot!\n");
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

    if (args->scaled) {
        // Apply target LUT, ICC profile and CSP override only in window mode
        gpu_next_core_apply_target_options(core, &target, 0, false,
                                           fallback_depth);
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
    gpu_next_core_update_tm_viz(core, &target);

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
            .mr = (image.crop.x1 - mpi->params.w) * rx,
            .mt = -image.crop.y0 * ry,
            .mb = (image.crop.y1 - mpi->params.h) * ry,
            .display_par = 1.0,
        };
        enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
            ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
        gpu_next_core_update_overlays(core, res, osd_flags,
                                     rel, &fp->subs, &image, mpi,
                                     mpi->params.stereo3d);
    } else {
        // Disable overlays when blend_subs is disabled
        gpu_next_core_update_overlays(core, osd_res, osd_flags,
                                     PL_OVERLAY_COORDS_DST_FRAME,
                                     osd_state, &target, mpi,
                                     mpi->params.stereo3d);
        image.num_overlays = 0;
    }

    if (!gpu_next_core_render_image(core, &image, &target, &params)) {
        MP_ERR(core, "Failed rendering frame!\n");
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

    if (!ok) {
        talloc_free(args->res);
        args->res = NULL;
    }

    // fall through
done:
    pl_tex_destroy(gpu, &fbo);
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
                          &core->hwdec.perf, &core->sw_upload_perf);
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

void gpu_next_core_queue_push(struct gpu_next_core *core,
                              struct mp_image *src, double duration)
{
    struct mp_image *mpi = mp_image_new_ref(src);
    struct frame_priv *fp = talloc_zero(mpi, struct frame_priv);
    mpi->priv = fp;
    fp->core = core;

    pl_queue_push(core->queue, &(struct pl_source_frame) {
        .pts = mpi->pts,
        .duration = duration,
        .frame_data = mpi,
        .map = gpu_next_core_map_frame,
        .unmap = gpu_next_core_unmap_frame,
        .discard = gpu_next_core_discard_frame,
    });
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

    ra_hwdec_mapper_free(&core->hwdec.mapper);
    timer_pool_destroy(core->hwdec.timer);
    core->hwdec.timer = NULL;
    ra_hwdec_mapper_free(&core->el_hwdec.mapper);
    timer_pool_destroy(core->el_hwdec.timer);
    core->el_hwdec.timer = NULL;
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

    // Trim the on-disk caches and destroy the pl_cache objects. The gpu
    // still references the (now-destroyed) shader cache, but nothing
    // dispatches between here and the front-end destroying the gpu, so
    // the dangling pointer is never read -- the same window mainline's
    // uninit relied on.
    cache_uninit(&core->shader_cache);
    cache_uninit(&core->icc_cache);
    pl_icc_close(&core->icc_profile);

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

static int gpu_next_core_plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                                struct pl_bit_encoding *out_bits,
                                                enum mp_imgfmt imgfmt,
                                                bool use_uint)
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

static bool gpu_next_core_upload_sw_planes(struct gpu_next_core *core,
                                           struct ra *ra,
                                           struct mp_image *mpi,
                                           pl_tex *tex,
                                           struct pl_frame *frame)
{
    pl_gpu gpu = core->gpu;

    // Front-ends without an ra_ctx (the libmpv render backend) cannot
    // create a timer_pool, so the SW-upload sample stays at zero there.
    if (ra && !core->sw_upload_timer)
        core->sw_upload_timer = timer_pool_create(ra);
    if (core->sw_upload_timer)
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
            if (core->sw_upload_timer)
                timer_pool_stop(core->sw_upload_timer);
            return false;
        }

        // Without async callback support, we have to poll...
        if (!gpu->limits.callbacks && data[n].buf)
            while (pl_buf_poll(gpu, data[n].buf, UINT64_MAX));
    }

    if (core->sw_upload_timer) {
        timer_pool_stop(core->sw_upload_timer);
        core->sw_upload_perf = timer_pool_measure(core->sw_upload_timer);
    }
    return true;
}

static void gpu_next_core_sw_upload_perf_reset(struct gpu_next_core *core)
{
    core->sw_upload_perf.count = 0;
}

static bool gpu_next_core_hwdec_reconfig(struct gpu_next_core *core,
                                           struct ra *ra,
                                           struct ra_hwdec *hwdec,
                                           const struct mp_image_params *par)
{
    return hwdec_reconfig(core, &core->hwdec, ra, hwdec, par);
}

static bool gpu_next_core_hwdec_el_reconfig(struct gpu_next_core *core, struct ra *ra,
                                            struct ra_hwdec *hwdec,
                                            const struct mp_image_params *par)
{
    return hwdec_reconfig(core, &core->el_hwdec, ra, hwdec, par);
}

static const struct mp_image_params *hwdec_dst_params(
    const struct gpu_next_hwdec_state *state)
{
    return &state->mapper->dst_params;
}

static const struct mp_image_params *gpu_next_core_hwdec_dst_params(
    const struct gpu_next_core *core)
{
    return hwdec_dst_params(&core->hwdec);
}

static const struct mp_image_params *gpu_next_core_hwdec_el_dst_params(
    const struct gpu_next_core *core)
{
    return hwdec_dst_params(&core->el_hwdec);
}

// For RAs not based on ra_pl, this creates a new pl_tex wrapper.
static pl_tex hwdec_plane_tex(struct gpu_next_core *core,
                              struct gpu_next_hwdec_state *state, int n)
{
    struct ra_tex *ratex = state->mapper->tex[n];
    struct ra *ra = state->mapper->ra;
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

static bool hwdec_acquire(struct gpu_next_core *core,
                          struct gpu_next_hwdec_state *state,
                          struct mp_image *mpi, struct pl_frame *frame,
                          const char *map_err)
{
    timer_pool_start(state->timer);
    if (ra_hwdec_mapper_map(state->mapper, mpi) < 0) {
        MP_ERR(core, "%s\n", map_err);
        timer_pool_stop(state->timer);
        return false;
    }

    for (int n = 0; n < frame->num_planes; n++) {
        if (!(frame->planes[n].texture = hwdec_plane_tex(core, state, n))) {
            if (!ra_pl_get(state->mapper->ra)) {
                for (int i = 0; i < n; i++)
                    pl_tex_destroy(core->gpu, &frame->planes[i].texture);
            }
            for (int i = 0; i < frame->num_planes; i++)
                frame->planes[i].texture = NULL;
            ra_hwdec_mapper_unmap(state->mapper);
            timer_pool_stop(state->timer);
            return false;
        }
    }

    timer_pool_stop(state->timer);
    state->perf = timer_pool_measure(state->timer);
    return true;
}

static bool gpu_next_core_hwdec_acquire(struct gpu_next_core *core,
                                          struct mp_image *mpi,
                                          struct pl_frame *frame)
{
    return hwdec_acquire(core, &core->hwdec, mpi, frame,
                         "Mapping hardware decoded surface failed.");
}

static bool gpu_next_core_hwdec_el_acquire(struct gpu_next_core *core,
                                             struct mp_image *mpi,
                                             struct pl_frame *frame)
{
    return hwdec_acquire(core, &core->el_hwdec, mpi, frame,
                         "Mapping enhancement-layer hwdec surface failed.");
}

static void hwdec_release(struct gpu_next_core *core,
                          struct gpu_next_hwdec_state *state,
                          struct pl_frame *frame)
{
    if (!ra_pl_get(state->mapper->ra)) {
        for (int n = 0; n < frame->num_planes; n++)
            pl_tex_destroy(core->gpu, &frame->planes[n].texture);
    }

    ra_hwdec_mapper_unmap(state->mapper);
}

static void gpu_next_core_hwdec_release(struct gpu_next_core *core,
                                         struct pl_frame *frame)
{
    hwdec_release(core, &core->hwdec, frame);
}

static void gpu_next_core_hwdec_el_release(struct gpu_next_core *core,
                                           struct pl_frame *frame)
{
    hwdec_release(core, &core->el_hwdec, frame);
}

static void setup_hwdec_plane_mapping(struct pl_frame *frame,
                                      const struct mp_imgfmt_desc *desc)
{
    frame->num_planes = desc->num_planes;
    for (int n = 0; n < frame->num_planes; n++) {
        struct pl_plane *plane = &frame->planes[n];
        int *map = plane->component_mapping;
        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(desc); c++) {
            if (desc->comps[c].plane != n)
                continue;
            // Sort by component offset
            uint8_t offset = desc->comps[c].offset;
            int index = plane->components++;
            while (index > 0 && desc->comps[map[index - 1]].offset > offset) {
                map[index] = map[index - 1];
                index--;
            }
            map[index] = c;
        }
    }
}

static void gpu_next_core_hwdec_perf_reset(struct gpu_next_core *core)
{
    core->hwdec.perf.count = 0;
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

static const struct pl_hook *gpu_next_core_load_hook(struct gpu_next_core *core,
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

static struct pl_custom_lut *gpu_next_core_load_lut(struct gpu_next_core *core,
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

static void gpu_next_core_sub_tex_push(struct gpu_next_core *core, pl_tex tex)
{
    if (!tex)
        return;
    MP_TARRAY_APPEND(core, core->sub_tex, core->num_sub_tex, tex);
}

static pl_tex gpu_next_core_sub_tex_pop(struct gpu_next_core *core)
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

static bool gpu_next_core_map_frame(pl_gpu gpu, pl_tex *tex,
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
        setup_hwdec_plane_mapping(frame, &desc);
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

#if PL_API_VER >= 367
    if (mpi->enhancement_layer) {
        struct mp_image *el = mpi->enhancement_layer;
        fp->el_hwdec = core->fe.hwdec_get
            ? core->fe.hwdec_get(core->fe.hwdec_ctx, el->imgfmt)
            : NULL;

        struct mp_image_params el_par = el->params;
        bool el_ok = true;
        if (fp->el_hwdec) {
            if (gpu_next_core_hwdec_el_reconfig(core, core->fe.ra,
                                                fp->el_hwdec, &el->params)) {
                el_par = *gpu_next_core_hwdec_el_dst_params(core);
            } else {
                fp->el_hwdec = NULL;
                el_ok = false;
            }
        }
        mp_image_params_guess_csp(&el_par);

        fp->el_frame = (struct pl_frame) {
            .color = el_par.color,
            .repr  = el_par.repr,
            .user_data = mpi, // BL mpi
        };

        if (el_ok && fp->el_hwdec) {
            struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(el_par.imgfmt);
            fp->el_frame.acquire = hwdec_acquire_el;
            fp->el_frame.release = hwdec_release_el;
            setup_hwdec_plane_mapping(&fp->el_frame, &desc);
        } else if (el_ok) {
            el_ok = gpu_next_core_upload_sw_planes(core, core->fe.ra, el,
                                                   fp->el_tex, &fp->el_frame);
        }

        if (el_ok) {
            pl_frame_set_chroma_location(&fp->el_frame, el_par.chroma_location);
            frame->enhancement_layer = &fp->el_frame;
        } else {
            MP_WARN(core, "Failed setting up enhancement layer; "
                    "rendering base layer only.\n");
        }
    }
#endif

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

static void gpu_next_core_unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                                      const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    for (int i = 0; i < MP_ARRAY_SIZE(fp->subs.entries); i++)
        gpu_next_core_sub_tex_push(fp->core, fp->subs.entries[i].tex);
    for (int i = 0; i < MP_ARRAY_SIZE(fp->el_tex); i++) {
        if (fp->el_tex[i])
            pl_tex_destroy(gpu, &fp->el_tex[i]);
    }
    talloc_free(mpi);
}

static void gpu_next_core_discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

void gpu_next_core_update_overlays(struct gpu_next_core *core,
                                   struct mp_osd_res res, int flags,
                                   enum pl_overlay_coords coords,
                                   struct gpu_next_osd_state *state,
                                   struct pl_frame *frame,
                                   struct mp_image *src, int stereo_mode)
{
    frame->overlays = state->overlays;
    frame->num_overlays = 0;
    if (!core->osd)
        return;

    const struct gl_next_opts *next_opts = core->fe.next_opts;
    double pts = src ? src->pts : 0;
    int div[2];
    mp_get_3d_side_by_side(stereo_mode, div);
    res.w /= div[0];
    res.h /= div[1];
    struct sub_bitmap_list *subs = osd_render(core->osd, res, pts, flags,
                                              mp_draw_sub_formats);

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;
        struct gpu_next_osd_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = core->osd_fmt[item->format];
        if (!entry->tex)
            entry->tex = gpu_next_core_sub_tex_pop(core);
        bool ok = pl_tex_recreate(core->gpu, &entry->tex, &(struct pl_tex_params) {
            .format = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable = true,
        });
        if (!ok) {
            MP_ERR(core, "Failed recreating OSD texture!\n");
            break;
        }
        struct pl_tex_transfer_params upload_params = {
            .tex        = entry->tex,
            .rc         = { .x1 = item->packed_w, .y1 = item->packed_h, },
            .row_pitch  = item->packed->stride[0],
            .ptr        = item->packed->planes[0],
        };
        // Keep the image alive until it's fully read.
        if (core->gpu->limits.callbacks) {
            upload_params.callback = talloc_free;
            upload_params.priv = mp_image_new_ref(item->packed);
        }
        ok = pl_tex_upload(core->gpu, &upload_params);
        if (!ok) {
            MP_ERR(core, "Failed uploading OSD texture!\n");
            talloc_free(upload_params.priv);
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8) & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                }
            };
            MP_TARRAY_APPEND(core, entry->parts, entry->num_parts, part);
        }

        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = pl_color_space_srgb,
            .coords = coords,
        };

        switch (item->format) {
        case SUBBITMAP_BGRA:
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            // Infer bitmap colorspace from source
            if (src) {
                ol->color = src->params.color;
                if (pl_color_transfer_is_hdr(ol->color.transfer)) {
                    bool use_static = next_opts->image_subs_hdr_peak == -2;
                    if (use_static || next_opts->image_subs_hdr_peak == -3) {
                        float max;
                        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                            .color      = &ol->color,
                            .metadata   = use_static ? PL_HDR_METADATA_HDR10 : PL_HDR_METADATA_ANY,
                            .scaling    = PL_HDR_NITS,
                            .out_max    = &max,
                        ));
                        ol->color.hdr = (struct pl_hdr_metadata) {
                            .max_luma = max,
                        };
                    } else if (next_opts->image_subs_hdr_peak != -1) {
                        ol->color.hdr = (struct pl_hdr_metadata) {
                            .max_luma = next_opts->image_subs_hdr_peak,
                        };
                    }
                }
            }
            break;
        case SUBBITMAP_LIBASS:
            if (src && item->video_color_space && !pl_color_space_is_hdr(&src->params.color))
                ol->color = src->params.color;
            if (src && pl_color_transfer_is_hdr(frame->color.transfer)) {
                ol->color.hdr = (struct pl_hdr_metadata) {
                    .max_luma = next_opts->sub_hdr_peak,
                };
            }
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            break;
        }

        // Duplicate overlay parts for each eye in stereo 3D modes
        if (div[0] > 1 || div[1] > 1) {
            int orig_num = entry->num_parts;
            for (int x = 0; x < div[0]; x++) {
                for (int y = 0; y < div[1]; y++) {
                    if (x == 0 && y == 0)
                        continue;
                    float off_x = res.w * x;
                    float off_y = res.h * y;
                    for (int i = 0; i < orig_num; i++) {
                        struct pl_overlay_part duped = entry->parts[i];
                        duped.dst.x0 += off_x;
                        duped.dst.x1 += off_x;
                        duped.dst.y0 += off_y;
                        duped.dst.y1 += off_y;
                        MP_TARRAY_APPEND(core, entry->parts, entry->num_parts, duped);
                    }
                }
            }
            ol->parts = entry->parts;
            ol->num_parts = entry->num_parts;
        }
    }

    talloc_free(subs);
}

void gpu_next_core_osd_changed(struct gpu_next_core *core)
{
    core->osd_sync++;
}

void gpu_next_core_update_frames(struct gpu_next_core *core,
                                 const struct pl_frame_mix *mix,
                                 const struct pl_frame *target,
                                 struct mp_rect src_crop,
                                 int src_width, int src_height, bool redraw)
{
    const struct gl_video_opts *opts = core->fe.opts;

    // Update source crop and overlays on all existing frames. We
    // technically own the `pl_frame` struct so this is kosher. This could
    // be partially avoided by instead flushing the queue on resizes, but
    // doing it this way avoids unnecessarily re-uploading frames.
    for (int i = 0; i < mix->num_frames; i++) {
        struct pl_frame *image = (struct pl_frame *) mix->frames[i];
        struct mp_image *mpi = image->user_data;
        struct frame_priv *fp = mpi->priv;
        gpu_next_core_apply_crop(image, src_crop, src_width, src_height);
        if (opts->blend_subs) {
            if (redraw)
                core->osd_sync++;
            if (fp->osd_sync < core->osd_sync) {
                float w = pl_rect_w(opts->blend_subs == BLEND_SUBS_VIDEO ? image->crop : target->crop);
                float h = pl_rect_h(opts->blend_subs == BLEND_SUBS_VIDEO ? image->crop : target->crop);
                float rx = w / pl_rect_w(image->crop);
                float ry = h / pl_rect_h(image->crop);
                struct mp_osd_res res = {
                    .w = w,
                    .h = h,
                    .ml = -image->crop.x0 * rx,
                    .mr = (image->crop.x1 - src_width) * rx,
                    .mt = -image->crop.y0 * ry,
                    .mb = (image->crop.y1 - src_height) * ry,
                    .display_par = 1.0,
                };
                enum pl_overlay_coords rel = opts->blend_subs == BLEND_SUBS_VIDEO
                    ? PL_OVERLAY_COORDS_SRC_CROP : PL_OVERLAY_COORDS_DST_CROP;
                if (core->fe.timer_start)
                    core->fe.timer_start(core->fe.timer_ctx, "osd-blend-update");
                gpu_next_core_update_overlays(core, res, OSD_DRAW_SUB_ONLY, rel,
                                              &fp->subs, image, mpi,
                                              mpi->params.stereo3d);
                if (core->fe.timer_end)
                    core->fe.timer_end(core->fe.timer_ctx, "osd-blend-update");
                fp->osd_sync = core->osd_sync;
            }
        } else {
            // Disable overlays when blend_subs is disabled
            image->num_overlays = 0;
            fp->osd_sync = 0;
        }

        // Update the frame signature to include the current OSD sync
        // value, in order to disambiguate between identical frames with
        // modified OSD. Shift the OSD sync value by a lot to avoid
        // collisions with low signature values.
        //
        // This is safe to do because `pl_frame_mix.signature` lives in
        // temporary memory that is only valid for this `pl_queue_update`.
        ((uint64_t *) mix->signatures)[i] ^= fp->osd_sync << 48;
    }
}

static void update_hook_opts(struct gpu_next_core *core, char **opts,
                             const char *shaderpath, const struct pl_hook *hook)
{
    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *hp = &hook->parameters[i];
        memcpy(hp->data, &hp->initial, sizeof(*hp->data));
    }

    if (!opts)
        return;

    const char *basename = mp_basename(shaderpath);
    struct bstr shadername;
    if (!mp_splitext(basename, &shadername))
        shadername = bstr0(basename);

    for (int n = 0; opts[n * 2]; n++) {
        struct bstr k = bstr0(opts[n * 2 + 0]);
        struct bstr v = bstr0(opts[n * 2 + 1]);
        int pos;
        if ((pos = bstrchr(k, '/')) >= 0) {
            if (!bstr_equals(bstr_splice(k, 0, pos), shadername))
                continue;
            k = bstr_cut(k, pos + 1);
        }

        for (int i = 0; i < hook->num_parameters; i++) {
            const struct pl_hook_par *hp = &hook->parameters[i];
            if (!bstr_equals0(k, hp->name) != 0)
                continue;

            m_option_t opt = {
                .name = hp->name,
            };

            if (hp->names) {
                for (int j = hp->minimum.i; j <= hp->maximum.i; j++) {
                    if (bstr_equals0(v, hp->names[j])) {
                        hp->data->i = j;
                        goto next_hook;
                    }
                }
            }

            switch (hp->type) {
            case PL_VAR_FLOAT:
                opt.type = &m_option_type_float;
                opt.min = hp->minimum.f;
                opt.max = hp->maximum.f;
                break;
            case PL_VAR_SINT:
                opt.type = &m_option_type_int;
                opt.min = hp->minimum.i;
                opt.max = hp->maximum.i;
                break;
            case PL_VAR_UINT:
                opt.type = &m_option_type_int;
                opt.min = MPMIN(hp->minimum.u, INT_MAX);
                opt.max = MPMIN(hp->maximum.u, INT_MAX);
                break;
            }

            if (!opt.type)
                goto next_hook;

            opt.type->parse(core->log, &opt, k, v, hp->data);
            goto next_hook;
        }

    next_hook:;
    }
}

void gpu_next_core_update_render_options(struct gpu_next_core *core,
                                         const struct gl_video_opts *opts,
                                         const struct gl_next_opts *next_opts,
                                         bool paused)
{
    pl_options pars = core->pars;
    pars->params.background_color[0] = opts->background_color.r / 255.0;
    pars->params.background_color[1] = opts->background_color.g / 255.0;
    pars->params.background_color[2] = opts->background_color.b / 255.0;
    pars->params.background_transparency = 1 - opts->background_color.a / 255.0;
    pars->params.skip_anti_aliasing = !opts->correct_downscaling;
    pars->params.disable_linear_scaling = !opts->linear_downscaling && !opts->linear_upscaling;
    pars->params.disable_fbos = opts->dumb_mode == 1;

    static const int map_background_types[] = {
        [BACKGROUND_NONE]  = PL_CLEAR_SKIP,
        [BACKGROUND_COLOR] = PL_CLEAR_COLOR,
        [BACKGROUND_TILES] = PL_CLEAR_TILES,
        [BACKGROUND_BLUR]  = PL_CLEAR_BLUR,
    };
    pars->params.background = map_background_types[opts->background];
    pars->params.border = map_background_types[next_opts->border_background];
    pars->params.blur_radius = next_opts->background_blur_radius;
    pars->params.tile_size = opts->background_tile_size * 2;
    for (int i = 0; i < 2; ++i) {
        pars->params.tile_colors[i][0] = opts->background_tile_color[i].r / 255.0f;
        pars->params.tile_colors[i][1] = opts->background_tile_color[i].g / 255.0f;
        pars->params.tile_colors[i][2] = opts->background_tile_color[i].b / 255.0f;
    }

    pars->params.corner_rounding = next_opts->corner_rounding;
    pars->params.correct_subpixel_offsets = !opts->scaler_resizes_only;

    // Map scaler options as best we can
    pars->params.upscaler = gpu_next_core_map_scaler(core, opts, SCALER_SCALE);
    pars->params.downscaler = gpu_next_core_map_scaler(core, opts, SCALER_DSCALE);
    pars->params.plane_upscaler = gpu_next_core_map_scaler(core, opts, SCALER_CSCALE);
    pars->params.frame_mixer = opts->interpolation ?
        gpu_next_core_map_scaler(core, opts, SCALER_TSCALE) : NULL;

    pars->params.deband_params = opts->deband ? &pars->deband_params : NULL;
    pars->deband_params.iterations = opts->deband_opts->iterations;
    pars->deband_params.radius = opts->deband_opts->range;
    pars->deband_params.threshold = opts->deband_opts->threshold / 16.384;
    pars->deband_params.grain = opts->deband_opts->grain / 8.192;

    pars->params.sigmoid_params = opts->sigmoid_upscaling ? &pars->sigmoid_params : NULL;
    pars->sigmoid_params.center = opts->sigmoid_center;
    pars->sigmoid_params.slope = opts->sigmoid_slope;

    pars->params.peak_detect_params = opts->tone_map.compute_peak >= 0 ? &pars->peak_detect_params : NULL;
    pars->peak_detect_params.smoothing_period = opts->tone_map.decay_rate;
    pars->peak_detect_params.scene_threshold_low = opts->tone_map.scene_threshold_low;
    pars->peak_detect_params.scene_threshold_high = opts->tone_map.scene_threshold_high;
    pars->peak_detect_params.percentile = opts->tone_map.peak_percentile;
    pars->peak_detect_params.allow_delayed = next_opts->delayed_peak;

    const struct pl_tone_map_function * const tone_map_funs[] = {
        [TONE_MAPPING_AUTO]     = &pl_tone_map_auto,
        [TONE_MAPPING_CLIP]     = &pl_tone_map_clip,
        [TONE_MAPPING_MOBIUS]   = &pl_tone_map_mobius,
        [TONE_MAPPING_REINHARD] = &pl_tone_map_reinhard,
        [TONE_MAPPING_HABLE]    = &pl_tone_map_hable,
        [TONE_MAPPING_GAMMA]    = &pl_tone_map_gamma,
        [TONE_MAPPING_LINEAR]   = &pl_tone_map_linear,
        [TONE_MAPPING_SPLINE]   = &pl_tone_map_spline,
        [TONE_MAPPING_BT_2390]  = &pl_tone_map_bt2390,
        [TONE_MAPPING_BT_2446A] = &pl_tone_map_bt2446a,
        [TONE_MAPPING_ST2094_40] = &pl_tone_map_st2094_40,
        [TONE_MAPPING_ST2094_10] = &pl_tone_map_st2094_10,
    };

    const struct pl_gamut_map_function * const gamut_modes[] = {
        [GAMUT_AUTO]            = pl_color_map_default_params.gamut_mapping,
        [GAMUT_CLIP]            = &pl_gamut_map_clip,
        [GAMUT_PERCEPTUAL]      = &pl_gamut_map_perceptual,
        [GAMUT_RELATIVE]        = &pl_gamut_map_relative,
        [GAMUT_SATURATION]      = &pl_gamut_map_saturation,
        [GAMUT_ABSOLUTE]        = &pl_gamut_map_absolute,
        [GAMUT_DESATURATE]      = &pl_gamut_map_desaturate,
        [GAMUT_DARKEN]          = &pl_gamut_map_darken,
        [GAMUT_WARN]            = &pl_gamut_map_highlight,
        [GAMUT_LINEAR]          = &pl_gamut_map_linear,
    };

    pars->color_map_params.tone_mapping_function = tone_map_funs[opts->tone_map.curve];
AV_NOWARN_DEPRECATED(
    pars->color_map_params.tone_mapping_param = opts->tone_map.curve_param;
    if (isnan(pars->color_map_params.tone_mapping_param)) // vo_gpu compatibility
        pars->color_map_params.tone_mapping_param = 0.0;
)
    pars->color_map_params.inverse_tone_mapping = opts->tone_map.inverse;
    pars->color_map_params.contrast_recovery = opts->tone_map.contrast_recovery;
    pars->color_map_params.visualize_lut = opts->tone_map.visualize;
    pars->color_map_params.contrast_smoothness = opts->tone_map.contrast_smoothness;
    pars->color_map_params.gamut_mapping = gamut_modes[opts->tone_map.gamut_mode];

    pars->params.dither_params = NULL;
    pars->params.error_diffusion = NULL;

    switch (opts->dither_algo) {
    case DITHER_ERROR_DIFFUSION:
        pars->params.error_diffusion = pl_find_error_diffusion_kernel(opts->error_diffusion);
        if (!pars->params.error_diffusion) {
            MP_WARN(core, "Could not find error diffusion kernel '%s', falling "
                    "back to fruit.\n", opts->error_diffusion);
        }
        MP_FALLTHROUGH;
    case DITHER_ORDERED:
    case DITHER_FRUIT:
        pars->params.dither_params = &pars->dither_params;
        pars->dither_params.method = opts->dither_algo == DITHER_ORDERED
                                ? PL_DITHER_ORDERED_FIXED
                                : PL_DITHER_BLUE_NOISE;
        pars->dither_params.lut_size = opts->dither_size;
        pars->dither_params.temporal = opts->temporal_dither;
        break;
    }

    if (opts->dither_depth < 0) {
        pars->params.dither_params = NULL;
        pars->params.error_diffusion = NULL;
    }

    gpu_next_core_update_icc_opts(core, opts->icc_opts);

    pars->params.num_hooks = 0;
    const struct pl_hook *hook;
    for (int i = 0; opts->user_shaders && opts->user_shaders[i]; i++) {
        if ((hook = gpu_next_core_load_hook(core, core->global, opts->user_shaders[i]))) {
            MP_TARRAY_APPEND(core, core->hooks, pars->params.num_hooks, hook);
            update_hook_opts(core, opts->user_shader_opts, opts->user_shaders[i], hook);
        }
    }

    pars->params.hooks = core->hooks;

    MP_DBG(core, "Render options updated, flushing renderer cache.\n");
    gpu_next_core_queue_set_flush(core, paused || !next_opts->inter_preserve);
}

static void gpu_next_core_update_lut(struct gpu_next_core *core,
                                     struct user_lut *lut)
{
    if (!lut->opt || !lut->opt[0]) {
        lut->lut = NULL;
        TA_FREEP(&lut->path);
        return;
    }

    if (lut->path && strcmp(lut->path, lut->opt) == 0)
        return; // no change

    // The path tracker (lut->path) lives in the user_lut, which sits in
    // the front-end's gl_next_opts m_config_cache buffer; the front-end's
    // VOCTRL_UPDATE_RENDER_OPTS compares it to request a queue reset on an
    // --image-lut change. The LUT itself is loaded and cached by the core
    // (one shared cache across the image/lut/target_lut call sites).
    talloc_replace(core, lut->path, lut->opt);
    lut->lut = gpu_next_core_load_lut(core, core->global, lut->opt);
}

void gpu_next_core_update_options(struct gpu_next_core *core,
                                  const struct gl_video_opts *opts,
                                  struct gl_next_opts *next_opts)
{
    pl_options pars = core->pars;

    gpu_next_core_update_lut(core, &next_opts->lut);
    pars->params.lut = next_opts->lut.lut;
    pars->params.lut_type = next_opts->lut.type;

    // Resolve the image LUT and hand it to the core's map callback, which
    // applies it per source frame. An --image-lut change forces a queue
    // reset (the front-end's VOCTRL_UPDATE_RENDER_OPTS), so no in-flight
    // frame can straddle a change.
    gpu_next_core_update_lut(core, &next_opts->image_lut);
    gpu_next_core_set_image_lut(core, next_opts->image_lut.lut,
                                next_opts->image_lut.type);

    // Update equalizer state
    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    mp_csp_equalizer_state_get(core->video_eq, &cparams);
    pars->color_adjustment.brightness = cparams.brightness;
    pars->color_adjustment.contrast = cparams.contrast;
    pars->color_adjustment.hue = cparams.hue;
    pars->color_adjustment.saturation = cparams.saturation;
    pars->color_adjustment.gamma = cparams.gamma * opts->gamma;
    core->output_levels = cparams.levels_out;

    for (char **kv = next_opts->raw_opts; kv && kv[0]; kv += 2)
        pl_options_set_str(pars, kv[0], kv[1]);
}

enum pl_color_levels gpu_next_core_output_levels(struct gpu_next_core *core)
{
    return core->output_levels;
}

static void update_hook_opts_dynamic(const struct pl_hook *hook,
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

void gpu_next_core_update_hooks_dynamic(struct gpu_next_core *core,
                                        const struct mp_image *mpi)
{
    for (int i = 0; i < core->pars->params.num_hooks; i++)
        update_hook_opts_dynamic(core->hooks[i], mpi);
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

bool gpu_next_core_update_icc(struct gpu_next_core *core, struct bstr icc)
{
    struct pl_icc_profile profile = {
        .data = icc.start,
        .len  = icc.len,
    };

    pl_icc_profile_compute_signature(&profile);

    bool ok = pl_icc_update(core->pllog, &core->icc_profile, &profile,
                            &core->icc_params);
    talloc_free(icc.start);
    return ok;
}

void gpu_next_core_update_icc_opts(struct gpu_next_core *core,
                                   const struct mp_icc_opts *opts)
{
    if (!opts)
        return;

    if (!opts->profile_auto && !core->icc_path) {
        // Un-set any auto-loaded profiles if icc-profile-auto was disabled
        gpu_next_core_update_icc(core, (bstr) {0});
    }

    int s_r = 0, s_g = 0, s_b = 0;
    gl_parse_3dlut_size(opts->size_str, &s_r, &s_g, &s_b);
    core->icc_params = pl_icc_default_params;
    core->icc_params.intent = opts->intent;
    core->icc_params.size_r = s_r;
    core->icc_params.size_g = s_g;
    core->icc_params.size_b = s_b;
    core->icc_params.cache = core->icc_cache.cache;

    if (!opts->profile || !opts->profile[0]) {
        // No profile enabled, un-load any existing profiles
        gpu_next_core_update_icc(core, (bstr) {0});
        TA_FREEP(&core->icc_path);
        return;
    }

    if (core->icc_path && strcmp(opts->profile, core->icc_path) == 0)
        return; // ICC profile hasn't changed

    char *fname = mp_get_user_path(NULL, core->global, opts->profile);
    MP_VERBOSE(core, "Opening ICC profile '%s'\n", fname);
    struct bstr icc = stream_read_file(fname, core, core->global, 100000000); // 100 MB
    talloc_free(fname);
    gpu_next_core_update_icc(core, icc);

    // Update cached path
    talloc_replace(core, core->icc_path, opts->profile);
}

bool gpu_next_core_icc_has_manual_profile(struct gpu_next_core *core)
{
    return core->icc_path != NULL;
}

void gpu_next_core_apply_target_icc(struct gpu_next_core *core,
                                    struct pl_frame *target, bool icc_use_luma)
{
    if (icc_use_luma) {
        core->icc_params.max_luma = 0.0f;
    } else {
        pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
            .color    = &target->color,
            .metadata = PL_HDR_METADATA_HDR10, // use only static HDR nits
            .scaling  = PL_HDR_NITS,
            .out_max  = &core->icc_params.max_luma,
        ));
    }

    pl_icc_update(core->pllog, &core->icc_profile, NULL, &core->icc_params);
    target->icc = core->icc_profile;
}

static void gpu_next_core_apply_target_contrast(const struct gl_video_opts *opts,
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

static bool use_ref_luma(const struct pl_color_space *csp,
                         const struct pl_color_space *target_csp)
{
    if (!pl_color_transfer_is_hdr(csp->transfer))
        return true;
#if PL_API_VER >= 362
    if (csp->transfer == PL_COLOR_TRC_SCRGB && target_csp &&
        !pl_color_transfer_is_hdr(target_csp->transfer))
        return true;
#endif
    return false;
}

void gpu_next_core_apply_target_options(struct gpu_next_core *core,
                                        struct pl_frame *target,
                                        float min_luma, bool hint,
                                        float target_ref_luma,
                                        const struct pl_color_space *target_csp,
                                        int fallback_depth)
{
    const struct gl_video_opts *opts = core->fe.opts;

    gpu_next_core_update_lut(core, &core->fe.next_opts->target_lut);
    target->lut = core->fe.next_opts->target_lut.lut;
    target->lut_type = core->fe.next_opts->target_lut.type;

    enum pl_color_levels output_levels = gpu_next_core_output_levels(core);
    if (output_levels)
        target->repr.levels = output_levels;
    if (opts->target_prim && (!target->color.primaries || !hint))
        target->color.primaries = opts->target_prim;
    if (opts->target_trc && (!target->color.transfer || !hint))
        target->color.transfer = opts->target_trc;
    if (opts->target_peak && (!target->color.hdr.max_luma || !hint))
        target->color.hdr.max_luma = opts->target_peak;
    if (target_ref_luma && (!target->color.hdr.max_luma || !hint) &&
        use_ref_luma(&target->color, target_csp))
        target->color.hdr.max_luma = target_ref_luma;
    if ((!target->color.hdr.min_luma || !hint))
        gpu_next_core_apply_target_contrast(opts, &target->color, min_luma);
    if (opts->target_gamut)
        mp_parse_raw_primaries(mp_null_log, opts->target_gamut, &target->color.hdr.prim);

    int dither_depth = opts->dither_depth;
    if (dither_depth == 0)
        dither_depth = fallback_depth;
#if PL_API_VER >= 362
    if (target->color.transfer == PL_COLOR_TRC_SCRGB)
        dither_depth = -1;
#endif
    if (dither_depth > 0) {
        struct pl_bit_encoding *tbits = &target->repr.bits;
        tbits->color_depth += dither_depth - tbits->sample_depth;
        tbits->sample_depth = dither_depth;
    }

    gpu_next_core_apply_target_icc(core, target, opts->icc_opts->icc_use_luma);
}

void gpu_next_core_finalize_target_csp(struct gpu_next_core *core,
                                       struct pl_frame *target,
                                       const struct mp_image *cur,
                                       const struct pl_color_space *target_csp,
                                       bool target_unknown)
{
    const struct gl_video_opts *opts = core->fe.opts;

    bool clip_gamut = pl_primaries_valid(&target->color.hdr.prim);
#if PL_API_VER >= 362
    clip_gamut = clip_gamut && target->color.transfer != PL_COLOR_TRC_SCRGB;
#endif
    if (clip_gamut) {
        // Ensure resulting gamut still fits inside container
        target->color.hdr.prim = pl_primaries_clip(&target->color.hdr.prim,
                                    pl_raw_primaries_get(target->color.primaries));
    }
    if (target->color.transfer == PL_COLOR_TRC_SRGB && cur &&
        ((opts->sdr_adjust_gamma == 0 && opts->target_trc == PL_COLOR_TRC_UNKNOWN) ||
         opts->sdr_adjust_gamma == -1))
    {
        switch (cur->params.color.transfer) {
        case PL_COLOR_TRC_BT_1886:
        case PL_COLOR_TRC_GAMMA22:
        case PL_COLOR_TRC_SRGB:
            target->color.transfer = cur->params.color.transfer;
        }
    }
    if (target->color.transfer == PL_COLOR_TRC_SRGB) {
        // sRGB reference display is pure 2.2 power function, see IEC 61966-2-1-1999.
        if (opts->treat_srgb_as_power22 & 2)
            target->color.transfer = PL_COLOR_TRC_GAMMA22;

        // TODO: Vulkan on Wayland currently interprets VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
        // in ambiguous way, depending if compositor advertises sRGB support.
        // There is currently no clear path forward to resolve this ambiguity.
        // Depending how it's resolved in Wayland Protocol, Mesa, things will
        // change.
        // See: <https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/456>
#ifdef _WIN32
        // Windows uses the sRGB piecewise function. Send piecewise sRGB to
        // Windows in HDR mode so that it can be converted to PQ, the same way
        // as mpv does internally. Note that in SDR mode, even with ACM enabled,
        // Windows assumes the display is sRGB. It doesn't perform gamma
        // conversion, or any conversions would roundtrip back to sRGB.
        // In which case the EOTF depends on the display.
        // Ideally, compositors would agree on how to handle sRGB, but I’ll
        // leave that part of the story for the reader to explore.
        // Note: Older Windows versions, without ACM, were not able to convert
        // sRGB to PQ output. We are not concerned about this case, as it would
        // look wrong anyway.
        bool target_pq = !target_unknown && target_csp->transfer == PL_COLOR_TRC_PQ;
        if (opts->treat_srgb_as_power22 & 4 && target_pq)
            target->color.transfer = PL_COLOR_TRC_SRGB;
#endif
    }
}

void gpu_next_core_update_tm_viz(struct gpu_next_core *core,
                                 const struct pl_frame *target)
{
    struct pl_color_map_params *params = &core->pars->color_map_params;
    if (!params->visualize_lut)
        return;

    // Use right half of screen for TM visualization, constrain to 1:1 AR
    const float out_w = fabsf(pl_rect_w(target->crop));
    const float out_h = fabsf(pl_rect_h(target->crop));
    const float size = MPMIN(out_w / 2.0f, out_h);
    params->visualize_rect = (pl_rect2df) {
        .x0 = 1.0f - size / out_w,
        .x1 = 1.0f,
        .y0 = 0.0f,
        .y1 = size / out_h,
    };

    // Visualize red-blue plane
    params->visualize_hue = M_PI / 4.0;
}

enum target_hint_action gpu_next_core_target_hint(
    struct gpu_next_core *core,
    const struct gl_video_opts *opts, int target_hint, int target_hint_mode,
    const struct pl_color_space *source, float target_ref_luma,
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
        if (target_ref_luma && use_ref_luma(&hint, target_csp))
            hint.hdr.max_luma = target_ref_luma;
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
        if (core->icc_profile)
            hint = core->icc_profile->csp;
        if (opts->icc_opts->icc_use_luma) {
            core->icc_params.max_luma = 0.0f;
        } else {
            pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
                .color    = &hint,
                .metadata = PL_HDR_METADATA_HDR10, // use only static HDR nits
                .scaling  = PL_HDR_NITS,
                .out_max  = &core->icc_params.max_luma,
            ));
        }
        pl_icc_update(core->pllog, &core->icc_profile, NULL, &core->icc_params);
        // Update again after possible max_luma change
        if (core->icc_profile)
            hint = core->icc_profile->csp;
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
