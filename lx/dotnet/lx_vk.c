/*
 * Vulkan for .NET (ppy.Vk / Veldrid) in the island. Linked when tools/aot-island.py gets --vulkan, together with the
 * trampolines tools/gen-vk-tramp.py generates (lx_vk_tramps, lx_vk_slots, lx_vk_names).
 *
 * 1. Lazy trampolines. Veldrid's bindings dlopen libvulkan and dlsym every command once, before any VkInstance
 *    exists; OunceVK, like any loader, only hands out most commands for an instance or device. So lx_vk_dlsym (called
 *    by the island's fake libdl) returns one trampoline per command, which resolves through vkGetInstanceProcAddr /
 *    vkGetDeviceProcAddr on its first call, once the instance or device exists, and caches the result.
 *
 * 2. The surface. Veldrid has no swapchain source for VK_NN_vi_surface, so the app passes
 *    SwapchainSource.CreateWayland(1, lx_vi_window()), and this file advertises VK_KHR_wayland_surface, enables
 *    VK_NN_vi_surface in its place, and turns vkCreateWaylandSurfaceKHR into vkCreateViSurfaceNN on that window.
 *
 * 3. Failed acquires and presents are logged with their result.
 *
 * Ported from the HBLauncher shim (hbxl_vk_shim.c) without its workarounds for nn's driver (1080p only, SRGB images
 * with UNORM views, bogus vkGetDeviceProcAddr results, teardown hacks): OunceVK is a Mesa driver and needed none of
 * them in vk-island. The host's wrapper tracks and logs instances, devices and swapchains.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name); /* island runtime -> host */

extern const unsigned lx_vk_count;
extern void *lx_vk_slots[];
extern const char *const lx_vk_names[];
extern const unsigned char lx_vk_device_level[];
extern char lx_vk_tramps[];

static VkInstance g_instance;
static VkDevice g_device;
static PFN_vkGetDeviceProcAddr g_gdpa;

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

static PFN_vkVoidFunction real_gipa(VkInstance instance, const char *name) {
    return vkGetInstanceProcAddr(instance, name);
}

/* ---- trampolines ---------------------------------------------------------------------------------------------- */

void *lx_vk_resolve(unsigned i);
void *lx_vk_resolve(unsigned i) {
    const char *name = i < lx_vk_count ? lx_vk_names[i] : "?";
    void *p = NULL;
    int device_level = i < lx_vk_count && lx_vk_device_level[i];
    if (device_level && g_device && g_gdpa) p = (void *)g_gdpa(g_device, name);
    if (!p && g_instance) p = (void *)real_gipa(g_instance, name);
    if (!p) p = (void *)real_gipa(VK_NULL_HANDLE, name);
    if (!p) {
        vlog("lx-vk: FATAL: %s called but unresolved (instance %p, device %p)", name, (void *)g_instance,
             (void *)g_device);
        abort();
    }
    lx_vk_slots[i] = p;
    return p;
}

static void *tramp_for(const char *name) {
    for (unsigned i = 0; i < lx_vk_count; i++)
        if (strcmp(lx_vk_names[i], name) == 0) return lx_vk_tramps + 32u * i;
    return NULL;
}

/* ---- overrides ------------------------------------------------------------------------------------------------- */

#define WAYLAND_EXT "VK_KHR_wayland_surface"

