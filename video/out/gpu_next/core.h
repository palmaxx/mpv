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
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/upload.h>

#include "misc/bstr.h"
#include "sub/osd.h"
#include "video/img_format.h"
#include "video/out/gpu/video.h"

struct gpu_next_osd_state;
struct mp_image;
struct mp_image_params;
struct m_sub_options;
struct mp_log;
struct mp_pass_perf;
struct mpv_global;
struct pl_hook;
struct ra;
struct ra_hwdec;
struct voctrl_performance_data;
struct voctrl_screenshot;

// A user-supplied LUT (--lut / --image-lut / --target-lut): the option
// string, the path it was last loaded from (change tracker) and the
// resolved libplacebo LUT borrowed from the core's LUT cache.
struct user_lut {
    char *opt;
    char *path;
    int type;
    struct pl_custom_lut *lut;
};

// gpu-next-specific render options, parallel to the shared gl_video_opts.
// Lives here (rather than in vo_gpu_next.c) so both the windowed VO and
// the libmpv render backend can allocate an m_config_cache for it; the
// option table itself is gl_next_conf, defined in core.c.
struct gl_next_opts {
    bool delayed_peak;
    int sub_hdr_peak;
    int image_subs_hdr_peak;
    int border_background;
    float background_blur_radius;
    float corner_rounding;
    bool inter_preserve;
    struct user_lut lut;
    struct user_lut image_lut;
    struct user_lut target_lut;
    int target_hint;
    int target_hint_mode;
    bool target_hint_strict;
    char **raw_opts;
};

extern const struct m_sub_options gl_next_conf;

// Front-end-agnostic libplacebo render core, shared by the windowed
// vo_gpu_next VO and (incrementally) the libmpv render backend, mirroring
// how gl_video is shared by vo_gpu.c and libmpv_gpu.c. The renderer
// itself is migrated here in later, individually golden-frame-gated
// steps; it currently owns the direct-rendering pool, the resolved
// scaler/user-shader state and the libplacebo options object.
struct gpu_next_core;

// Create the render core on an existing pl_gpu. global/opts drive the
// on-disk shader and ICC object caches: opts gives the cache enable
// flags and directories, and the core installs the shader cache on the
// gpu (pl_gpu_set_cache) before creating the renderer, exactly as the
// windowed VO's preinit did. opts may be NULL (both caches disabled) --
// the libmpv render backend passes NULL until it resolves its own
// gl_video_opts.
struct gpu_next_core *gpu_next_core_create(pl_gpu gpu, struct mp_log *log,
                                           pl_log pllog,
                                           struct mpv_global *global,
                                           const struct gl_video_opts *opts);
void gpu_next_core_destroy(struct gpu_next_core **core);

// ICC profile handling, owned by the core. The core tracks the
// manually-configured --icc-profile path, the resolved libplacebo ICC
// object (cached in its icc_cache) and the pl_icc_params, and attaches
// the object to render targets.

// Process the mp_icc_opts: rebuild icc_params (rendering intent, 3DLUT
// size, the icc_cache handle) and, when --icc-profile names a file, load
// it -- unloading any previously loaded manual or auto profile first.
// opts may be NULL (no ICC options group).
void gpu_next_core_update_icc_opts(struct gpu_next_core *core,
                                   const struct mp_icc_opts *opts);

// Load a raw ICC profile blob, taking ownership of icc.start; icc.len ==
// 0 unloads the current profile. The front-end's auto-profile path --
// which queries the profile from the platform display, a front-end-
// specific operation -- feeds the queried blob in through here.
bool gpu_next_core_update_icc(struct gpu_next_core *core, struct bstr icc);

// Whether a manual --icc-profile is currently loaded. The front-end's
// auto-profile query skips when one is, so icc-profile-auto does not
// override an explicitly configured profile.
bool gpu_next_core_icc_has_manual_profile(struct gpu_next_core *core);

// Refresh the ICC object for a render target and attach it (target->icc),
// deriving the ICC max-luma from the target colorspace unless icc_use_luma
// (gl_video_opts.icc_opts.icc_use_luma) is set.
void gpu_next_core_apply_target_icc(struct gpu_next_core *core,
                                    struct pl_frame *target,
                                    bool icc_use_luma);

