/*
 * hbxl_vk_shim.c - Vulkan for .NET (ppy.Vk / Veldrid) on HBLauncher's Linux
 * personality.
 *
 * 1. A fake dynamic loader.  vk.dll P/Invokes dlopen/dlsym from "libdl.so.2"
 *    (bound at link time with DirectPInvoke), opens "libvulkan.so.1", and
 *    dlsym()s every command up front.  dlopen of anything Vulkan returns a
 *    sentinel; dlsym returns either an override below or a lazy trampoline
 *    (vk_tramp.S, generated) that resolves through the console's
 *    vk_icdGetInstanceProcAddr once an instance/device exists.
 *
 * 2. The display.  Veldrid has no swapchain source for VK_NN_vi_surface, so
 *    the app passes SwapchainSource.CreateWayland(1, vi_window) and this shim
 *    translates: it advertises VK_KHR_wayland_surface, enables
 *    VK_NN_vi_surface in its place, and turns vkCreateWaylandSurfaceKHR into
 *    vkCreateViSurfaceNN on the window.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>

static int unorm_views(void); /* HBXL_VK_UNORM_VIEWS, see shim_GetPhysicalDeviceSurfaceFormatsKHR */

typedef void (*vk_void)(void);
vk_void vk_icdGetInstanceProcAddr(void *instance, const char *name);   /* hbxl runtime */

extern const unsigned hbxl_vk_count;
extern void *hbxl_vk_slots[];
extern const char *const hbxl_vk_names[];
extern const unsigned char hbxl_vk_device_level[];
extern char hbxl_vk_tramps[];

static VkInstance g_instance;
static VkDevice g_device;
static PFN_vkGetDeviceProcAddr g_gdpa;
extern void hbxl_on_exit(void (*fn)(void));
static void vk_exit_cleanup(void);
static int g_vk_teardown;              /* HBXL_VK_TEARDOWN, read at vkCreateDevice */

static void vlog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    (void)!write(2, buf, (size_t)n);
}

static vk_void real_gipa(VkInstance instance, const char *name) {
    return vk_icdGetInstanceProcAddr((void *)instance, name);
}

/* ---- trampolines ---------------------------------------------------- */

void *hbxl_vk_resolve(unsigned i);
void *hbxl_vk_resolve(unsigned i) {
    const char *name = i < hbxl_vk_count ? hbxl_vk_names[i] : "?";
    void *p = 0;
    const char *how = "device";
    /* Device-level commands only through vkGetDeviceProcAddr: for instance-level
     * ones (vkGetPhysicalDevice*) the console's driver returns a device-dispatch
     * pointer that crashes when called with a VkPhysicalDevice. */
    int device_level = i < hbxl_vk_count && hbxl_vk_device_level[i];
    if (device_level && g_device && g_gdpa) p = (void *)g_gdpa(g_device, name);
    if (!p && g_instance) { p = (void *)real_gipa(g_instance, name); how = "instance"; }
    if (!p) { p = (void *)real_gipa(VK_NULL_HANDLE, name); how = "global"; }
    if (!p) {
        vlog("vk-shim: FATAL: %s called but unresolved (instance %p, device %p)",
             name, (void *)g_instance, (void *)g_device);
        abort();
    }
    hbxl_vk_slots[i] = p;
    vlog("vk-shim: resolved %s (%s)", name, how);
    return p;
}

static void *tramp_for(const char *name) {
    for (unsigned i = 0; i < hbxl_vk_count; i++)
        if (strcmp(hbxl_vk_names[i], name) == 0) return hbxl_vk_tramps + 32u * i;
    return 0;
}

/* ---- overrides ------------------------------------------------------ */

#define WAYLAND_EXT "VK_KHR_wayland_surface"

