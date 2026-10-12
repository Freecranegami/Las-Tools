/* A tiny "game": a Vulkan swapchain whose colour fades forever.
   It links libvulkan.so the normal way, like a real game does, and logs which
   GPU / driver it ended up with (tag "GuestGame"). */
#include <android_native_app_glue.h>
#include <android/log.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define TAG "GuestGame"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VK_TRY(call) do { \
    VkResult r_ = (call); \
    if (r_ != VK_SUCCESS) { LOGE("%s failed: %d", #call, (int)r_); return 0; } \
} while (0)

typedef struct {
    int ready;
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice pd;
    VkDevice dev;
    uint32_t qf;
    VkQueue queue;
    VkSwapchainKHR swap;
    VkImage imgs[8];
    uint32_t imc;
    VkExtent2D ext;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkSemaphore sem_acquire;
    VkSemaphore sem_render;
    uint64_t frame;
} Gfx;

static Gfx g;

static void destroy_swapchain(void) {
    if (!g.dev) return;
    vkDeviceWaitIdle(g.dev);
    if (g.sem_render) { vkDestroySemaphore(g.dev, g.sem_render, NULL); g.sem_render = VK_NULL_HANDLE; }
    if (g.sem_acquire) { vkDestroySemaphore(g.dev, g.sem_acquire, NULL); g.sem_acquire = VK_NULL_HANDLE; }
    if (g.swap) { vkDestroySwapchainKHR(g.dev, g.swap, NULL); g.swap = VK_NULL_HANDLE; }
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.pd, g.surf, &caps));
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOGE("swapchain images can't be TRANSFER_DST");
        return 0;
    }

    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.pd, g.surf, &fc, NULL);
    if (fc > 32) fc = 32;
    VkSurfaceFormatKHR fmts[32];
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.pd, g.surf, &fc, fmts);
    if (fc == 0) { LOGE("no surface formats"); return 0; }

    g.ext = caps.currentExtent;
    if (g.ext.width == 0xFFFFFFFFu) { g.ext.width = 640; g.ext.height = 360; }

    uint32_t ic = caps.minImageCount + 1;
    if (caps.maxImageCount && ic > caps.maxImageCount) ic = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ca)) ca = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainCreateInfoKHR sc = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g.surf,
        .minImageCount = ic,
        .imageFormat = fmts[0].format,
        .imageColorSpace = fmts[0].colorSpace,
        .imageExtent = g.ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = ca,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VK_TRY(vkCreateSwapchainKHR(g.dev, &sc, NULL, &g.swap));

    g.imc = 0;
    vkGetSwapchainImagesKHR(g.dev, g.swap, &g.imc, NULL);
    if (g.imc > 8) g.imc = 8;
    vkGetSwapchainImagesKHR(g.dev, g.swap, &g.imc, g.imgs);

    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_TRY(vkCreateSemaphore(g.dev, &semi, NULL, &g.sem_acquire));
    VK_TRY(vkCreateSemaphore(g.dev, &semi, NULL, &g.sem_render));

    LOGI("swapchain %ux%u, %u images", g.ext.width, g.ext.height, g.imc);
    return 1;
}