// Front-end-specific resources the core's frame-ingest callbacks
// (map/unmap/discard) reach through, instead of a back-pointer to the
// front-end. The front-end fills this in once after gpu_next_core_create()
// and installs it with gpu_next_core_set_frontend(); the core keeps a
// copy. This mirrors mpv's existing shared-core/two-front-end interfaces
// (render_backend_fns, libmpv_gpu_context_fns).
struct gpu_next_core_frontend {
    // Resolved gl_video_opts / gl_next_opts. These are the stable
    // m_config_cache->opts pointers -- m_config_cache_update() copies new
    // values into the same buffer -- so the core may hold them for its
    // whole lifetime. next_opts is non-const: the core resolves the
    // user_lut path/lut trackers embedded in it.
    const struct gl_video_opts *opts;
    struct gl_next_opts *next_opts;

    // RA handle, used to lazily create the SW-upload / hwdec perf timers
    // and to wrap hwdec plane textures.
    struct ra *ra;

    // Optional instrumentation hooks. The windowed VO supplies thin
    // wrappers over stats_time_start/_end bound to its stats_ctx; the
    // libmpv render backend has no stats_ctx and leaves these NULL, in
    // which case the core skips the timing wrap.
    void *timer_ctx;
    void (*timer_start)(void *ctx, const char *name);
    void (*timer_end)(void *ctx, const char *name);

    // Hwdec registry lookup, called from the core's map callback at map
    // time (mirroring ra_hwdec_get). NULL means SW decoding only.
    struct ra_hwdec *(*hwdec_get)(void *ctx, int imgfmt);
    void *hwdec_ctx;
};

// Install the front-end interface (see struct gpu_next_core_frontend).
// The core copies *fe; the caller may free the struct afterwards.
void gpu_next_core_set_frontend(struct gpu_next_core *core,
                                const struct gpu_next_core_frontend *fe);

// Store the resolved image LUT; the core's map callback applies it to
// each source frame. An --image-lut change forces a queue reset
// (gpu_next_core_queue_request_reset), so no in-flight frame can straddle
// a change. The front-end re-sets this from its options-update path; lut
// may be NULL (no image LUT).
void gpu_next_core_set_image_lut(struct gpu_next_core *core,
                                 struct pl_custom_lut *lut,
                                 enum pl_lut_type type);

// Install the front-end's OSD source (struct osd_state). The core reads
// it in gpu_next_core_update_overlays. The windowed VO sets vo->osd once
// at preinit; the libmpv render backend sets the OSD source it receives
// through update_external (and clears it with NULL).
void gpu_next_core_set_osd(struct gpu_next_core *core, struct osd_state *osd);

// libplacebo OSD overlay state for one frame's worth of subtitle/OSD
// bitmaps: a recyclable texture plus its built pl_overlay parts, per OSD
// part slot. mpv's core already has an (opaque) struct osd_state in
// sub/osd_state.h, so this one -- VO-private until it moved into this
// shared header -- carries the gpu_next_ prefix to avoid the clash.
struct gpu_next_osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct gpu_next_osd_state {
    struct gpu_next_osd_entry entries[MAX_OSD_PARTS];
    struct pl_overlay overlays[MAX_OSD_PARTS];
};

// The libplacebo options/render-params object owned by the core. The
// front-end populates pars->params before driving the render.
pl_options gpu_next_core_options(struct gpu_next_core *core);

// The libplacebo frame queue owned by the core. The core destroys it
// first in gpu_next_core_destroy(); see the contract there.
pl_queue gpu_next_core_queue(struct gpu_next_core *core);

// pl_queue lifecycle, owned by the core alongside the queue object. The
// windowed VO drove this state machine inline; centralising it here lets
// the libmpv render backend reuse the exact same frame-ingest logic.

// Before pushing a new batch of source frames, check whether the queue's
// head virtual PTS has already advanced past the frame the front-end is
// about to show (which happens after a non-monotonic PTS request); if so,
// latch a deferred reset. current_pts is the front-end's current frame
// PTS; pts_offset/ideal_frame_vsync_duration drive the decision and the
// log message. A reset already latched is left untouched.
void gpu_next_core_queue_check_refill(struct gpu_next_core *core,
                                      double current_pts, double pts_offset,
                                      double ideal_frame_vsync_duration);

// Per-source-frame ingest gate, called once per incoming frame id before
// pl_queue_push. Honours any latched reset (pl_queue_reset, then a cache
// flush) and cache-flush request, then deduplicates already-seen ids.
// Returns true if the front-end should push this frame, false to skip it.
bool gpu_next_core_queue_accept(struct gpu_next_core *core, int id);

