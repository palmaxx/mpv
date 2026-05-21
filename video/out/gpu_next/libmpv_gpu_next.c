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

#include <string.h>

#include "libmpv_gpu_next.h"

#include "mpv/render.h"
#include "video/out/libmpv.h"

static const struct libmpv_pl_context_fns *context_backends[] = {
    // First impl (libmpv_pl_context_gl) lands in the next wiring commit.
    NULL,
};

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;

    for (int n = 0; context_backends[n]; n++) {
        if (strcmp(context_backends[n]->api_name, api) == 0) {
            // Backend match; instantiation lands in the next wiring commit.
            break;
        }
    }
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void destroy(struct render_backend *ctx)
{
}

const struct render_backend_fns render_backend_gpu_next = {
    .init = init,
    .destroy = destroy,
};
