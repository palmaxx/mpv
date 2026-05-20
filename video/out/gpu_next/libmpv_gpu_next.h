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

#include "video/out/libmpv.h"

// libplacebo-backed render backend, parallel to render_backend_gpu (which is
// the RA/gl_video backend). Driven by the same gpu_next_core that backs the
// windowed vo_gpu_next VO, mirroring how gl_video is shared between vo_gpu.c
// and the gpu/libmpv_gpu.c backend. Not yet registered in render_backends[];
// the registration and the per-GPU-API context-fns layer land in subsequent
// commits.
extern const struct render_backend_fns render_backend_gpu_next;