// Take a new reference to a decoded source frame, attach a fresh
// frame_priv to it (a talloc child of the new ref, carrying the core
// back-reference), and push it into the queue with the core's
// map/unmap/discard callbacks. Called once per frame the front-end's
// push loop has cleared through gpu_next_core_queue_accept(). duration is
// the frame's approximate duration when interpolating, 0 otherwise.
// Centralising the push here gives the libmpv render backend the exact
// same frame-ingest entry with no source-frame plumbing of its own.
void gpu_next_core_queue_push(struct gpu_next_core *core,
                              struct mp_image *src, double duration);

// Latch a deferred queue reset, honoured at the next
// gpu_next_core_queue_accept(). Used by VOCTRL_RESET (seek) and on an
// --image-lut change.
void gpu_next_core_queue_request_reset(struct gpu_next_core *core);

// Latch (or clear) a deferred renderer-cache flush, honoured at the next
// gpu_next_core_queue_accept(). Set by a render-options update.
void gpu_next_core_queue_set_flush(struct gpu_next_core *core, bool flush);

// Run pl_queue_update for the windowed draw path: clamp the requested
// virtual PTS up to the first queued frame's PTS when the queue head has
// not yet reached it (the up-to-half-a-vsync lead described inline),
// record the resulting PTS as last_pts, then update. current_pts is the
// front-end's current frame PTS, used only for a diagnostic. Returns the
// pl_queue status for the caller to act on.
enum pl_queue_status gpu_next_core_queue_update(struct gpu_next_core *core,
                                                struct pl_frame_mix *mix,
                                                struct pl_queue_params *qparams,
                                                double current_pts);

// The last virtual PTS handed to pl_queue_update (recorded by
// gpu_next_core_queue_update); the screenshot path reads it back to
// retrieve the current frame.
double gpu_next_core_queue_last_pts(struct gpu_next_core *core);

// Per-source-frame finalisation for the draw path, run on the freshly
// updated frame mix between gpu_next_core_queue_update and the render.
// For every frame in the mix it applies the source crop, then either
// refreshes the frame's blended-subtitle overlays when they have gone
// stale (gpu_next_core_update_overlays, wrapped in the optional
// "osd-blend-update" timer) or clears them when blend-subs is off, and
// finally folds the per-frame OSD sync counter into the frame signature
// so libplacebo treats an OSD change as a distinct frame. src_crop /
// src_width / src_height are the front-end's source rect and the
// unrotated source dimensions; redraw is the vo_frame.redraw flag, which
// forces a blended-subs refresh on every queued frame.
void gpu_next_core_update_frames(struct gpu_next_core *core,
                                 const struct pl_frame_mix *mix,
                                 const struct pl_frame *target,
                                 struct mp_rect src_crop,
                                 int src_width, int src_height, bool redraw);

// Bump the OSD sync counter, invalidating every queued frame's cached
// blended-subtitle overlay so gpu_next_core_update_frames re-blends it.
// The front-end calls this when its OSD layout changes -- the windowed
// VO does so on a resize that moves the src/dst/osd rects.
void gpu_next_core_osd_changed(struct gpu_next_core *core);

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
//
// The core installs its own render-pass perf info callback on the params
// for this call (read back via gpu_next_core_get_perf_data); the caller
// does not provide one.
bool gpu_next_core_render_mix(struct gpu_next_core *core,
                              const struct pl_frame_mix *mix,
                              struct pl_frame *target,
                              const struct pl_render_params *params);

// Render a screenshot of the current frame into args->res. args is the
// VOCTRL_SCREENSHOT request; src/dst/osd_res are the front-end's current
// geometry (used for windowed screenshots); fallback_depth is the
// front-end's surface bit depth, used by apply_target_options when
// --dither-depth is 0 (the windowed VO's swapchain color_depth, 0 for
// the libmpv render API); want_alpha selects RGBA vs RGB0 for the 8-bit
// screenshot FBO; osd_state is the front-end's main-OSD overlay cache
// for the !blend_subs path. The caller refreshes the render options
// (m_config_cache_update + change-detection update_render_options +
// gpu_next_core_update_options) before calling. On success args->res
// holds the downloaded image; on failure args->res is NULL.
void gpu_next_core_screenshot(struct gpu_next_core *core,
                              struct voctrl_screenshot *args,
                              struct mp_rect src, struct mp_rect dst,
                              struct mp_osd_res osd_res,
                              int fallback_depth, bool want_alpha,
                              struct gpu_next_osd_state *osd_state);

