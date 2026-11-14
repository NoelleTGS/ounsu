/*
 * Vulkan for the island: OunceVK's vkGetInstanceProcAddr, wrapped so the host knows which instances, devices,
 * swapchains and surfaces are open.
 *
 * The loader runs hbmenu in this same process after the program exits, and a Vulkan instance left open keeps the
 * GPU's device files open: the next program finds no GPU (Dusklight: black screen after quitting). So at exit the host
 * destroys whatever the island left open (lx_vulkan_destroy_leftovers), devices first, after the GPU is idle.
 *
 * Adapted from Dusklight's hen/platform/hen_vulkan.c (CC0).
 */
#include <switch.h>

#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

#include <string.h>

#include "lx_host.h"

#define MAX_OBJECTS 4

static PFN_vkCreateInstance s_vkCreateInstance;
static PFN_vkDestroyInstance s_vkDestroyInstance;
static PFN_vkCreateDevice s_vkCreateDevice;
static PFN_vkDestroyDevice s_vkDestroyDevice;
static PFN_vkGetDeviceProcAddr s_getDeviceProcAddr;
static PFN_vkCreateSwapchainKHR s_vkCreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR s_vkDestroySwapchainKHR;
static PFN_vkCreateViSurfaceNN s_vkCreateViSurfaceNN;
static PFN_vkDestroySurfaceKHR s_vkDestroySurfaceKHR;
static int s_instances, s_devices, s_swapchains, s_surfaces;
static VkInstance s_openInstances[MAX_OBJECTS];
static VkDevice s_openDevices[MAX_OBJECTS];
static Mutex s_lock;
/* Swapchains and surfaces with their parents, so leftovers can be destroyed in a valid order at exit. */
static struct {
    VkSwapchainKHR handle;
    VkDevice device;
} s_openSwapchains[MAX_OBJECTS];
static struct {
    VkSurfaceKHR handle;
    VkInstance instance;
} s_openSurfaces[MAX_OBJECTS];