static VKAPI_ATTR VkResult VKAPI_CALL shim_EnumerateInstanceExtensionProperties(
        const char *layer, uint32_t *count, VkExtensionProperties *props) {
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)real_gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    static VkExtensionProperties list[160];
    static uint32_t total;
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return real(layer, count, props);
    if (!total) {
        uint32_t n = 0;
        VkResult r = real(NULL, &n, NULL);
        if (r < 0) return r;
        if (n > 159) n = 159;
        r = real(NULL, &n, list);
        if (r < 0) return r;
        int vi = 0;
        for (uint32_t k = 0; k < n; k++) {
            vlog("vk-shim: instance extension %s v%u", list[k].extensionName, list[k].specVersion);
            if (!strcmp(list[k].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) vi = 1;
        }
        if (vi) {
            memset(&list[n], 0, sizeof list[n]);
            strcpy(list[n].extensionName, WAYLAND_EXT);
            list[n].specVersion = 6;
            n++;
        } else {
            vlog("vk-shim: WARNING: console reports no %s", VK_NN_VI_SURFACE_EXTENSION_NAME);
        }
        total = n;
    }
    if (!props) { *count = total; return VK_SUCCESS; }
    uint32_t copy = *count < total ? *count : total;
    memcpy(props, list, copy * sizeof *props);
    *count = copy;
    return copy < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateInstance(
        const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    const char *names[64];
    VkInstanceCreateInfo copy = *ci;
    uint32_t n = ci->enabledExtensionCount < 64 ? ci->enabledExtensionCount : 64;
    for (uint32_t k = 0; k < n; k++) {
        names[k] = ci->ppEnabledExtensionNames[k];
        if (!strcmp(names[k], WAYLAND_EXT)) names[k] = VK_NN_VI_SURFACE_EXTENSION_NAME;
        vlog("vk-shim: enabling instance extension %s", names[k]);
    }
    copy.ppEnabledExtensionNames = names;
    copy.enabledExtensionCount = n;
    for (uint32_t k = 0; k < ci->enabledLayerCount; k++)
        vlog("vk-shim: requested layer %s", ci->ppEnabledLayerNames[k]);
    if (ci->pApplicationInfo)
        vlog("vk-shim: apiVersion %u.%u", VK_API_VERSION_MAJOR(ci->pApplicationInfo->apiVersion),
             VK_API_VERSION_MINOR(ci->pApplicationInfo->apiVersion));
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(&copy, alloc, out);
    vlog("vk-shim: vkCreateInstance -> %d (%p)", (int)r, r == VK_SUCCESS ? (void *)*out : 0);
    if (r == VK_SUCCESS) g_instance = *out;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyInstance real = (PFN_vkDestroyInstance)real_gipa(instance, "vkDestroyInstance");
    if (real) real(instance, alloc);
    if (instance == g_instance) g_instance = VK_NULL_HANDLE;
    vlog("vk-shim: instance destroyed");
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkDevice *out) {
    PFN_vkCreateDevice real = (PFN_vkCreateDevice)real_gipa(g_instance, "vkCreateDevice");
    /* HBXL_VK_UNORM_VIEWS: the mutable-format swapchain needs these (all listed by the console). */
    static const char *const mutable_exts[] = {
        "VK_KHR_swapchain_mutable_format", "VK_KHR_image_format_list", "VK_KHR_maintenance2",
    };
    const char *names[64];
    VkDeviceCreateInfo copy = *ci;
    if (unorm_views() && ci->enabledExtensionCount + 3 <= 64) {
        uint32_t n = 0;
        for (uint32_t k = 0; k < ci->enabledExtensionCount; k++) names[n++] = ci->ppEnabledExtensionNames[k];
        for (size_t m = 0; m < sizeof mutable_exts / sizeof mutable_exts[0]; m++) {
            int have = 0;
            for (uint32_t k = 0; k < n; k++) have |= !strcmp(names[k], mutable_exts[m]);
            if (!have) names[n++] = mutable_exts[m];
        }
        copy.enabledExtensionCount = n;
        copy.ppEnabledExtensionNames = names;
    }
    for (uint32_t k = 0; k < copy.enabledExtensionCount; k++)
        vlog("vk-shim: enabling device extension %s", copy.ppEnabledExtensionNames[k]);
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(pd, &copy, alloc, out);
    vlog("vk-shim: vkCreateDevice -> %d", (int)r);
    if (r == VK_SUCCESS) {
        g_device = *out;
        {
            static int registered;
            if (!registered) {
                const char *e = getenv("HBXL_VK_TEARDOWN");
                registered = 1;
                g_vk_teardown = e && *e == '1';
                hbxl_on_exit(vk_exit_cleanup);
            }
        }
        g_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(g_instance, "vkGetDeviceProcAddr");
    }
    return r;
}

/* Exit cleanup (registered with hbxl_on_exit when the device is created). osu!'s host disposal never destroys its
 * Veldrid device, so its GPU memory (textures, buffers, the kept swapchain) outlives the app: relaunching from HBLauncher
 * then fails in .NET startup, because the GC's 1 GiB reservation (Vulkan-backed on hbxl) no longer fits (exit_group(-1),
 * 2026-11-09). Destroying swapchain + device + surface + instance at exit crashed SmileBASIC when the launcher took its
 * display back (the surface sits on the launcher's vi window), so this is now OPT-IN: HBXL_VK_TEARDOWN=1 destroys only
 * the swapchain and the device. Clean exits only (after a crash another thread may still draw). Logs through hbxl_log:
 * any syscall (write) during exit returns to the host immediately. */
extern volatile int hbxl_exiting;
extern void hbxl_log(const char *text);
static VkSurfaceKHR g_sc_surface;
static VkSwapchainKHR g_swapchain;
static unsigned g_sc_refs;

static void vk_exit_cleanup(void) {
    if (!g_device || !g_vk_teardown) return;
    if (hbxl_exiting & 0xff) {
        hbxl_log("vk-shim: exit with an error status: leaving the device alone");
        return;
    }
    PFN_vkDeviceWaitIdle wait = g_gdpa ? (PFN_vkDeviceWaitIdle)g_gdpa(g_device, "vkDeviceWaitIdle") : 0;
    PFN_vkDestroySwapchainKHR destroy_sc = g_gdpa ? (PFN_vkDestroySwapchainKHR)g_gdpa(g_device, "vkDestroySwapchainKHR") : 0;
    PFN_vkDestroyDevice destroy_dev = g_gdpa ? (PFN_vkDestroyDevice)g_gdpa(g_device, "vkDestroyDevice") : 0;
    if (!destroy_dev) destroy_dev = (PFN_vkDestroyDevice)real_gipa(g_instance, "vkDestroyDevice");
    hbxl_log("vk-shim: exit: destroying swapchain and device (HBXL_VK_TEARDOWN=1)");
    if (wait) wait(g_device);
    if (g_swapchain && destroy_sc) destroy_sc(g_device, g_swapchain, NULL);
    g_swapchain = VK_NULL_HANDLE;
    g_sc_refs = 0;
    if (destroy_dev) destroy_dev(g_device, NULL);
    g_device = VK_NULL_HANDLE;
    hbxl_log("vk-shim: exit: swapchain and device destroyed (surface and instance kept)");
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyDevice real = g_gdpa ? (PFN_vkDestroyDevice)g_gdpa(device, "vkDestroyDevice") : 0;
    if (!real) real = (PFN_vkDestroyDevice)real_gipa(g_instance, "vkDestroyDevice");
    if (real) real(device, alloc);
    if (device == g_device) g_device = VK_NULL_HANDLE;
    vlog("vk-shim: device destroyed");
}

/* VkWaylandSurfaceCreateInfoKHR without the wayland headers. */
typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    void *surface;
} wayland_ci;

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateWaylandSurfaceKHR(VkInstance instance, const wayland_ci *ci,
                                                                   const VkAllocationCallbacks *alloc,
                                                                   VkSurfaceKHR *out) {
    PFN_vkCreateViSurfaceNN real = (PFN_vkCreateViSurfaceNN)real_gipa(instance, "vkCreateViSurfaceNN");
    VkViSurfaceCreateInfoNN vi = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, NULL, 0, ci->surface};
    if (!real) { vlog("vk-shim: vkCreateViSurfaceNN unavailable"); return VK_ERROR_EXTENSION_NOT_PRESENT; }
    VkResult r = real(instance, &vi, alloc, out);
    vlog("vk-shim: vkCreateViSurfaceNN(window %p) -> %d", ci->surface, (int)r);
    return r;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL shim_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice pd,
                                                                                           uint32_t family, void *display) {
    (void)pd; (void)family; (void)display;
    return VK_TRUE;
}

