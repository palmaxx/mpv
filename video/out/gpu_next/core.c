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
#include "osdep/threads.h"
#include "video/mp_image.h"
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
