/*
 * vk-island: Vulkan from a musl island (Phase 3, step 1). Built with the aarch64-linux-musl toolchain.
 *
 * The devkit's vulkan-clear example, moved into the island: every Vulkan function comes from vkGetInstanceProcAddr,
 * which the island runtime forwards to OunceVK in the host; the surface is VK_NN_vi_surface on the console's default
 * window (lx_vi_window). Clears the screen to a colour that moves around the colour wheel for 5 seconds, reports the
 * frame timing, and destroys everything (the host destroys what a program leaves open, but this one shouldn't).
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name); /* island runtime */
void *lx_vi_window(void);                                                         /* island runtime */

#define MAX_IMAGES 8
#define SECONDS 5

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkCreateViSurfaceNN) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)    \
    X(vkGetSwapchainImagesKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)          \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)               \
    X(vkResetFences) X(vkAcquireNextImageKHR) X(vkResetCommandBuffer) X(vkBeginCommandBuffer)                      \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkQueueSubmit) X(vkQueuePresentKHR)    \
    X(vkDeviceWaitIdle)

#define DECLARE(fn) static PFN_##fn fn;
static PFN_vkCreateInstance vkCreateInstance;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)

static VkInstance instance;
static VkSurfaceKHR surface;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkSwapchainKHR swapchain;
static uint32_t image_count;
static VkImage images[MAX_IMAGES];
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkSemaphore acquired, rendered;
static VkFence frame_done;

static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        printf("vk-island: %s failed: %d\n", what, r);
        exit(1);
    }
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void setup(void) {
#define LOAD_INSTANCE(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(instance, #fn); if (!fn) { printf("vk-island: no %s\n", #fn); exit(1); }
#define LOAD_DEVICE(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(device, #fn); if (!fn) { printf("vk-island: no %s\n", #fn); exit(1); }
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance) {
        printf("vk-island: no Vulkan from the host\n");
        exit(1);
    }
    const char *instance_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME };
    const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-island",
                                    .apiVersion = VK_API_VERSION_1_3 };
    const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                       .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_ext };
    check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");
    INSTANCE_FUNCS(LOAD_INSTANCE)

    const VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = lx_vi_window() };
    check(vkCreateViSurfaceNN(instance, &sci, NULL, &surface), "vkCreateViSurfaceNN");

    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &gpu);
    if (!count) {
        printf("vk-island: no GPU\n");
        exit(1);
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("vk-island: GPU \"%s\", Vulkan %u.%u.%u, driver 0x%x\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                          .queueCount = 1, .pQueuePriorities = &priority };
    const char *device_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                     .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                                     .ppEnabledExtensionNames = device_ext };
    check(vkCreateDevice(gpu, &dci, NULL, &device), "vkCreateDevice");
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceFormatKHR formats[16];
    uint32_t nformats = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &nformats, formats);
    printf("vk-island: surface %ux%u, %u formats (first %d), images %u..%u\n", caps.currentExtent.width,
           caps.currentExtent.height, nformats, formats[0].format, caps.minImageCount, caps.maxImageCount);

    const VkSwapchainCreateInfoKHR swci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = caps.minImageCount,
        .imageFormat = formats[0].format, .imageColorSpace = formats[0].colorSpace, .imageExtent = caps.currentExtent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    check(vkCreateSwapchainKHR(device, &swci, NULL, &swapchain), "vkCreateSwapchainKHR");
    image_count = MAX_IMAGES;
    check(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images), "vkGetSwapchainImagesKHR");

    const VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    check(vkCreateCommandPool(device, &cpci, NULL, &pool), "vkCreateCommandPool");
    const VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                               .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               .commandBufferCount = 1 };
    check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers");
    const VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    check(vkCreateSemaphore(device, &semci, NULL, &acquired), "vkCreateSemaphore");
    check(vkCreateSemaphore(device, &semci, NULL, &rendered), "vkCreateSemaphore");
    const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    check(vkCreateFence(device, &fci, NULL, &frame_done), "vkCreateFence");
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    const VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src,
                                     .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                                     .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void frame(uint32_t n) {
    check(vkWaitForFences(device, 1, &frame_done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkResetFences(device, 1, &frame_done);
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");

    const float t = (float)n / 60.0f;
    const VkClearColorValue color = { { 0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.094f),
                                        0.5f + 0.5f * sinf(t + 4.189f), 1.0f } };
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    barrier(images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier(images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                  .pWaitSemaphores = &acquired, .pWaitDstStageMask = &stage,
                                  .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1,
                                  .pSignalSemaphores = &rendered };
    check(vkQueueSubmit(queue, 1, &submit, frame_done), "vkQueueSubmit");
    const VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                       .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &swapchain,
                                       .pImageIndices = &index };
    r = vkQueuePresentKHR(queue, &present);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) check(r, "vkQueuePresentKHR");
}

int main(void) {
    printf("vk-island: start\n");
    setup();
    printf("vk-island: swapchain with %u images; drawing for %d s\n", image_count, SECONDS);

    double start = now(), last = start, worst = 0;
    uint32_t n = 0;
    while (now() - start < SECONDS) {
        frame(n++);
        double t = now();
        if (n > 2 && t - last > worst) worst = t - last;
        last = t;
    }
    double elapsed = now() - start;
    printf("vk-island: %u frames in %.2f s = %.1f fps, slowest frame %.1f ms\n", n, elapsed, n / elapsed, worst * 1000);

    vkDeviceWaitIdle(device);
    vkDestroyFence(device, frame_done, NULL);
    vkDestroySemaphore(device, rendered, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    printf("vk-island: done\n");
    return 0;
}
