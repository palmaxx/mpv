// Headless libmpv render-API golden harness for the gpu_next render backend,
// Vulkan surface backend (MPV_RENDER_API_TYPE_PL_VULKAN).
//
//   rapi_harness_vulkan --probe                          [create+free a ctx]
//   rapi_harness_vulkan <clip> <pts> <w> <h> <out.raw>   [render a frame]
//
// The Vulkan analogue of rapi_harness.c / rapi_harness_d3d11.c. The "host" is a
// self-contained Vulkan device on lavapipe (Mesa's software Vulkan) -- the same
// deterministic-software oracle the windowed golden uses, so this self-baselines
// exactly like the GL/D3D11 harnesses: capture once on a known-good build, then
// re-verify byte-identical after each commit (see rapi_vk_run.sh).
//
// This proves the V2.1 libmpv_pl_context_vulkan implementation
// (pl_vulkan_import / pl_vulkan_wrap + the per-frame acquire/release hold
// handshake) actually renders, not just compiles. It renders into an SDR
// VK_FORMAT_R8G8B8A8_UNORM image, copies it back through a host-visible buffer,
// and writes raw RGBA8 + a tiny sidecar.

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include <libplacebo/vulkan.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vulkan.h>

#define API_TYPE MPV_RENDER_API_TYPE_PL_VULKAN

#ifdef __APPLE__
// Vulkan-Headers only exposes these names through vulkan_beta.h and
// vulkan_metal.h. This harness only needs their string names for device
// creation, so keep the source compatible with both SDK and Homebrew headers.
#define MPV_VK_PORTABILITY_SUBSET "VK_KHR_portability_subset"
#define MPV_VK_METAL_OBJECTS       "VK_EXT_metal_objects"
#endif

static void die(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

#define VK(call) do {                                                       \
        VkResult _r = (call);                                               \
        if (_r != VK_SUCCESS) {                                             \
            fprintf(stderr, "FATAL: %s -> VkResult %d\n", #call, (int)_r);  \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

// --- host Vulkan device (lavapipe) -----------------------------------------

static VkInstance       g_inst;
static VkPhysicalDevice g_phys;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_qfam;
static const char      *g_dev_exts[128];
static uint32_t         g_num_dev_exts;
static bool             g_hwdec;
static bool             g_saw_hw_download;

#ifdef __APPLE__
static bool has_extension(const VkExtensionProperties *avail, uint32_t count,
                          const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(name, avail[i].extensionName) == 0)
            return true;
    }
    return false;
}
#endif

static void add_device_extension(const char *name)
{
    for (uint32_t i = 0; i < g_num_dev_exts; i++) {
        if (strcmp(name, g_dev_exts[i]) == 0)
            return;
    }
    if (g_num_dev_exts == sizeof(g_dev_exts) / sizeof(g_dev_exts[0]))
        die("too many Vulkan device extensions");
    g_dev_exts[g_num_dev_exts++] = name;
}

static void vk_init(void)
{
    const char *inst_exts[4] = {0};
    uint32_t num_inst_exts = 0;
    VkInstanceCreateFlags inst_flags = 0;

#ifdef __APPLE__
    // MoltenVK requires portability enumeration. This is deliberately a
    // hard requirement for the macOS gate: silently selecting no physical
    // device would make a passing probe meaningless.
    uint32_t in = 0;
    VK(vkEnumerateInstanceExtensionProperties(NULL, &in, NULL));
    VkExtensionProperties *iavail = calloc(in ? in : 1, sizeof(*iavail));
    VK(vkEnumerateInstanceExtensionProperties(NULL, &in, iavail));
    if (!has_extension(iavail, in, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        die("MoltenVK did not advertise VK_KHR_portability_enumeration");
    free(iavail);
    inst_exts[num_inst_exts++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    inst_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "rapi_harness_vulkan",
        .apiVersion = VK_API_VERSION_1_2,   // libplacebo needs >= 1.2
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .flags = inst_flags,
        .enabledExtensionCount = num_inst_exts,
        .ppEnabledExtensionNames = inst_exts,
    };
    VK(vkCreateInstance(&ici, NULL, &g_inst));

    uint32_t n = 0;
    VK(vkEnumeratePhysicalDevices(g_inst, &n, NULL));
    if (!n)
        die("no Vulkan physical device (lavapipe missing?)");
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK(vkEnumeratePhysicalDevices(g_inst, &n, devs));
    // Prefer a discrete GPU (the NVIDIA card on the Windows HDR rig) over an
    // integrated / software device; fall back to the first enumerated one.
    g_phys = devs[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(devs[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g_phys = devs[i];
            break;
        }
    }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_phys, &pp);
        fprintf(stderr, "[vk] using device: %s\n", pp.deviceName);
    }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, NULL);
    if (qn > 16) qn = 16;
    VkQueueFamilyProperties qfp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qfp);
    g_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfam = i; break; }
    }
    if (g_qfam == UINT32_MAX)
        die("no graphics queue family");

    // Enable the available subset of libplacebo's recommended device
    // extensions, so the imported device exposes what the renderer can use.
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &en, NULL);
    VkExtensionProperties *avail = calloc(en ? en : 1, sizeof(*avail));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &en, avail);
    for (int i = 0; i < pl_vulkan_num_recommended_extensions; i++) {
        const char *want = pl_vulkan_recommended_extensions[i];
        for (uint32_t j = 0; j < en; j++) {
            if (strcmp(want, avail[j].extensionName) == 0) {
                add_device_extension(want);
                break;
            }
        }
    }

