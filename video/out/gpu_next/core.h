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
#include <libplacebo/options.h>
#include <libplacebo/shaders/icc.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/upload.h>

#include "video/img_format.h"
#include "video/out/gpu/video.h"

struct mp_image;
struct mp_log;
struct mpv_global;
struct pl_hook;

// Front-end-agnostic libplacebo render core, shared by the windowed
// vo_gpu_next VO and (incrementally) the libmpv render backend, mirroring
// how gl_video is shared by vo_gpu.c and libmpv_gpu.c. The renderer
// itself is migrated here in later, individually golden-frame-gated
// steps; it currently owns the direct-rendering pool, the resolved
// scaler/user-shader state and the libplacebo options object.
struct gpu_next_core;

struct gpu_next_core *gpu_next_core_create(pl_gpu gpu, struct mp_log *log,
                                           pl_log pllog);
void gpu_next_core_destroy(struct gpu_next_core **core);

// The libplacebo options/render-params object owned by the core. The
// front-end populates pars->params before driving the render.
pl_options gpu_next_core_options(struct gpu_next_core *core);

// The libplacebo renderer owned by the core.
pl_renderer gpu_next_core_renderer(struct gpu_next_core *core);

// The libplacebo frame queue owned by the core. The core destroys it
// first in gpu_next_core_destroy(); see the contract there.
pl_queue gpu_next_core_queue(struct gpu_next_core *core);

// Render a frame mix into target (the windowed VO's draw_frame path).
// Swapchain-free: the front-end acquires target, drives the colorspace
// hint, and presents the result; the same entry point will back the
// libmpv render API, which has no pl_swapchain. On success the renderer's
// internal HDR-metadata state and target (color/repr/rotation) have been
// updated in place by the post-render inference (mirroring mainline's
// unused pl_frames_infer_mix follow-up), so the caller may then read
// target back and call gpu_next_core_get_hdr_metadata(). Returns false on
// libplacebo render failure, in which case the caller clears target
// itself and the inference is skipped (as in mainline).
bool gpu_next_core_render_mix(struct gpu_next_core *core,
                              const struct pl_frame_mix *mix,
                              struct pl_frame *target,
                              const struct pl_render_params *params);

// Render a single image into target (the screenshot path). Returns false
// on libplacebo render failure.
bool gpu_next_core_render_image(struct gpu_next_core *core,
                                const struct pl_frame *image,
                                const struct pl_frame *target,
                                const struct pl_render_params *params);

// Mirror pl_renderer_get_hdr_metadata(): fetch the renderer's current
// peak-detection HDR metadata into *metadata. Returns false when it is
// unavailable (non-HDR source or peak detection disabled).
bool gpu_next_core_get_hdr_metadata(struct gpu_next_core *core,
                                    struct pl_hdr_metadata *metadata);

// Wrap pl_renderer_flush_cache(): drop renderer state related to peak
// detection and frame mixing, which the front-end calls on seek (after
// pl_queue_reset). This was the last raw-renderer use in the windowed
// VO; the libmpv render backend will use the same entry point as its
// seek hook.
void gpu_next_core_flush_cache(struct gpu_next_core *core);

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

// Upload the software-decoded planes of `mpi` into the supplied texture
// slot, using the core's DR-buffer pool for zero-copy when applicable,
// and fill the corresponding pl_plane / pl_plane_data fields of `frame`.
// Sets frame->num_planes and frame->repr.bits. Mirrors how vo_gpu's
// SW-upload is shared between the windowed VO and libmpv_gpu via
// gl_video; the libmpv render API (no pl_swapchain) will drive the same
// entry point.
//
// Returns false on libplacebo upload failure, in which case mpi and any
// ref kept for an async upload callback have already been talloc_freed
// (matching mainline's map_frame failure path); the front-end's outer
// timer/stats wrapping should be closed and the caller should propagate
// false back to pl_queue. On success the lifetime contract is the
// existing one: pl_queue holds the source-frame's mpi ref until unmap,
// and the additional ref held for the async-callback path is released by
// libplacebo when it is done with the buffer.
//
// The hwdec branch of map_frame and the per-frame frame_priv plumbing
// (hwdec acquire/release, OSD subs cache, info_callback) stay with the
// front-end and remain hwdec-rig-deferred — lavapipe cannot honestly
// verify hwdec.
bool gpu_next_core_upload_sw_planes(struct gpu_next_core *core,
                                    struct mp_image *mpi,
                                    pl_tex *tex,
                                    struct pl_frame *frame);

// Map the mpv scaler option for the given unit to a libplacebo filter
// config (caching the resolved config in the core), as vo_gpu maps its
// scalers. Backs the renderer's up/down/plane scalers and frame mixer.
const struct pl_filter_config *gpu_next_core_map_scaler(
    struct gpu_next_core *core, const struct gl_video_opts *opts,
    enum scaler_unit unit);

// Load (and cache, across options updates) the user shader at the given
// path as a libplacebo hook, or NULL on failure/empty path. The cache is
// owned by the core and freed in gpu_next_core_destroy().
const struct pl_hook *gpu_next_core_load_hook(struct gpu_next_core *core,
                                              struct mpv_global *global,
                                              const char *path);

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
    float target_ref_luma, struct pl_color_space *target_csp,
    struct pl_color_space *out_hint, bool *out_target_hint,
    bool *out_target_unknown);