/* ---- presentation diagnostics (swapchain, acquire, present) ----------- */

static void *device_fn(const char *name) {
    return g_gdpa && g_device ? (void *)g_gdpa(g_device, name) : 0;
}

/* One swapchain per vi window.  The console only displays the first swapchain made on
 * the launcher's vi layer: osu-framework recreates its swapchain (window resize event,
 * vsync change) before presenting, and every later swapchain presented "successfully" to
 * a black screen.  So a request matching the live swapchain (same surface, extent, format)
 * returns that swapchain again and counts a reference; vkDestroySwapchainKHR only destroys
 * it when the last reference goes.  Anything else (e.g. a new size) is a real recreation. */
static VkExtent2D g_sc_extent;
static VkFormat g_sc_format;
/* Veldrid acquires an image right after every (re)creation.  With reuse, an image the
 * app acquired but never presented is still owned by the app; acquiring again each time
 * would leave images stuck until FIFO deadlocks.  So the next acquire after a reuse
 * returns that image again, signalling the caller's semaphore/fence with an empty submit. */
static int64_t g_outstanding = -1;     /* acquired, not yet presented */
static int64_t g_pending = -1;         /* to hand back on the next acquire */
static VkQueue g_queue;                /* first queue fetched (Veldrid's graphics+present) */