#ifdef __APPLE__
    // Apple devices expose this extension through MoltenVK. It is required by
    // the Vulkan portability contract, even though the renderer itself does
    // not use a presentable surface in this headless harness.
    if (!has_extension(avail, en, MPV_VK_PORTABILITY_SUBSET))
        die("MoltenVK device did not advertise VK_KHR_portability_subset");
    add_device_extension(MPV_VK_PORTABILITY_SUBSET);

    // The VideoToolbox interop imports CVMetalTexture planes. Only require the
    // Metal-object extension for the hwdec gate: the software-decode render
    // gate remains useful on older MoltenVK versions that lack this bridge.
    if (g_hwdec) {
        if (!has_extension(avail, en, MPV_VK_METAL_OBJECTS))
            die("MoltenVK device did not advertise VK_EXT_metal_objects");
        add_device_extension(MPV_VK_METAL_OBJECTS);
    }
#endif
    free(avail);

    // Create the device with libplacebo's required features chained in pNext
    // (these are guaranteed supported by any device libplacebo accepts).
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_qfam,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &pl_vulkan_required_features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &dqci,
        .enabledExtensionCount = g_num_dev_exts,
        .ppEnabledExtensionNames = g_dev_exts,
    };
    VK(vkCreateDevice(g_phys, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
}

static void vk_uninit(void)
{
    if (g_dev) { vkDeviceWaitIdle(g_dev); vkDestroyDevice(g_dev, NULL); }
    if (g_inst) vkDestroyInstance(g_inst, NULL);
    g_dev = NULL; g_inst = NULL;
}

static uint32_t find_mem(uint32_t typebits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typebits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    die("no suitable memory type");
    return 0;
}

// Intersect the desired usage with what `fmt` actually supports as an
// optimal-tiling image. HDR formats differ in coverage -- e.g. STORAGE on
// A2B10G10R10 is optional and absent on some GPUs -- so clamp rather than fail
// vkCreateImage. COLOR_ATTACHMENT + TRANSFER_SRC are required (render + read
// back) and asserted by the caller via the returned set.
static VkImageUsageFlags supported_usage(VkFormat fmt, VkImageUsageFlags want)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_phys, fmt, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    VkImageUsageFlags got = 0;
    if ((want & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
        (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        got |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((want & VK_IMAGE_USAGE_STORAGE_BIT) &&
        (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        got |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((want & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        got |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((want & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        got |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((want & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
        got |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return got;
}

static VkImage make_image(int w, int h, VkFormat fmt, VkImageUsageFlags usage,
                          VkDeviceMemory *out_mem)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    VK(vkCreateImage(g_dev, &ici, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, img, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(mr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VK(vkAllocateMemory(g_dev, &mai, NULL, out_mem));
    VK(vkBindImageMemory(g_dev, img, *out_mem, 0));
    return img;
}

// --- common ----------------------------------------------------------------

static mpv_handle *mpv_start(void)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("mpv_create failed");
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "config", "no");
    return mpv;
}

static void drain_log(mpv_handle *mpv)
{
    while (1) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            if (g_hwdec && strstr(m->text, "HW-downloading"))
                g_saw_hw_download = true;
            fprintf(stderr, "[mpv:%s] %s: %s", m->level, m->prefix, m->text);
        }
    }
}

static mpv_vulkan_init_params vk_init_params(void)
{
    return (mpv_vulkan_init_params){
        .instance = g_inst,
        .get_proc_addr = vkGetInstanceProcAddr,
        .phys_device = g_phys,
        .device = g_dev,
        .queue_graphics_index = g_qfam,
        .queue_graphics_count = 1,
        // compute / transfer left 0 -> libplacebo falls back to graphics.
        .extensions = g_dev_exts,
        .num_extensions = (int)g_num_dev_exts,
        .features = &pl_vulkan_required_features,
    };
}

// --- probe -----------------------------------------------------------------

static int run_probe(void)
{
    vk_init();

    mpv_handle *mpv = mpv_start();
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize failed");
    mpv_request_log_messages(mpv, "error");

    mpv_vulkan_init_params vp = vk_init_params();
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)API_TYPE},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vp},
        {0},
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, params);
    drain_log(mpv);

    printf("mpv_render_context_create(\"%s\") -> %d (%s)\n",
           API_TYPE, err, mpv_error_string(err));

    int rc;
    if (err >= 0) {
        printf("PROBE OK: pl-vulkan render context created\n");
        mpv_render_context_free(rctx);
        rc = 0;
    } else {
        printf("PROBE FAIL: backend unavailable\n");
        rc = 2;
    }

    mpv_destroy(mpv);
    vk_uninit();
    return rc;
}

// --- target colorspace + format selection (mirrors rapi_harness_d3d11) -----

enum csp_mode { CSP_NONE, CSP_SRGB, CSP_PQ2020 };

static enum csp_mode parse_csp(const char *tag)
{
    if (!tag || strcmp(tag, "none") == 0) return CSP_NONE;
    if (strcmp(tag, "srgb") == 0)         return CSP_SRGB;
    if (strcmp(tag, "pq2020") == 0)       return CSP_PQ2020;
    die("csp must be none|srgb|pq2020");
    return CSP_NONE;
}

// Returns true if a TARGET_COLORSPACE param should be passed. CSP_SRGB fills an
// all-zero struct (documented "same as omitted" -> must be byte-identical to
// CSP_NONE); CSP_PQ2020 is a BT.2020/PQ HDR10 target.
static bool fill_target_colorspace(enum csp_mode mode,
                                   mpv_render_param_target_colorspace *tc)
{
    *tc = (mpv_render_param_target_colorspace){0};
    if (mode == CSP_NONE)
        return false;
    if (mode == CSP_PQ2020) {
        tc->primaries = MPV_COLOR_PRIMARIES_BT_2020;
        tc->transfer  = MPV_COLOR_TRANSFER_PQ;
        tc->hdr.max_luma = 1000.0f;
        tc->hdr.max_cll  = 1000.0f;
        tc->hdr.max_fall = 400.0f;
    }
    return true; // CSP_SRGB passes the all-zero struct on purpose
}

static VkFormat parse_fmt(const char *tag, int *out_bpp)
{
    if (!tag || strcmp(tag, "rgba8") == 0) { *out_bpp = 4; return VK_FORMAT_R8G8B8A8_UNORM; }
    if (strcmp(tag, "rgb10a2") == 0)       { *out_bpp = 4; return VK_FORMAT_A2B10G10R10_UNORM_PACK32; }
    if (strcmp(tag, "rgba16f") == 0)       { *out_bpp = 8; return VK_FORMAT_R16G16B16A16_SFLOAT; }
    die("fmt must be rgba8|rgb10a2|rgba16f");
    *out_bpp = 4;
    return VK_FORMAT_R8G8B8A8_UNORM;
}

// --- render a held frame into a VkImage and read it back -------------------

static int run_render(const char *clip, const char *pts, int w, int h,
                      const char *out, enum csp_mode csp, VkFormat fmt, int bpp)
{
    vk_init();

    mpv_handle *mpv = mpv_start();
    mpv_set_option_string(mpv, "audio", "no");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "pause", "yes");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "osd-level", "0");
    mpv_set_option_string(mpv, "interpolation", "no");
    mpv_set_option_string(mpv, "scale", "bilinear");
    mpv_set_option_string(mpv, "cscale", "bilinear");
    mpv_set_option_string(mpv, "dscale", "bilinear");
    mpv_set_option_string(mpv, "dither-depth", "8");
    mpv_set_option_string(mpv, "hr-seek", "yes");
    mpv_set_option_string(mpv, "start", pts);
    mpv_set_option_string(mpv, "hwdec", g_hwdec ? "videotoolbox" : "no");
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize failed");
    mpv_request_log_messages(mpv, g_hwdec ? "v" : "error");

    mpv_vulkan_init_params vp = vk_init_params();
    mpv_render_param cparams[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)API_TYPE},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vp},
        {0},
    };
    mpv_render_context *rctx = NULL;
    int err = mpv_render_context_create(&rctx, mpv, cparams);
    if (err < 0) {
        drain_log(mpv);
        fprintf(stderr, "FATAL: mpv_render_context_create -> %d (%s)\n",
                err, mpv_error_string(err));
        return 1;
    }

    const char *cmd[] = {"loadfile", clip, NULL};
    if (mpv_command(mpv, cmd) < 0)
        die("loadfile failed");

    VkImageUsageFlags want_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_STORAGE_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags usage = supported_usage(fmt, want_usage);
    if (!(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        die("target format lacks COLOR_ATTACHMENT or TRANSFER_SRC support");

    VkDeviceMemory img_mem;
    VkImage img = make_image(w, h, fmt, usage, &img_mem);

    // release_sem: mpv (libplacebo's hold) signals it when the image is in
    // final_layout; our readback copy waits on it. Binary semaphore, 1:1.
    VkSemaphore release_sem;
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK(vkCreateSemaphore(g_dev, &sci, NULL, &release_sem));

    bool restarted = false, rendered = false;
    int rerr = 0;
    for (int i = 0; i < 2000 && !rendered; i++) {
        mpv_event *ev = mpv_wait_event(mpv, 0.05);
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            die("mpv shut down before a frame was rendered");
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            if (g_hwdec && strstr(m->text, "HW-downloading"))
                g_saw_hw_download = true;
            fprintf(stderr, "[mpv:%s] %s: %s", m->level, m->prefix, m->text);
        }
        if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART)
            restarted = true;

        uint64_t flags = mpv_render_context_update(rctx);
        if (restarted && (flags & MPV_RENDER_UPDATE_FRAME)) {
            mpv_vulkan_tex vt = {
                .image = img, .w = w, .h = h, .format = fmt,
                .usage = usage, // exactly what the image was created with
                // Fresh image, no prior host work -> available immediately,
                // contents undefined. mpv hands it back ready for transfer.
                .current_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .acquire_sem = VK_NULL_HANDLE,
                .release_sem = release_sem,
            };
            int block = 1;
            mpv_render_param_target_colorspace tc;
            bool have_tc = fill_target_colorspace(csp, &tc);
            // A type==0 entry terminates the param array, so the conditional
            // TARGET_COLORSPACE slot is simply skipped when have_tc is false.
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_VULKAN_TEX, &vt},
                {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                {have_tc ? MPV_RENDER_PARAM_TARGET_COLORSPACE
                         : MPV_RENDER_PARAM_INVALID, &tc},
                {0},
            };
            rerr = mpv_render_context_render(rctx, rp);
            rendered = true;
        }
    }
    if (!rendered)
        die("timed out waiting for a frame");
    if (rerr < 0) {
        drain_log(mpv);
        fprintf(stderr, "FATAL: mpv_render_context_render -> %d (%s)\n",
                rerr, mpv_error_string(rerr));
        return 1;
    }

    // Read the rendered pixels back: copy the (TRANSFER_SRC_OPTIMAL) image to a
    // host-visible buffer, waiting on release_sem so libplacebo's render + the
    // hold layout transition have completed.
    size_t npix = (size_t)w * h * bpp;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = npix,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf;
    VK(vkCreateBuffer(g_dev, &bci, NULL, &buf));
    VkMemoryRequirements bmr;
    vkGetBufferMemoryRequirements(g_dev, buf, &bmr);
    VkMemoryAllocateInfo bmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = bmr.size,
        .memoryTypeIndex = find_mem(bmr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory buf_mem;
    VK(vkAllocateMemory(g_dev, &bmai, NULL, &buf_mem));
    VK(vkBindBufferMemory(g_dev, buf, buf_mem, 0));

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = g_qfam,
    };
    VkCommandPool pool;
    VK(vkCreateCommandPool(g_dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cbuf;
    VK(vkAllocateCommandBuffers(g_dev, &cbai, &cbuf));
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(vkBeginCommandBuffer(cbuf, &cbbi));
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cbuf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
    VK(vkEndCommandBuffer(cbuf));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &release_sem,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cbuf,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK(vkCreateFence(g_dev, &fci, NULL, &fence));
    VK(vkQueueSubmit(g_queue, 1, &si, fence));
    VK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));

    void *mapped = NULL;
    VK(vkMapMemory(g_dev, buf_mem, 0, npix, 0, &mapped));
    bool uniform = true;
    const uint8_t *pixels = mapped;
    for (size_t k = bpp; k < npix; k += bpp) {
        if (memcmp(pixels, pixels + k, bpp) != 0) {
            uniform = false;
            break;
        }
    }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(mapped, 1, npix, f) != npix)
        die("writing raw output failed");
    fclose(f);
    vkUnmapMemory(g_dev, buf_mem);

    // Drain any decode/interop diagnostics emitted by the render call before
    // evaluating the zero-copy gate and writing its sidecar.
    drain_log(mpv);
    char *pixfmt = mpv_get_property_string(mpv, "video-params/pixelformat");
    char *hwdec_current = g_hwdec ? mpv_get_property_string(mpv, "hwdec-current") : NULL;
    char *drop_count = g_hwdec ? mpv_get_property_string(mpv, "decoder-frame-drop-count") : NULL;
    char sidecar[512];
    snprintf(sidecar, sizeof(sidecar), "%s.txt", out);
    f = fopen(sidecar, "w");
    if (f) {
        fprintf(f, "width=%d\nheight=%d\npixelformat=%s\nsurface=vulkan\n"
                   "vkformat=%d\ncsp=%d\nbpp=%d\nhwdec_requested=%d\n"
                   "hwdec_current=%s\nhw_downloading=%d\n"
                   "decoder_frame_drop_count=%s\nnon_uniform=%d\n",
                w, h, pixfmt ? pixfmt : "?", (int)fmt, (int)csp, bpp,
                g_hwdec, hwdec_current ? hwdec_current : "(null)",
                g_saw_hw_download, drop_count ? drop_count : "?", !uniform);
        fclose(f);
    }
    int rc = 0;
    if (g_hwdec) {
        bool engaged = hwdec_current && strstr(hwdec_current, "videotoolbox");
        bool hwfmt = pixfmt && strstr(pixfmt, "videotoolbox");
        printf("hwdec: hwdec-current=%s pixelformat=%s decoder-frame-drop-count=%s "
               "hw-downloading=%d non-uniform=%d\n",
               hwdec_current ? hwdec_current : "(null)",
               pixfmt ? pixfmt : "(null)", drop_count ? drop_count : "?",
               g_saw_hw_download, !uniform);
        if (!engaged || !hwfmt || g_saw_hw_download || uniform) {
            fprintf(stderr,
                    "FAIL: expected zero-copy VideoToolbox rendering "
                    "(engaged=%d hwfmt=%d download=%d uniform=%d)\n",
                    engaged, hwfmt, g_saw_hw_download, uniform);
            rc = 1;
        } else {
            printf("PASS: VideoToolbox engaged through the Vulkan render API\n");
        }
    }
    mpv_free(pixfmt);
    mpv_free(hwdec_current);
    mpv_free(drop_count);

    printf("rendered %s @ %s -> %s (%dx%d, %zu bytes)\n",
           clip, pts, out, w, h, npix);

    vkDeviceWaitIdle(g_dev);
    vkDestroyFence(g_dev, fence, NULL);
    vkDestroyCommandPool(g_dev, pool, NULL);
    vkDestroyBuffer(g_dev, buf, NULL);
    vkFreeMemory(g_dev, buf_mem, NULL);
    vkDestroySemaphore(g_dev, release_sem, NULL);
    mpv_render_context_free(rctx);
    mpv_destroy(mpv);
    vkDestroyImage(g_dev, img, NULL);
    vkFreeMemory(g_dev, img_mem, NULL);
    vk_uninit();
    return rc;
}

int main(int argc, char **argv)
{
    int a = 1;
    for (; a < argc; a++) {
        if (strcmp(argv[a], "--hwdec") == 0)
            g_hwdec = true;
        else
            break;
    }
    argc -= a;
    argv += a;

    if (argc >= 1 && strcmp(argv[0], "--probe") == 0)
        return run_probe();

    if (argc >= 5 && argc <= 7) {
        int w = atoi(argv[2]), h = atoi(argv[3]);
        if (w <= 0 || h <= 0)
            die("width/height must be positive");
        enum csp_mode csp = parse_csp(argc >= 6 ? argv[5] : NULL);
        int bpp;
        VkFormat fmt = parse_fmt(argc >= 7 ? argv[6] : NULL, &bpp);
        return run_render(argv[0], argv[1], w, h, argv[4], csp, fmt, bpp);
    }

    fprintf(stderr,
            "usage: rapi_harness_vulkan [--hwdec] --probe\n"
            "       rapi_harness_vulkan [--hwdec] <clip> <pts> <w> <h> <out.raw> "
            "[none|srgb|pq2020] [rgba8|rgb10a2|rgba16f]\n");
    return 1;
}