static void track(void **list, void *handle, int add) {
    mutexLock(&s_lock);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (add ? list[i] == NULL : list[i] == handle) {
            list[i] = add ? handle : NULL;
            break;
        }
    }
    mutexUnlock(&s_lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_vkCreateInstance(const VkInstanceCreateInfo *info,
                                                          const VkAllocationCallbacks *alloc, VkInstance *out) {
    VkResult r = s_vkCreateInstance(info, alloc, out);
    if (r == VK_SUCCESS) {
        track((void **)s_openInstances, *out, 1);
        lx_logf("lx-vk: vkCreateInstance (%d open)", ++s_instances);
    } else {
        lx_logf("lx-vk: vkCreateInstance failed: %d", r);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    if (instance != VK_NULL_HANDLE) {
        track((void **)s_openInstances, instance, 0);
        lx_logf("lx-vk: vkDestroyInstance (%d open)", --s_instances);
    }
    s_vkDestroyInstance(instance, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkDevice *out) {
    VkResult r = s_vkCreateDevice(physical, info, alloc, out);
    if (r == VK_SUCCESS) {
        track((void **)s_openDevices, *out, 1);
        lx_logf("lx-vk: vkCreateDevice (%d open)", ++s_devices);
    } else {
        lx_logf("lx-vk: vkCreateDevice failed: %d", r);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    if (device != VK_NULL_HANDLE) {
        track((void **)s_openDevices, device, 0);
        lx_logf("lx-vk: vkDestroyDevice (%d open)", --s_devices);
    }
    s_vkDestroyDevice(device, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                              const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    VkResult r = s_vkCreateSwapchainKHR(device, info, alloc, out);
    if (r == VK_SUCCESS) {
        mutexLock(&s_lock);
        for (int i = 0; i < MAX_OBJECTS; i++) {
            if (s_openSwapchains[i].handle == VK_NULL_HANDLE) {
                s_openSwapchains[i].handle = *out;
                s_openSwapchains[i].device = device;
                break;
            }
        }
        mutexUnlock(&s_lock);
    }
    lx_logf("lx-vk: vkCreateSwapchainKHR %ux%u format %d, %u images, present mode %d%s: %d (%d open)",
            info->imageExtent.width, info->imageExtent.height, info->imageFormat, info->minImageCount,
            info->presentMode, info->oldSwapchain ? ", replacing one" : "", r,
            r == VK_SUCCESS ? ++s_swapchains : s_swapchains);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks *alloc) {
    if (swapchain != VK_NULL_HANDLE) {
        mutexLock(&s_lock);
        for (int i = 0; i < MAX_OBJECTS; i++)
            if (s_openSwapchains[i].handle == swapchain) s_openSwapchains[i].handle = VK_NULL_HANDLE;
        mutexUnlock(&s_lock);
        lx_logf("lx-vk: vkDestroySwapchainKHR (%d open)", --s_swapchains);
    }
    s_vkDestroySwapchainKHR(device, swapchain, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL lx_vkCreateViSurfaceNN(VkInstance instance, const VkViSurfaceCreateInfoNN *info,
                                                             const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    VkResult r = s_vkCreateViSurfaceNN(instance, info, alloc, out);
    if (r == VK_SUCCESS) {
        mutexLock(&s_lock);
        for (int i = 0; i < MAX_OBJECTS; i++) {
            if (s_openSurfaces[i].handle == VK_NULL_HANDLE) {
                s_openSurfaces[i].handle = *out;
                s_openSurfaces[i].instance = instance;
                break;
            }
        }
        mutexUnlock(&s_lock);
    }
    lx_logf("lx-vk: vkCreateViSurfaceNN: %d (%d open)", r, r == VK_SUCCESS ? ++s_surfaces : s_surfaces);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL lx_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                         const VkAllocationCallbacks *alloc) {
    if (surface != VK_NULL_HANDLE) {
        mutexLock(&s_lock);
        for (int i = 0; i < MAX_OBJECTS; i++)
            if (s_openSurfaces[i].handle == surface) s_openSurfaces[i].handle = VK_NULL_HANDLE;
        mutexUnlock(&s_lock);
        lx_logf("lx-vk: vkDestroySurfaceKHR (%d open)", --s_surfaces);
    }
    s_vkDestroySurfaceKHR(instance, surface, alloc);
}

static PFN_vkVoidFunction wrap(const char *name, PFN_vkVoidFunction real);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lx_vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return wrap(name, s_getDeviceProcAddr(device, name));
}

void *lx_vk_get_instance_proc_addr(void *instance, const char *name) {
    return (void *)wrap(name, vkGetInstanceProcAddr((VkInstance)instance, name));
}

#define WRAP(fn)                                       \
    if (strcmp(name, #fn) == 0) {                      \
        s_##fn = (PFN_##fn)real;                       \
        return (PFN_vkVoidFunction)lx_##fn;            \
    }

/* Remember the real function and return our wrapper for the calls we track; the real function otherwise. */
static PFN_vkVoidFunction wrap(const char *name, PFN_vkVoidFunction real) {
    if (!real || !name) return real;
    WRAP(vkCreateInstance)
    WRAP(vkDestroyInstance)
    WRAP(vkCreateDevice)
    WRAP(vkDestroyDevice)
    WRAP(vkCreateSwapchainKHR)
    WRAP(vkDestroySwapchainKHR)
    WRAP(vkCreateViSurfaceNN)
    WRAP(vkDestroySurfaceKHR)
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)lx_vk_get_instance_proc_addr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        s_getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)real;
        return (PFN_vkVoidFunction)lx_vkGetDeviceProcAddr;
    }
    return real;
}

void lx_vulkan_destroy_leftovers(void) {
    /* The real functions are only known once the island looked them up, and osu! never destroys its device: resolve
       what's missing now (run 13 died calling a NULL vkDestroyDevice). */
    for (int i = 0; i < MAX_OBJECTS && s_openInstances[i] != VK_NULL_HANDLE; i++) {
        VkInstance inst = s_openInstances[i];
        if (!s_vkDestroyInstance) s_vkDestroyInstance = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(inst, "vkDestroyInstance");
        if (!s_vkDestroySurfaceKHR)
            s_vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(inst, "vkDestroySurfaceKHR");
        if (!s_getDeviceProcAddr)
            s_getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(inst, "vkGetDeviceProcAddr");
        if (!s_vkDestroyDevice) s_vkDestroyDevice = (PFN_vkDestroyDevice)vkGetInstanceProcAddr(inst, "vkDestroyDevice");
        if (!s_vkDestroySwapchainKHR)
            s_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)vkGetInstanceProcAddr(inst, "vkDestroySwapchainKHR");
    }
    if (!s_vkDestroyDevice || !s_vkDestroySwapchainKHR || !s_vkDestroySurfaceKHR || !s_vkDestroyInstance)
        lx_logf("lx-vk: at exit: destroy functions missing (device %d, swapchain %d, surface %d, instance %d)",
                !!s_vkDestroyDevice, !!s_vkDestroySwapchainKHR, !!s_vkDestroySurfaceKHR, !!s_vkDestroyInstance);
    if (s_swapchains || s_surfaces)
        lx_logf("lx-vk: at exit: %d swapchain(s), %d surface(s) still open", s_swapchains, s_surfaces);
    for (int i = 0; i < MAX_OBJECTS; i++) {
        VkDevice device = s_openDevices[i];
        if (device != VK_NULL_HANDLE && s_getDeviceProcAddr && s_vkDestroyDevice) {
            PFN_vkDeviceWaitIdle wait_idle = (PFN_vkDeviceWaitIdle)s_getDeviceProcAddr(device, "vkDeviceWaitIdle");
            if (wait_idle) wait_idle(device);
            /* Its swapchains first (osu-framework leaves the device and its swapchain open at exit; run 12 died
               destroying the device under a live swapchain). */
            for (int k = 0; k < MAX_OBJECTS; k++) {
                if (s_openSwapchains[k].handle != VK_NULL_HANDLE && s_openSwapchains[k].device == device &&
                    s_vkDestroySwapchainKHR) {
                    lx_logf("lx-vk: destroying a swapchain left open");
                    lx_vkDestroySwapchainKHR(device, s_openSwapchains[k].handle, NULL);
                }
            }
            lx_logf("lx-vk: destroying a Vulkan device left open");
            lx_vkDestroyDevice(device, NULL);
        }
    }
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (s_openSurfaces[i].handle != VK_NULL_HANDLE && s_vkDestroySurfaceKHR) {
            lx_logf("lx-vk: destroying a surface left open");
            lx_vkDestroySurfaceKHR(s_openSurfaces[i].instance, s_openSurfaces[i].handle, NULL);
        }
    }
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (s_openInstances[i] != VK_NULL_HANDLE && s_vkDestroyInstance) {
            lx_logf("lx-vk: destroying a Vulkan instance left open");
            lx_vkDestroyInstance(s_openInstances[i], NULL);
        }
    }
}
