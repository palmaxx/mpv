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

#include "common/common.h"
#include "common/msg.h"
#include "osdep/threads.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/out/gpu/video.h"
#include "video/out/vo.h"

struct gpu_next_core {
    pl_gpu gpu;

    // Allocated DR buffers
    mp_mutex dr_lock;
    pl_buf *dr_buffers;
    int num_dr_buffers;
};

struct gpu_next_core *gpu_next_core_create(pl_gpu gpu)
{
    struct gpu_next_core *core = talloc_zero(NULL, struct gpu_next_core);
    core->gpu = gpu;
    mp_mutex_init(&core->dr_lock);
    return core;
}

void gpu_next_core_destroy(struct gpu_next_core **core_ptr)
{
    struct gpu_next_core *core = *core_ptr;
    if (!core)
        return;

    mp_assert(core->num_dr_buffers == 0);
    mp_mutex_destroy(&core->dr_lock);

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
