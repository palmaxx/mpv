/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_VULKAN_H_
#define MPV_CLIENT_API_RENDER_VULKAN_H_

#include <vulkan/vulkan_core.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vulkan backend
 * --------------
 *
 * This header contains definitions for using Vulkan with the render.h API.
 * The backend renders through the libplacebo-based gpu-next pipeline onto a
 * host-provided VkImage, parallel to MPV_RENDER_API_TYPE_PL_D3D11 but
 * cross-platform: Linux/Wayland HDR, and Vulkan-on-Windows for hosts that
 * prefer a single graphics API across platforms.
 *
 * The host owns the VkInstance / VkPhysicalDevice / VkDevice and presents the
 * rendered image itself (there is no swapchain on mpv's side). mpv imports the
 * existing device via libplacebo's pl_vulkan_import() — it never creates its
 * own device — and wraps each host-provided VkImage as a render target.
 *
 * Device requirements
 * -------------------
 *
 * The VkInstance must be created with VkApplicationInfo.apiVersion >= Vulkan
 * 1.2. The VkDevice must be created with at least the physical-device features
 * libplacebo requires (see libplacebo's pl_vulkan_required_features); the host
 * is responsible for enabling them, and for enabling any device extensions it
 * lists in mpv_vulkan_init_params::extensions. A graphics-capable queue family
 * is required; dedicated compute / transfer queues are optional (mpv falls
 * back to the graphics queue when they are left at {0}).
 *
 * Threading
 * ---------
 *
 * libplacebo serializes its own queue submissions, but the host must not submit
 * work to, or otherwise use, the queues it handed to mpv concurrently with
 * mpv_render_context_render() unless it provides lock_queue / unlock_queue
 * callbacks (not currently exposed — call render() on a thread that is not
 * racing the host's own queue use). VkImage ownership is transferred per frame
 * via the explicit acquire / release synchronization below.
 *
 * Per-frame image synchronization
 * -------------------------------
 *
 * Unlike D3D11, a wrapped VkImage is an externally-synchronized resource. For
 * each render, the host hands mpv an image it currently owns and gets it back
 * in a defined layout, mediated by two semaphores:
 *
 *   - acquire_sem: the host signals it when the image is ready for mpv to
 *     render into; mpv waits on it before touching the image. mpv tells
 *     libplacebo the image's current_layout at that point.
 *   - release_sem: mpv signals it when it has finished rendering; the host
 *     waits on it before presenting / reusing the image. mpv transitions the
 *     image to final_layout before signaling.
 *
 * Both may be timeline semaphores (set the corresponding *_value) or binary
 * semaphores (leave the value 0). This mirrors libplacebo's
 * pl_vulkan_release_ex / pl_vulkan_hold_ex contract directly.
 *
 * mpv wraps the image at the start of each render call and destroys its wrap
 * before the call returns; no mpv-held wrap of a host VkImage survives
 * between calls. Exception: when mpv_render_context_render() returns an error
 * because the final hand-back failed (release_sem could not be signalled),
 * mpv retains the wrap and reclaims it on a subsequent render call or at
 * context destruction — the host must not destroy or recycle the VkImage
 * until release_sem has been signalled anyway, so this imposes no extra
 * host obligation.
 *
 * Colorspace and HDR
 * ------------------
 *
 * As with the D3D11 backend, the host negotiates the target colorspace and HDR
 * metadata per frame via MPV_RENDER_PARAM_TARGET_COLORSPACE (see render.h).
 * For an HDR10 target the host presents to a BT.2020 / PQ surface (e.g. a
 * VK_COLOR_SPACE_HDR10_ST2084_EXT swapchain) and passes the matching
 * mpv_render_param_target_colorspace; for SDR, omit the parameter.
 *
 * On platforms without libplacebo Vulkan support this header is still
 * installed, but mpv_render_context_create() returns MPV_ERROR_NOT_IMPLEMENTED.
 */

/**
 * For initializing the libplacebo Vulkan backend via
 * MPV_RENDER_PARAM_VULKAN_INIT_PARAMS. Mirrors libplacebo's
 * pl_vulkan_import_params; all handles are owned by the host and must outlive
 * the mpv_render_context.
 */
typedef struct mpv_vulkan_init_params {
    /**
     * The host's VkInstance. Required. Must be created with apiVersion >=
     * Vulkan 1.2.
     */
    VkInstance instance;
    /**
     * Pointer to vkGetInstanceProcAddr. If NULL, libplacebo uses its directly
     * linked version (if available). Strongly recommended to be set so mpv and
     * the host share one loader.
     */
    PFN_vkGetInstanceProcAddr get_proc_addr;
    /**
     * The host's chosen VkPhysicalDevice. Required.
     */
    VkPhysicalDevice phys_device;
    /**
     * The host's logical VkDevice. Required. Must have been created with the
     * features libplacebo requires and with the extensions listed below.
     */
    VkDevice device;
    /**
     * Enabled queue families. graphics is required and must support
     * VK_QUEUE_GRAPHICS_BIT; compute / transfer are optional (leave count = 0
     * to fall back to the graphics queue). index is the queue family index,
     * count the number of queues created from it.
     */
    uint32_t queue_graphics_index, queue_graphics_count;
    uint32_t queue_compute_index,  queue_compute_count;
    uint32_t queue_transfer_index, queue_transfer_count;
    /**
     * All device-level extensions enabled on `device`. May be NULL/0.
     */
    const char * const *extensions;
    int num_extensions;
    /**
     * The VkPhysicalDeviceFeatures2 the device was created with. The device
     * must have been created with at least libplacebo's required features. May
     * be NULL (libplacebo then assumes only the base feature set).
     */
    const VkPhysicalDeviceFeatures2 *features;
} mpv_vulkan_init_params;

/**
 * For MPV_RENDER_PARAM_VULKAN_TEX. Describes the host's render-target VkImage
 * and the per-frame acquire / release synchronization (see "Per-frame image
 * synchronization" above).
 */
typedef struct mpv_vulkan_tex {
    /**
     * The render-target image. Must be a 2D, single-sample, single-mip color
     * image usable as a color attachment (and, for libplacebo's compute paths,
     * ideally VK_IMAGE_USAGE_STORAGE_BIT). Created by the same VkDevice passed
     * at init. The host retains ownership; mpv does not destroy it.
     */
    VkImage image;
    /**
     * The image dimensions in pixels. Must match the image's own extent.
     */
    int w, h;
    /**
     * The image's VkFormat. libplacebo maps it to a pl_fmt; wrapping fails if
     * no compatible format exists.
     */
    VkFormat format;
    /**
     * The VkImageUsageFlags the image was created with. libplacebo derives the
     * pl_tex capabilities from these.
     */
    VkImageUsageFlags usage;
    /**
     * The image layout the host leaves the image in when handing it to mpv
     * (when acquire_sem fires). Passed to libplacebo as the current/acquire
     * layout.
     */
    VkImageLayout current_layout;
    /**
     * The layout mpv must transition the image to before signaling release_sem,
     * i.e. the layout the host needs for presentation (e.g.
     * VK_IMAGE_LAYOUT_PRESENT_SRC_KHR or VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).
     */
    VkImageLayout final_layout;
    /**
     * Semaphore the host signals when the image is ready for mpv. mpv waits on
     * it before rendering. acquire_value is the timeline value to wait for
     * (0 for a binary semaphore). May be VK_NULL_HANDLE if the host guarantees
     * the image is already available (e.g. via host-side vkDeviceWaitIdle).
     */
    VkSemaphore acquire_sem;
    uint64_t    acquire_value;
    /**
     * Semaphore mpv signals when rendering is complete and the image is in
     * final_layout. The host waits on it before presenting / reusing the image.
     * release_value is the timeline value mpv signals (0 for a binary
     * semaphore). Required.
     */
    VkSemaphore release_sem;
    uint64_t    release_value;
} mpv_vulkan_tex;

#ifdef __cplusplus
}
#endif

#endif