/* Gamma. Veldrid picks B8G8R8A8_UNORM if offered, else the FIRST surface format. The console
 * lists R8G8B8A8_SRGB (43) then R8G8B8A8_UNORM (37); osu-framework writes already gamma-encoded
 * colours, so the sRGB swapchain encodes them twice (washed out). A plain UNORM swapchain
 * is created fine but NEVER PRESENTS on the vi layer (hardware 2026-11-08: black screen, the
 * first present never returned). So: report UNORM first to Veldrid, but create the swapchain as
 * SRGB with MUTABLE_FORMAT + a format list, so Veldrid's UNORM image views write the bytes
 * unconverted into images the vi layer accepts. Confirmed on hardware 2026-11-08 (correct colours),
 * so it is the default; HBXL_VK_UNORM_VIEWS=0 turns it off. */
static int unorm_views(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("HBXL_VK_UNORM_VIEWS");
        v = !(e && e[0] == '0');
        vlog("vk-shim: HBXL_VK_UNORM_VIEWS %s", v ? "on (SRGB swapchain, UNORM views)" : "off (SRGB swapchain as reported)");
    }
    return v;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                             uint32_t *count, VkSurfaceFormatKHR *formats) {
    static int logged;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)real_gipa(g_instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(pd, surface, count, formats);
    if (!formats || (r != VK_SUCCESS && r != VK_INCOMPLETE) || !count) return r;

    uint32_t unorm = *count;
    for (uint32_t i = 0; i < *count && unorm == *count; i++)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            unorm = i;

    if (!logged) {
        logged = 1;
        for (uint32_t i = 0; i < *count; i++)
            vlog("vk-shim: surface format %u: format %d colorspace %d", i, (int)formats[i].format, (int)formats[i].colorSpace);
    }

    if (unorm_views() && unorm != *count && unorm != 0) {
        VkSurfaceFormatKHR chosen = formats[unorm];
        memmove(&formats[1], &formats[0], unorm * sizeof formats[0]);
        formats[0] = chosen;
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                             const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    PFN_vkCreateSwapchainKHR real = (PFN_vkCreateSwapchainKHR)device_fn("vkCreateSwapchainKHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (g_swapchain && g_sc_refs && ci->surface == g_sc_surface && ci->imageFormat == g_sc_format &&
        ci->imageExtent.width == g_sc_extent.width && ci->imageExtent.height == g_sc_extent.height) {
        g_sc_refs++;
        *out = g_swapchain;
        g_pending = g_outstanding;
        vlog("vk-shim: vkCreateSwapchainKHR %ux%u (asked present %d, old %p) -> reusing %p (refs %u)",
             ci->imageExtent.width, ci->imageExtent.height, (int)ci->presentMode, (void *)ci->oldSwapchain,
             (void *)g_swapchain, g_sc_refs);
        return VK_SUCCESS;
    }
    /* Only FIFO is known to display on the vi window (HBLauncher ARCHITECTURE.md). */
    VkSwapchainCreateInfoKHR copy = *ci;
    copy.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    /* HBXL_VK_UNORM_VIEWS (see shim_GetPhysicalDeviceSurfaceFormatsKHR): SRGB images, UNORM views. */
    static const VkFormat view_formats[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
    VkImageFormatListCreateInfoKHR list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = ci->pNext,
        .viewFormatCount = 2,
        .pViewFormats = view_formats,
    };
    if (unorm_views() && ci->imageFormat == VK_FORMAT_R8G8B8A8_UNORM) {
        copy.imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
        copy.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        copy.pNext = &list;
        vlog("vk-shim: swapchain as R8G8B8A8_SRGB, mutable, for UNORM views");
    }
    VkResult r = real(device, &copy, alloc, out);
    vlog("vk-shim: vkCreateSwapchainKHR %ux%u images %u format %d colorspace %d present %d (asked %d) alpha %d old %p -> %d (%p)",
         ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount, (int)ci->imageFormat,
         (int)ci->imageColorSpace, (int)copy.presentMode, (int)ci->presentMode, (int)ci->compositeAlpha,
         (void *)ci->oldSwapchain, (int)r, r == VK_SUCCESS ? (void *)*out : 0);
    if (r == VK_SUCCESS) {
        g_swapchain = *out;
        g_sc_surface = ci->surface;
        g_sc_extent = ci->imageExtent;
        g_sc_format = ci->imageFormat;
        g_sc_refs = 1;
        g_outstanding = g_pending = -1;
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
                                                          const VkAllocationCallbacks *alloc) {
    PFN_vkDestroySwapchainKHR real = (PFN_vkDestroySwapchainKHR)device_fn("vkDestroySwapchainKHR");
    if (sc && sc == g_swapchain) {
        if (g_sc_refs > 1) {
            g_sc_refs--;
            vlog("vk-shim: vkDestroySwapchainKHR %p kept (refs %u)", (void *)sc, g_sc_refs);
            return;
        }
        g_swapchain = VK_NULL_HANDLE;
        g_sc_refs = 0;
    }
    vlog("vk-shim: vkDestroySwapchainKHR %p", (void *)sc);
    if (real) real(device, sc, alloc);
}

static unsigned g_acquire_errors, g_present_errors;
static unsigned long g_presents;

static VKAPI_ATTR VkResult VKAPI_CALL shim_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR sc, uint64_t timeout,
                                                              VkSemaphore sem, VkFence fence, uint32_t *index) {
    PFN_vkAcquireNextImageKHR real = (PFN_vkAcquireNextImageKHR)device_fn("vkAcquireNextImageKHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (sc == g_swapchain && g_pending >= 0 && g_queue) {
        PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)device_fn("vkQueueSubmit");
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, sem ? 1u : 0u, sem ? &sem : NULL};
        VkResult sr = submit ? submit(g_queue, sem ? 1u : 0u, sem ? &si : NULL, fence) : VK_ERROR_INITIALIZATION_FAILED;
        vlog("vk-shim: vkAcquireNextImageKHR -> image %u again (acquired before the reuse), signal -> %d",
             (unsigned)g_pending, (int)sr);
        if (sr != VK_SUCCESS) return sr;
        *index = (uint32_t)g_pending;
        g_pending = -1;
        return VK_SUCCESS;
    }
    VkResult r = real(device, sc, timeout, sem, fence, index);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        if (sc == g_swapchain) g_outstanding = *index;
    } else if (g_acquire_errors++ < 20) {
        vlog("vk-shim: vkAcquireNextImageKHR -> %d", (int)r);
    }
    return r;
}