static VKAPI_ATTR VkResult VKAPI_CALL lx_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                              VkExtensionProperties *props) {
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
        for (uint32_t k = 0; k < n; k++)
            if (!strcmp(list[k].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME)) vi = 1;
        if (vi) {
            memset(&list[n], 0, sizeof list[n]);
            strcpy(list[n].extensionName, WAYLAND_EXT);
            list[n].specVersion = 6;
            n++;
        } else {
            vlog("lx-vk: WARNING: the driver reports no %s", VK_NN_VI_SURFACE_EXTENSION_NAME);
        }
        total = n;
    }
    if (!props) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t copy = *count < total ? *count : total;
    memcpy(props, list, copy * sizeof *props);
    *count = copy;
    return copy < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_CreateInstance(const VkInstanceCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkInstance *out) {
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    const char *names[64];
    VkInstanceCreateInfo copy = *ci;
    uint32_t n = ci->enabledExtensionCount < 64 ? ci->enabledExtensionCount : 64;
    for (uint32_t k = 0; k < n; k++) {
        names[k] = ci->ppEnabledExtensionNames[k];
        if (!strcmp(names[k], WAYLAND_EXT)) names[k] = VK_NN_VI_SURFACE_EXTENSION_NAME;
    }
    copy.ppEnabledExtensionNames = names;
    copy.enabledExtensionCount = n;
    copy.enabledLayerCount = 0; /* no layers on the console */
    copy.ppEnabledLayerNames = NULL;
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(&copy, alloc, out);
    if (r == VK_SUCCESS) g_instance = *out;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyInstance real = (PFN_vkDestroyInstance)real_gipa(instance, "vkDestroyInstance");
    if (real) real(instance, alloc);
    if (instance == g_instance) g_instance = VK_NULL_HANDLE;
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                      const VkAllocationCallbacks *alloc, VkDevice *out) {
    PFN_vkCreateDevice real = (PFN_vkCreateDevice)real_gipa(g_instance, "vkCreateDevice");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(pd, ci, alloc, out);
    if (r == VK_SUCCESS) {
        g_device = *out;
        g_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(g_instance, "vkGetDeviceProcAddr");
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    PFN_vkDestroyDevice real = g_gdpa ? (PFN_vkDestroyDevice)g_gdpa(device, "vkDestroyDevice") : NULL;
    if (!real) real = (PFN_vkDestroyDevice)real_gipa(g_instance, "vkDestroyDevice");
    if (real) real(device, alloc);
    if (device == g_device) g_device = VK_NULL_HANDLE;
}

static void *device_fn(const char *name) {
    void *p = g_gdpa && g_device ? (void *)g_gdpa(g_device, name) : NULL;
    return p ? p : (void *)real_gipa(g_instance, name);
}

/* Swapchain recreation does not work on the console's window: a second swapchain on the same surface failed in
   gfx-probe whether the old one was destroyed after (acquire failed) or before (VK_ERROR_SURFACE_LOST_KHR). The host
   sizes the window before anything is made, but osu-framework still recreates its swapchain once at
   start. So a recreation with the same size and format hands back the old swapchain (HBLauncher did the same), and
   the destroy of the "old" one that follows is swallowed. Anything else goes through (and is logged).
   Veldrid acquires an image right after creating a swapchain, so at a reuse one image is still acquired and not
   presented: the next acquire hands that image back (signalling its semaphore and fence with an empty submit)
   instead of acquiring another one, which would run the swapchain out of images.
   A recreation at another size (docking, the Display settings) goes through. Veldrid holds an acquired image at that
   point; Dusklight (Dawn) resizes on this window with no image held, after a present. So by default the held image is
   presented first (LX_VK_RESIZE=present; `pass` leaves Veldrid's order alone). And since the window's size changes
   before the program has noticed, acquire's VK_SUBOPTIMAL_KHR is reported as VK_SUCCESS: the image presents (scaled)
   and the program recreates at the size the host reports, instead of Veldrid recreating at the old size. */
static VkSwapchainKHR g_sc;
static int64_t g_outstanding = -1; /* acquired, not yet presented */
static int64_t g_pending = -1;     /* to hand back on the next acquire */
static VkQueue g_queue;
static VkExtent2D g_sc_extent;
static VkFormat g_sc_format;
static int g_sc_swallow;

static VKAPI_ATTR VkResult VKAPI_CALL lx_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                            const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    static PFN_vkCreateSwapchainKHR real;
    if (!real) real = (PFN_vkCreateSwapchainKHR)device_fn("vkCreateSwapchainKHR");
    static int keep = -1;
    if (keep < 0) {
        const char *e = getenv("LX_VK_KEEP_SWAPCHAIN");
        keep = !(e && e[0] == '0');
    }
    if (keep && ci->oldSwapchain != VK_NULL_HANDLE && ci->oldSwapchain == g_sc &&
        ci->imageExtent.width == g_sc_extent.width && ci->imageExtent.height == g_sc_extent.height &&
        ci->imageFormat == g_sc_format) {
        g_sc_swallow++;
        g_pending = g_outstanding;
        vlog("lx-vk: swapchain recreation at %ux%u: keeping the one swapchain", ci->imageExtent.width,
             ci->imageExtent.height);
        *out = g_sc;
        return VK_SUCCESS;
    }
    if (ci->oldSwapchain != VK_NULL_HANDLE) {
        static int present_first = -1;
        if (present_first < 0) {
            const char *e = getenv("LX_VK_RESIZE");
            present_first = !(e && strcmp(e, "pass") == 0);
        }
        const char *how = "";
        VkResult pr = VK_SUCCESS;
        if (present_first && ci->oldSwapchain == g_sc && g_outstanding >= 0 && g_queue) {
            static PFN_vkQueuePresentKHR present;
            static PFN_vkQueueWaitIdle wait_idle;
            if (!present) present = (PFN_vkQueuePresentKHR)device_fn("vkQueuePresentKHR");
            if (!wait_idle) wait_idle = (PFN_vkQueueWaitIdle)device_fn("vkQueueWaitIdle");
            uint32_t index = (uint32_t)g_outstanding;
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &g_sc, &index, NULL };
            pr = present(g_queue, &pi);
            wait_idle(g_queue);
            g_outstanding = g_pending = -1;
            how = ", held image presented first";
        }
        vlog("lx-vk: swapchain recreation %ux%u -> %ux%u (format %d -> %d): passed through%s (%d)", g_sc_extent.width,
             g_sc_extent.height, ci->imageExtent.width, ci->imageExtent.height, (int)g_sc_format, (int)ci->imageFormat,
             how, (int)pr);
    }
    VkResult r = real(device, ci, alloc, out);
    if (r == VK_SUCCESS) {
        g_sc = *out;
        g_sc_extent = ci->imageExtent;
        g_sc_format = ci->imageFormat;
        g_sc_swallow = 0;
        g_outstanding = g_pending = -1;
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc,
                                                         const VkAllocationCallbacks *alloc) {
    static PFN_vkDestroySwapchainKHR real;
    if (!real) real = (PFN_vkDestroySwapchainKHR)device_fn("vkDestroySwapchainKHR");
    if (sc != VK_NULL_HANDLE && sc == g_sc && g_sc_swallow > 0) {
        g_sc_swallow--;
        return;
    }
    if (sc == g_sc) g_sc = VK_NULL_HANDLE;
    real(device, sc, alloc);
}

