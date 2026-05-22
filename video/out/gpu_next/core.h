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
    // Resolved gl_video_opts. This is the stable m_config_cache->opts
    // pointer -- m_config_cache_update() copies new values into the same
    // buffer -- so the core may hold it for its whole lifetime and read
    // live option values at map time.
    const struct gl_video_opts *opts;

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

    // Blend the front-end's OSD/subtitle source onto a frame. The core's
    // per-frame loop (gpu_next_core_update_frames) calls this for each
    // queued frame whose cached blended-subtitle overlay has gone stale.
    // The windowed VO wraps its update_overlays (which reaches vo->osd);
    // a future libmpv render backend wraps its own OSD source. NULL
    // leaves blended subtitles unrendered.
    void (*update_overlays)(void *ctx, struct mp_osd_res res, int flags,
                            enum pl_overlay_coords coords,
                            struct gpu_next_osd_state *state,
                            struct pl_frame *frame, struct mp_image *src,
                            int stereo_mode);
    void *overlays_ctx;
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

// Per-source-frame private data, hung off mp_image->priv for every frame
// pushed into the queue and allocated as a talloc child of that mp_image
// by the front-end's push loop. Visible to both the core (its map/unmap
// callbacks) and the front-end (its per-frame overlay loop, VO-side
// until E6).
struct frame_priv {
    // Back-reference to the owning core, set by the front-end at push.
    struct gpu_next_core *core;

    // hwdec resolved for this frame (NULL for software frames), looked
    // up by gpu_next_core_map_frame.
    struct ra_hwdec *hwdec;

    // Per-frame blended-subtitle overlay cache, populated by the
    // front-end's overlay loop; unmap returns its textures to the pool.
    struct gpu_next_osd_state subs;
    uint64_t osd_sync;
};

// pl_queue map/unmap/discard callbacks owned by the core. The front-end's
// push loop installs all three on the pl_source_frame; centralising them
// gives the libmpv render backend the exact same frame-ingest path with
// zero callback duplication.
//
// map turns an mpv source frame into a pl_frame: it resolves hwdec via
// the front-end interface, builds the pl_frame colorspace/representation
// (applying the hdr-reference-white and treat-srgb-as-power22 option
// tweaks), then either uploads the software planes
// (gpu_next_core_upload_sw_planes, wrapped in the front-end's
// "swdec-upload" timer) or sets up the hwdec planes with the core's own
// acquire/release callbacks, and attaches the resolved image LUT. On
// failure it frees mpi and returns false (mainline's map_frame contract).
//
// unmap returns the frame's OSD overlay textures to the core's sub_tex
// recycle pool and frees the mp_image; discard (a frame dropped before
// it was ever mapped) just frees the mp_image.
bool gpu_next_core_map_frame(pl_gpu gpu, pl_tex *tex,
                             const struct pl_source_frame *src,
                             struct pl_frame *frame);
void gpu_next_core_unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                               const struct pl_source_frame *src);
void gpu_next_core_discard_frame(const struct pl_source_frame *src);

// The libplacebo options/render-params object owned by the core. The
// front-end populates pars->params before driving the render.
pl_options gpu_next_core_options(struct gpu_next_core *core);

// The libplacebo renderer owned by the core.
pl_renderer gpu_next_core_renderer(struct gpu_next_core *core);

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
// stale (through the front-end's update_overlays hook, wrapped in the
// optional "osd-blend-update" timer) or clears them when blend-subs is
// off, and finally folds the per-frame OSD sync counter into the frame
// signature so libplacebo treats an OSD change as a distinct frame.
// src_crop / src_width / src_height are the front-end's source rect and
// the unrotated source dimensions; redraw is the vo_frame.redraw flag,
// which forces a blended-subs refresh on every queued frame. Centralising
// the loop here lets the libmpv render backend reuse it; update_overlays
// itself stays front-end-specific (it needs the front-end's OSD source).
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
// size or vo_set_queue_params's VO_MAX_REQ_FRAMES. Centralising the
// formula here keeps the queue-params request in sync with whatever
// frame mixer the core's options were configured with, which was the
// historical §3.5 leak point when the formula lived in the VO away
// from the option-resolution code that drives frame_mixer.
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
// entry point. The upload is self-timed into the core's SW-upload perf
// counter (gpu_next_core_sw_upload_perf); `ra` is used only to create
// that timer lazily on the first software frame.
//
// Returns false on libplacebo upload failure, in which case mpi and any
// ref kept for an async upload callback have already been talloc_freed
// (matching mainline's map_frame failure path); the front-end's outer
// stats wrapping should be closed and the caller should propagate false
// back to pl_queue. On success the lifetime contract is the existing
// one: pl_queue holds the source-frame's mpi ref until unmap, and the
// additional ref held for the async-callback path is released by
// libplacebo when it is done with the buffer.
//
// The hwdec branch of map_frame and the per-frame frame_priv plumbing
// (hwdec acquire/release, OSD subs cache, info_callback) stay with the
// front-end and remain hwdec-rig-deferred — lavapipe cannot honestly
// verify hwdec.
bool gpu_next_core_upload_sw_planes(struct gpu_next_core *core,
                                    struct ra *ra,
                                    struct mp_image *mpi,
                                    pl_tex *tex,
                                    struct pl_frame *frame);

