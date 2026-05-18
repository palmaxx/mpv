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

#include <libplacebo/gpu.h>

struct mp_image;

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