// Mirror pl_renderer_get_hdr_metadata(): fetch the renderer's current
// peak-detection HDR metadata into *metadata. Returns false when it is
// unavailable (non-HDR source or peak detection disabled).
bool gpu_next_core_get_hdr_metadata(struct gpu_next_core *core,
                                    struct pl_hdr_metadata *metadata);

// Fill *perf with the most recent frame's render-pass timings: the
// fresh-frame and redraw-frame pl_dispatch_info lists collected by the
// core's render info callback (installed by gpu_next_core_render_mix),
// with the hwdec-map and SW-upload samples prepended to the fresh list.
// Backs vo_driver VOCTRL_PERFORMANCE_DATA and the libmpv render API's
// perfdata hook.
void gpu_next_core_get_perf_data(struct gpu_next_core *core,
                                 struct voctrl_performance_data *perf);

// Wrap pl_renderer_flush_cache(): drop renderer state related to peak
// detection and frame mixing, which the front-end calls on seek (after
// pl_queue_reset). This was the last raw-renderer use in the windowed
// VO; the libmpv render backend will use the same entry point as its
// seek hook.
void gpu_next_core_flush_cache(struct gpu_next_core *core);

// Number of source frames the renderer needs queued at draw time for
// the currently-configured options (read from the core's pl_options:
// 2 + frame_mixer kernel radius scaled by skip_anti_aliasing). The
// result is uncapped -- the front-end clamps against its own pl_queue
// size or vo_set_queue_params's VO_MAX_REQ_FRAMES. Keeping the formula
// here keeps the queue-params request in sync with the frame mixer the
// core's options were configured with.
int gpu_next_core_required_frames(struct gpu_next_core *core);

// Look up the DR buffer backing a decoder-provided host pointer, or NULL
// if it is not a direct-rendering allocation (frame-upload fast path).
pl_buf gpu_next_core_get_dr_buf(struct gpu_next_core *core, const uint8_t *ptr);

// Direct-rendering image allocator: backs vo_driver.get_image_ts and
// render_backend_fns.get_image. Returns NULL when DR is unavailable, in
// which case the caller falls back to a normal allocation.
struct mp_image *gpu_next_core_get_image(struct gpu_next_core *core,
                                         int imgfmt, int w, int h,
                                         int stride_align, int flags);

// Whether the GPU can directly upload software frames of this mpv format.
bool gpu_next_core_format_supported(pl_gpu gpu, int format, bool use_uint);

// Map the mpv scaler option for the given unit to a libplacebo filter
// config (caching the resolved config in the core), as vo_gpu maps its
// scalers. Backs the renderer's up/down/plane scalers and frame mixer.
const struct pl_filter_config *gpu_next_core_map_scaler(
    struct gpu_next_core *core, const struct gl_video_opts *opts,
    enum scaler_unit unit);

// Render the front-end's OSD/subtitle bitmaps (osd_render over the OSD
// source set with gpu_next_core_set_osd) into *frame as pl_overlays,
// recycling textures through the sub_tex pool. flags are OSD_DRAW_*;
// coords selects the libplacebo overlay coordinate space; state is the
// per-frame (blended subs) or front-end (main OSD) overlay cache; src is
// the current source frame (NULL for none), used to infer subtitle
// colorspaces; stereo_mode duplicates overlays per eye. A NULL OSD
// source leaves frame->num_overlays at 0.
void gpu_next_core_update_overlays(struct gpu_next_core *core,
                                   struct mp_osd_res res, int flags,
                                   enum pl_overlay_coords coords,
                                   struct gpu_next_osd_state *state,
                                   struct pl_frame *frame,
                                   struct mp_image *src, int stereo_mode);

// Resolve the gpu-next render options into the core's libplacebo
// pl_options: scalers, frame mixer, deband / sigmoid / peak-detect /
// tone-map / dither params, the ICC options (gpu_next_core_update_icc_opts)
// and the user-shader hook list. paused is the front-end's pause state,
// folded with interpolation-preserve into the deferred renderer-cache
// flush. The front-end calls this on an options change; it must then
// resize its frame queue from gpu_next_core_required_frames(), which the
// freshly-resolved frame mixer drives.
void gpu_next_core_update_render_options(struct gpu_next_core *core,
                                         const struct gl_video_opts *opts,
                                         const struct gl_next_opts *next_opts,
                                         bool paused);