// Reset the SW-upload perf counter (count -> 0), called when a hwdec
// frame is mapped so a stale SW sample is not reported. The sample
// itself (measured by gpu_next_core_upload_sw_planes) is consumed
// internally by gpu_next_core_get_perf_data.
void gpu_next_core_sw_upload_perf_reset(struct gpu_next_core *core);

// Hwdec interop owned by the core: per-decode ra_hwdec_mapper, its perf
// timer, and the last perf sample. Mirrors how SW upload moved here --
// the libplacebo-facing helpers live in the renderer core while the
// libplacebo acquire/release callbacks themselves stay in the front-end
// (which holds the stats_ctx the windowed VO wraps the map with, and
// which has whatever ra_hwdec_ctx-equivalent registry makes sense for
// it). The mapper's hwdec is looked up by the front-end and passed in
// at reconfig time; the core does not own an ra back-pointer either, ra
// is provided at reconfig (used only to lazily create the timer).

// (Re-)configure the per-decode hwdec mapper for this frame's
// format/colorspace. If params are static-equal to the existing
// mapper's source params, fast-refreshes dovi/hdr metadata in place and
// returns true. Otherwise destroys the existing mapper+timer and
// creates new ones with the supplied hwdec. Returns false on
// mapper-create failure (after logging via core->log).
bool gpu_next_core_hwdec_reconfig(struct gpu_next_core *core, struct ra *ra,
                                  struct ra_hwdec *hwdec,
                                  const struct mp_image_params *par);

// Post-reconfig destination params, valid until the next reconfig. The
// front-end uses these to drive pl_frame's color/repr/rotation under
// hwdec.
const struct mp_image_params *gpu_next_core_hwdec_dst_params(
    const struct gpu_next_core *core);

// Map a hwdec source frame: ra_hwdec_mapper_map() then populate
// frame->planes[n].texture for the first frame->num_planes via the
// per-RA-backend pl_tex wrap (ra_pl, opengl, d3d11). On success the
// libplacebo render holds the planes until gpu_next_core_hwdec_release().
// Returns false if the surface could not be mapped or any plane texture
// could not be obtained. Internally times the work via the core's
// hwdec timer; the caller wraps stats_time_start/_end around the call
// since the libmpv backend may not have a stats_ctx.
bool gpu_next_core_hwdec_acquire(struct gpu_next_core *core,
                                 struct mp_image *mpi,
                                 struct pl_frame *frame);

// Symmetric release: for non-ra_pl RA backends destroys the per-plane
// pl_tex wraps obtained at acquire time, then unmaps the mapper.
void gpu_next_core_hwdec_release(struct gpu_next_core *core,
                                 struct pl_frame *frame);

// Reset the hwdec-map perf counter, called when a SW frame is mapped so
// a stale hwdec sample is not reported. The sample itself (set by the
// most recent successful gpu_next_core_hwdec_acquire) is consumed
// internally by gpu_next_core_get_perf_data.
void gpu_next_core_hwdec_perf_reset(struct gpu_next_core *core);

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

// Load (and cache, across options updates) the LUT (cube format) at the
// given path as a libplacebo pl_custom_lut, or NULL on failure/empty
// path. Mirrors gpu_next_core_load_hook: one cache shared across the
// image/lut/target_lut call sites in the windowed VO (the libmpv backend
// will share the same cache). Cache failures (path -> NULL) so a broken
// file is not re-loaded every frame. The cache is owned by the core and
// freed in gpu_next_core_destroy().
struct pl_custom_lut *gpu_next_core_load_lut(struct gpu_next_core *core,
                                             struct mpv_global *global,
                                             const char *path);

// OSD overlay-texture recycle pool. The front-end's libplacebo unmap
// callback hands the per-frame OSD textures back to the pool when a
// source frame is released (sub_tex_push), and the front-end's overlay
// updater pops a recyclable tex when building the next frame's overlays
// (sub_tex_pop, returns NULL when empty -- the caller then allocates
// fresh). The pool is destroyed in gpu_next_core_destroy. The rest of
// the OSD path (osd_render via vo->osd, p->osd_fmt[], the per-frame
// frame_priv.subs cache, p->osd_state) stays with the front-end since
// it is front-end-specific.
void gpu_next_core_sub_tex_push(struct gpu_next_core *core, pl_tex tex);
pl_tex gpu_next_core_sub_tex_pop(struct gpu_next_core *core);

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

// Refresh every active hook's named parameters that have an auto-source
// (gpu_get_auto_param in video/out/gpu/utils.c -- PTS, chroma_offset_*,
// HDR fields) from the current frame. The front-end calls this once per
// draw, before the render.
void gpu_next_core_update_hooks_dynamic(struct gpu_next_core *core,
                                        const struct mp_image *mpi);

// Apply the target-contrast option to a colorspace (pure; no swapchain).
void gpu_next_core_apply_target_contrast(const struct gl_video_opts *opts,
                                         struct pl_color_space *color,
                                         float min_luma);

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
    const struct pl_color_space *source,
    struct pl_color_space *target_csp, struct pl_color_space *out_hint,
    bool *out_target_hint, bool *out_target_unknown);
