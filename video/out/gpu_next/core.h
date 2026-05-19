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

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/upload.h>

#include "video/img_format.h"

struct mp_image;
struct gl_video_opts;

// Front-end-agnostic libplacebo render core, shared by the windowed
// vo_gpu_next VO and (incrementally) the libmpv render backend, mirroring
// how gl_video is shared by vo_gpu.c and libmpv_gpu.c. It currently owns
// only the direct-rendering buffer pool; the renderer is migrated here in
// later, individually golden-frame-gated steps.
struct gpu_next_core;

struct gpu_next_core *gpu_next_core_create(pl_gpu gpu);
void gpu_next_core_destroy(struct gpu_next_core **core);

// Look up the DR buffer backing a decoder-provided host pointer, or NULL
// if it is not a direct-rendering allocation (frame-upload fast path).
pl_buf gpu_next_core_get_dr_buf(struct gpu_next_core *core, const uint8_t *ptr);

// Direct-rendering image allocator: backs vo_driver.get_image_ts and
// render_backend_fns.get_image. Returns NULL when DR is unavailable, in
// which case the caller falls back to a normal allocation.
struct mp_image *gpu_next_core_get_image(struct gpu_next_core *core,
                                         int imgfmt, int w, int h,
                                         int stride_align, int flags);

// Derive libplacebo plane upload descriptors (and, optionally, the shared
// bit encoding) from an mpv image format. Pure format math; returns the
// number of planes, or 0 if the format cannot be uploaded directly.
int gpu_next_core_plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                         struct pl_bit_encoding *out_bits,
                                         enum mp_imgfmt imgfmt, bool use_uint);

// Whether the GPU can directly upload software frames of this mpv format.
bool gpu_next_core_format_supported(pl_gpu gpu, int format, bool use_uint);

// Apply the target-contrast option to a colorspace (pure; no swapchain).
void gpu_next_core_apply_target_contrast(const struct gl_video_opts *opts,
                                         struct pl_color_space *color,
                                         float min_luma);

// What the caller must do with the swapchain colorspace hint after
// gpu_next_core_target_hint() produced it. The hint math is kept free of
// any swapchain access (the backend-reported target colorspace is passed
// in via target_csp; set_colorspace_hint is driven by the caller per this
// action) so the same computation backs both the windowed VO and the
// libmpv render API, which has no pl_swapchain.
enum target_hint_action {
    TARGET_HINT_NONE = 0,   // leave the swapchain hint untouched
    TARGET_HINT_SET,        // hint(&out_hint)
    TARGET_HINT_CLEAR,      // hint(NULL)
};

// Pure (swapchain-free) computation of the libplacebo target colorspace
// hint. target_csp is the backend-reported target colorspace on input
// (zeroed if unavailable); it is post-processed in place and also consumed
// by the caller afterwards. source is the current frame's colorspace, or
// NULL if there is no current frame. target_hint/target_hint_mode are the
// corresponding gl_next_opts fields. icc_profile/icc_params are updated in
// place (pl_icc_update). Behaviour is identical to the inline block this
// was extracted from.
enum target_hint_action gpu_next_core_target_hint(
    const struct gl_video_opts *opts, int target_hint, int target_hint_mode,
    const struct pl_color_space *source, pl_log pllog,
    pl_icc_object *icc_profile, struct pl_icc_params *icc_params,
    struct pl_color_space *target_csp, struct pl_color_space *out_hint,
    bool *out_target_hint, bool *out_target_unknown);