// Resolve the per-draw gpu-next options into the core's pl_options: the
// main / image LUTs, the colour-equalizer adjustment and the
// libplacebo-opts raw passthrough. The front-end calls this every draw,
// after running gpu_next_core_update_render_options when its option
// caches report a change. next_opts is mutated (the LUT path trackers).
void gpu_next_core_update_options(struct gpu_next_core *core,
                                  const struct gl_video_opts *opts,
                                  struct gl_next_opts *next_opts);

// The output colour levels resolved from the equalizer by the last
// gpu_next_core_update_options (PL_COLOR_LEVELS_UNKNOWN until the first
// one). The front-end reads this back when building its swapchain
// colorspace hint and render target.
enum pl_color_levels gpu_next_core_output_levels(struct gpu_next_core *core);

// Refresh every active hook's named parameters that have an auto-source
// (gpu_get_auto_param in video/out/gpu/utils.c -- PTS, chroma_offset_*,
// HDR fields) from the current frame. The front-end calls this once per
// draw, before the render.
void gpu_next_core_update_hooks_dynamic(struct gpu_next_core *core,
                                        const struct mp_image *mpi);

// Resolve all target-frame options into *target: the target LUT, the
// gl_video_opts colorspace / peak / contrast / gamut overrides, dither
// depth and the ICC profile. min_luma feeds target-contrast; hint marks
// a strict swapchain-negotiated target (the overrides are then only
// applied to fields the swapchain left unset). fallback_depth is the
// front-end's surface bit depth, used when --dither-depth is 0 (the
// windowed VO's swapchain color_depth, the render API's
// MPV_RENDER_PARAM_DEPTH).
void gpu_next_core_apply_target_options(struct gpu_next_core *core,
                                        struct pl_frame *target,
                                        float min_luma, bool hint,
                                        float target_ref_luma,
                                        const struct pl_color_space *target_csp,
                                        int fallback_depth);

// Final target-colorspace refinement, run after
// gpu_next_core_apply_target_options: clip the gamut to fit its
// primaries' container, and resolve an SRGB target transfer against the
// source transfer and the treat-srgb-as-power22 option. cur is the
// current source frame (NULL for none). target_csp / target_unknown are
// the front-end's backend-reported target colorspace (zeroed / true when
// unavailable, as for the libmpv render API).
void gpu_next_core_finalize_target_csp(struct gpu_next_core *core,
                                       struct pl_frame *target,
                                       const struct mp_image *cur,
                                       const struct pl_color_space *target_csp,
                                       bool target_unknown);

// Position the tone-mapping visualization rectangle
// (--tone-mapping-visualize) in the core's render params for the given
// target. A no-op unless the visualization is enabled.
void gpu_next_core_update_tm_viz(struct gpu_next_core *core,
                                 const struct pl_frame *target);

// Apply an mpv crop rect to a pl_frame's crop field (pure; no swapchain,
// no core state). mpv hands us rotated/flipped rects while libplacebo
// expects unrotated ones, so the frame's rotation is undone in place;
// width/height are the unrotated frame dimensions, used to mirror
// flipped rects.
void gpu_next_core_apply_crop(struct pl_frame *frame, struct mp_rect crop,
                              int width, int height);

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

// Swapchain-free computation of the libplacebo target colorspace hint.
// target_csp is the backend-reported target colorspace on input (zeroed
// if unavailable); it is post-processed in place and also consumed by the
// caller afterwards. source is the current frame's colorspace, or NULL if
// there is no current frame. target_hint/target_hint_mode are the
// corresponding gl_next_opts fields. The core's ICC object/params are
// refreshed in place (pl_icc_update). Behaviour is identical to the
// inline block this was extracted from.
enum target_hint_action gpu_next_core_target_hint(
    struct gpu_next_core *core,
    const struct gl_video_opts *opts, int target_hint, int target_hint_mode,
    const struct pl_color_space *source, float target_ref_luma,
    struct pl_color_space *target_csp, struct pl_color_space *out_hint,
    bool *out_target_hint, bool *out_target_unknown);