static int init_vulkan(ANativeWindow *win) {
    memset(&g, 0, sizeof g);

    const char *iexts[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "GuestGame",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = iexts,
    };
    VK_TRY(vkCreateInstance(&ici, NULL, &g.inst));

    VkAndroidSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = win,
    };
    VK_TRY(vkCreateAndroidSurfaceKHR(g.inst, &sci, NULL, &g.surf));

    uint32_t pc = 1;
    VkResult r = vkEnumeratePhysicalDevices(g.inst, &pc, &g.pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || pc == 0) {
        LOGE("no GPU found: %d", (int)r);
        return 0;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.pd, &props);
    LOGI("GPU: %s", props.deviceName);
    LOGI("Vulkan %u.%u.%u, driverVersion 0x%x",
         VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion),
         VK_API_VERSION_PATCH(props.apiVersion),
         props.driverVersion);

    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &qc, NULL);
    if (qc > 16) qc = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &qc, qp);
    g.qf = UINT32_MAX;
    for (uint32_t i = 0; i < qc; i++) {
        VkBool32 sup = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g.pd, i, g.surf, &sup);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { g.qf = i; break; }
    }
    if (g.qf == UINT32_MAX) { LOGE("no graphics+present queue"); return 0; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g.qf,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const char *dexts[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dexts,
    };
    VK_TRY(vkCreateDevice(g.pd, &dci, NULL, &g.dev));
    vkGetDeviceQueue(g.dev, g.qf, 0, &g.queue);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g.qf,
    };
    VK_TRY(vkCreateCommandPool(g.dev, &cpci, NULL, &g.pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_TRY(vkAllocateCommandBuffers(g.dev, &cbai, &g.cb));

    if (!create_swapchain()) return 0;
    LOGI("Vulkan ready");
    return 1;
}

static void term_vulkan(void) {
    if (!g.inst) return;
    if (g.dev) {
        destroy_swapchain();
        if (g.pool) vkDestroyCommandPool(g.dev, g.pool, NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.surf) vkDestroySurfaceKHR(g.inst, g.surf, NULL);
    vkDestroyInstance(g.inst, NULL);
    memset(&g, 0, sizeof g);
    LOGI("Vulkan shut down");
}

static void draw_frame(void) {
    if (!g.swap) { g.ready = 0; return; }

    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(g.dev, g.swap, UINT64_MAX, g.sem_acquire,
                                       VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroy_swapchain();
        if (!create_swapchain()) g.ready = 0;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("acquire failed: %d", (int)r);
        g.ready = 0;
        return;
    }

    vkResetCommandBuffer(g.cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(g.cb, &bi);

    VkImageSubresourceRange rng = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };
    VkImageMemoryBarrier b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g.imgs[idx],
        .subresourceRange = rng,
    };
    vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &b1);

    float t = (float)g.frame * 0.02f;
    VkClearColorValue cc = {{
        0.5f + 0.5f * sinf(t),
        0.5f + 0.5f * sinf(t + 2.094f),
        0.5f + 0.5f * sinf(t + 4.188f),
        1.0f }};
    vkCmdClearColorImage(g.cb, g.imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &cc, 1, &rng);

    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = 0;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &b2);
    vkEndCommandBuffer(g.cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &g.cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &g.sem_render,
    };
    r = vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        LOGE("submit failed: %d", (int)r);
        g.ready = 0;
        return;
    }

    VkPresentInfoKHR pinfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g.sem_render,
        .swapchainCount = 1,
        .pSwapchains = &g.swap,
        .pImageIndices = &idx,
    };
    r = vkQueuePresentKHR(g.queue, &pinfo);
    vkQueueWaitIdle(g.queue);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroy_swapchain();
        if (!create_swapchain()) g.ready = 0;
        return;
    }
    if (r != VK_SUCCESS) {
        LOGE("present failed: %d", (int)r);
        g.ready = 0;
        return;
    }

    g.frame++;
    if (g.frame % 300 == 0) LOGI("frame %llu", (unsigned long long)g.frame);
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window != NULL) {
            g.ready = init_vulkan(app->window);
            LOGI("init_vulkan -> %d", g.ready);
        }
        break;
    case APP_CMD_TERM_WINDOW:
        term_vulkan();
        break;
    default:
        break;
    }
}

void android_main(struct android_app *app) {
    LOGI("android_main started");
    app->onAppCmd = on_cmd;

    for (;;) {
        int events;
        struct android_poll_source *source;
        while (ALooper_pollAll(g.ready ? 0 : -1, NULL, &events, (void **)&source) >= 0) {
            if (source != NULL) source->process(app, source);
            if (app->destroyRequested) {
                term_vulkan();
                return;
            }
        }
        if (g.ready) draw_frame();
    }
}