/* Failed acquires and presents are logged (the first 20) with their result. */
static int g_present_errors;

static VKAPI_ATTR VkResult VKAPI_CALL lx_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR sc, uint64_t timeout,
                                                             VkSemaphore sem, VkFence fence, uint32_t *index) {
    static PFN_vkAcquireNextImageKHR real;
    if (!real) real = (PFN_vkAcquireNextImageKHR)device_fn("vkAcquireNextImageKHR");
    if (sc == g_sc && g_pending >= 0 && g_queue) {
        static PFN_vkQueueSubmit submit;
        if (!submit) submit = (PFN_vkQueueSubmit)device_fn("vkQueueSubmit");
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, sem ? 1u : 0u, sem ? &sem : NULL };
        VkResult sr = submit(g_queue, 1, &si, fence);
        vlog("lx-vk: vkAcquireNextImageKHR -> image %u again (acquired before the reuse), signal: %d",
             (unsigned)g_pending, (int)sr);
        if (sr != VK_SUCCESS) return sr;
        *index = (uint32_t)g_pending;
        g_pending = -1;
        return VK_SUCCESS;
    }
    VkResult r = real(device, sc, timeout, sem, fence, index);
    if (r == VK_SUBOPTIMAL_KHR) {
        static int logged;
        if (!logged++) vlog("lx-vk: vkAcquireNextImageKHR -> VK_SUBOPTIMAL_KHR, reported as VK_SUCCESS (logged once)");
        r = VK_SUCCESS;
    }
    if (r == VK_SUCCESS && sc == g_sc) g_outstanding = *index;
    if (r != VK_SUCCESS && g_present_errors++ < 20) vlog("lx-vk: vkAcquireNextImageKHR -> %d", (int)r);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    static PFN_vkQueuePresentKHR real;
    if (!real) real = (PFN_vkQueuePresentKHR)device_fn("vkQueuePresentKHR");
    VkResult r = real(queue, info);
    for (uint32_t k = 0; k < info->swapchainCount; k++)
        if (info->pSwapchains[k] == g_sc && (int64_t)info->pImageIndices[k] == g_outstanding) g_outstanding = -1;
    if (r != VK_SUCCESS && g_present_errors++ < 20) vlog("lx-vk: vkQueuePresentKHR -> %d", (int)r);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out) {
    static PFN_vkGetDeviceQueue real;
    if (!real) real = (PFN_vkGetDeviceQueue)device_fn("vkGetDeviceQueue");
    real(device, family, index, out);
    if (!g_queue) g_queue = *out;
}