/* osu-game's stall detector (shim/hbxl_input.c); weak, so other apps sharing this shim link without it. */
void hbxl_stall_beat(int which) __attribute__((weak));
static VKAPI_ATTR VkResult VKAPI_CALL shim_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    static PFN_vkQueuePresentKHR real;
    if (!real) real = (PFN_vkQueuePresentKHR)device_fn("vkQueuePresentKHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(queue, info);
    if (hbxl_stall_beat) hbxl_stall_beat(1);
    for (uint32_t k = 0; k < info->swapchainCount; k++)
        if (info->pSwapchains[k] == g_swapchain && (int64_t)info->pImageIndices[k] == g_outstanding)
            g_outstanding = -1;
    g_presents++;
    if (r != VK_SUCCESS && g_present_errors++ < 20) vlog("vk-shim: vkQueuePresentKHR #%lu -> %d", g_presents, (int)r);
    else if (g_presents <= 3 || g_presents % 600 == 0) vlog("vk-shim: present #%lu ok (image %u)", g_presents,
                                                            info->pImageIndices ? info->pImageIndices[0] : 0);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out) {
    PFN_vkGetDeviceQueue real = (PFN_vkGetDeviceQueue)device_fn("vkGetDeviceQueue");
    if (!real) { *out = VK_NULL_HANDLE; return; }
    real(device, family, index, out);
    if (!g_queue) g_queue = *out;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetInstanceProcAddr(VkInstance instance, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(VkDevice device, const char *name);

static void *override_for(const char *name) {
    static const struct { const char *name; void *fn; } table[] = {
        {"vkGetInstanceProcAddr", (void *)shim_GetInstanceProcAddr},
        {"vkGetDeviceProcAddr", (void *)shim_GetDeviceProcAddr},
        {"vkEnumerateInstanceExtensionProperties", (void *)shim_EnumerateInstanceExtensionProperties},
        {"vkCreateInstance", (void *)shim_CreateInstance},
        {"vkDestroyInstance", (void *)shim_DestroyInstance},
        {"vkCreateDevice", (void *)shim_CreateDevice},
        {"vkDestroyDevice", (void *)shim_DestroyDevice},
        {"vkCreateWaylandSurfaceKHR", (void *)shim_CreateWaylandSurfaceKHR},
        {"vkGetPhysicalDeviceWaylandPresentationSupportKHR", (void *)shim_GetPhysicalDeviceWaylandPresentationSupportKHR},
        {"vkGetPhysicalDeviceSurfaceFormatsKHR", (void *)shim_GetPhysicalDeviceSurfaceFormatsKHR},
        {"vkCreateSwapchainKHR", (void *)shim_CreateSwapchainKHR},
        {"vkDestroySwapchainKHR", (void *)shim_DestroySwapchainKHR},
        {"vkGetDeviceQueue", (void *)shim_GetDeviceQueue},
        {"vkAcquireNextImageKHR", (void *)shim_AcquireNextImageKHR},
        {"vkQueuePresentKHR", (void *)shim_QueuePresentKHR},
    };
    for (size_t k = 0; k < sizeof table / sizeof table[0]; k++)
        if (!strcmp(table[k].name, name)) return table[k].fn;
    return 0;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetInstanceProcAddr(VkInstance instance, const char *name) {
    void *o = override_for(name);
    if (o) return (PFN_vkVoidFunction)o;
    return (PFN_vkVoidFunction)real_gipa(instance, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(VkDevice device, const char *name) {
    void *o = override_for(name);
    if (o) return (PFN_vkVoidFunction)o;
    PFN_vkGetDeviceProcAddr gdpa = g_gdpa ? g_gdpa : (PFN_vkGetDeviceProcAddr)real_gipa(g_instance, "vkGetDeviceProcAddr");
    return gdpa ? gdpa(device, name) : 0;
}

/* ---- fake libdl ----------------------------------------------------- */

#define VULKAN_HANDLE ((void *)0x564b4c4eu)   /* "VKLN" */
#define OPENSSL_HANDLE ((void *)0x5353514cu)  /* "LQSS" */
#define FFMPEG_HANDLE ((void *)0x47504646u)   /* "FFPG" */
static const char *g_dlerror;

/* Statically linked OpenSSL (libssl.a + libcrypto.a), exposed to .NET's
 * System.Security.Cryptography.Native.OpenSsl, which dlopen()s "libssl.so.3" and
 * dlsym()s every function it needs.  The table is generated by gen-ssl-syms.py and
 * only exists in apps that link OpenSSL (weak, so other apps link unchanged). */
struct hbxl_dl_sym { const char *name; void *addr; };
extern const struct hbxl_dl_sym hbxl_ssl_syms[] __attribute__((weak));
extern const unsigned hbxl_ssl_count __attribute__((weak));

/* Statically linked FFmpeg (libavformat/avcodec/avutil/swresample.a, osu-game audio),
 * for FFmpeg.AutoGen's DynamicallyLoadedBindings and osu-framework's video decoder, which
 * dlopen() "libavcodec.so.61" etc. and dlsym() each function.  Same generator, same rules. */
extern const struct hbxl_dl_sym hbxl_ffmpeg_syms[] __attribute__((weak));
extern const unsigned hbxl_ffmpeg_count __attribute__((weak));

static void *table_lookup(const struct hbxl_dl_sym *tab, unsigned n, const char *name) {
    unsigned lo = 0, hi = n;                       /* table is sorted by strcmp */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(name, tab[mid].name);
        if (!c) return tab[mid].addr;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

static void *ssl_lookup(const char *name) {
    return &hbxl_ssl_count ? table_lookup(hbxl_ssl_syms, hbxl_ssl_count, name) : 0;
}

static void *ffmpeg_lookup(const char *name) {
    return &hbxl_ffmpeg_count ? table_lookup(hbxl_ffmpeg_syms, hbxl_ffmpeg_count, name) : 0;
}

static int is_ffmpeg_lib(const char *file) {
    return strstr(file, "libavcodec") || strstr(file, "libavformat") || strstr(file, "libavutil")
        || strstr(file, "libswresample");
}

void *dlopen(const char *file, int mode) {
    (void)mode;
    if (file && strstr(file, "vulkan")) {
        vlog("vk-shim: dlopen(%s) -> console Vulkan", file);
        g_dlerror = 0;
        return VULKAN_HANDLE;
    }
    if (file && (strstr(file, "libssl") || strstr(file, "libcrypto")) && &hbxl_ssl_count) {
        vlog("vk-shim: dlopen(%s) -> static OpenSSL (%u symbols)", file, hbxl_ssl_count);
        g_dlerror = 0;
        return OPENSSL_HANDLE;
    }
    if (file && is_ffmpeg_lib(file) && &hbxl_ffmpeg_count) {
        vlog("vk-shim: dlopen(%s) -> static FFmpeg (%u symbols)", file, hbxl_ffmpeg_count);
        g_dlerror = 0;
        return FFMPEG_HANDLE;
    }
    vlog("vk-shim: dlopen(%s) refused: no dynamic loading on hbxl", file ? file : "(null)");
    g_dlerror = "hbxl: dynamic loading is not supported";
    return 0;
}

void *dlsym(void *restrict handle, const char *restrict name) {
    void *p = 0;
    if (handle == VULKAN_HANDLE && name) {
        p = override_for(name);
        if (!p && (!strcmp(name, "vkEnumerateInstanceVersion") || !strcmp(name, "vkEnumerateInstanceLayerProperties")))
            p = (void *)real_gipa(VK_NULL_HANDLE, name);
        if (!p) p = tramp_for(name);
    } else if (handle == OPENSSL_HANDLE && name) {
        p = ssl_lookup(name);
    } else if (handle == FFMPEG_HANDLE && name) {
        p = ffmpeg_lookup(name);
    }
    g_dlerror = p ? 0 : "hbxl: symbol not found";
    return p;
}

char *dlerror(void) {
    const char *e = g_dlerror;
    g_dlerror = 0;
    return (char *)e;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}