/* VkWaylandSurfaceCreateInfoKHR without the wayland headers. */
typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    void *surface;
} wayland_ci;

static VKAPI_ATTR VkResult VKAPI_CALL lx_CreateWaylandSurfaceKHR(VkInstance instance, const wayland_ci *ci,
                                                                 const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    PFN_vkCreateViSurfaceNN real = (PFN_vkCreateViSurfaceNN)real_gipa(instance, "vkCreateViSurfaceNN");
    VkViSurfaceCreateInfoNN vi = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, NULL, 0, ci->surface };
    if (!real) {
        vlog("lx-vk: vkCreateViSurfaceNN unavailable");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    return real(instance, &vi, alloc, out);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL lx_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice pd,
                                                                                         uint32_t family, void *display) {
    (void)pd;
    (void)family;
    (void)display;
    return VK_TRUE;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lx_GetInstanceProcAddr(VkInstance instance, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lx_GetDeviceProcAddr(VkDevice device, const char *name);

static void *override_for(const char *name) {
    static const struct {
        const char *name;
        void *fn;
    } table[] = {
        { "vkGetInstanceProcAddr", (void *)lx_GetInstanceProcAddr },
        { "vkGetDeviceProcAddr", (void *)lx_GetDeviceProcAddr },
        { "vkEnumerateInstanceExtensionProperties", (void *)lx_EnumerateInstanceExtensionProperties },
        { "vkCreateInstance", (void *)lx_CreateInstance },
        { "vkDestroyInstance", (void *)lx_DestroyInstance },
        { "vkCreateDevice", (void *)lx_CreateDevice },
        { "vkDestroyDevice", (void *)lx_DestroyDevice },
        { "vkCreateWaylandSurfaceKHR", (void *)lx_CreateWaylandSurfaceKHR },
        { "vkGetPhysicalDeviceWaylandPresentationSupportKHR", (void *)lx_GetPhysicalDeviceWaylandPresentationSupportKHR },
        { "vkCreateSwapchainKHR", (void *)lx_CreateSwapchainKHR },
        { "vkDestroySwapchainKHR", (void *)lx_DestroySwapchainKHR },
        { "vkAcquireNextImageKHR", (void *)lx_AcquireNextImageKHR },
        { "vkGetDeviceQueue", (void *)lx_GetDeviceQueue },
        { "vkQueuePresentKHR", (void *)lx_QueuePresentKHR },
    };
    for (size_t k = 0; k < sizeof table / sizeof table[0]; k++)
        if (!strcmp(table[k].name, name)) return table[k].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lx_GetInstanceProcAddr(VkInstance instance, const char *name) {
    void *o = override_for(name);
    return o ? (PFN_vkVoidFunction)o : real_gipa(instance, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lx_GetDeviceProcAddr(VkDevice device, const char *name) {
    void *o = override_for(name);
    if (o) return (PFN_vkVoidFunction)o;
    PFN_vkGetDeviceProcAddr gdpa = g_gdpa ? g_gdpa : (PFN_vkGetDeviceProcAddr)real_gipa(g_instance, "vkGetDeviceProcAddr");
    return gdpa ? gdpa(device, name) : NULL;
}

/* Called by the island's fake libdl for dlsym(libvulkan, name). */
void *lx_vk_dlsym(const char *name) {
    void *p = override_for(name);
    if (!p && (!strcmp(name, "vkEnumerateInstanceVersion") || !strcmp(name, "vkEnumerateInstanceLayerProperties")))
        p = (void *)real_gipa(VK_NULL_HANDLE, name);
    if (!p) p = tramp_for(name);
    return p;
}
