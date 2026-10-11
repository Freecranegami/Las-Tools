#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <android/native_window_jni.h>
#include <adrenotools/driver.h>

static void *g_custom = NULL;

static void *open_vk(const char *tmp, const char *hook, const char *dir,
                     const char *name, int custom) {
    if (!custom) return dlopen("libvulkan.so", RTLD_NOW);
    if (!g_custom) {
        g_custom = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                              tmp, hook, dir, name, NULL, NULL);
    }
    return g_custom;
}

/* ---------- simple probe: instance + device name ---------- */

static void probe_run(char *out, size_t n, void *vk) {
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { snprintf(out, n, "no vkGetInstanceProcAddr"); return; }

    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst;
    VkResult r = create(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { snprintf(out, n, "vkCreateInstance failed: %d", (int)r); return; }

    PFN_vkEnumeratePhysicalDevices enumd =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");

    uint32_t count = 1;
    VkPhysicalDevice dev;
    enumd(inst, &count, &dev);
    if (count == 0) { snprintf(out, n, "no physical devices"); return; }

    VkPhysicalDeviceProperties p;
    props(dev, &p);
    snprintf(out, n, "%s\nVulkan %u.%u.%u\ndriverVersion 0x%x",
             p.deviceName,
             VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion),
             VK_API_VERSION_PATCH(p.apiVersion),
             p.driverVersion);
}

/* ---------- render test: swapchain + colour fade ---------- */

#define STEP(label, expr) do { VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { snprintf(out, n, "%s failed: %d", label, (int)_r); return; } \
} while (0)

#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static void render_run(char *out, size_t n, ANativeWindow *win, void *vk, int frames) {
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { snprintf(out, n, "no vkGetInstanceProcAddr"); return; }
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

    /* instance */
    const char *iexts[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = iexts,
    };
    VkInstance inst;
    STEP("vkCreateInstance", vkCreateInstance(&ici, NULL, &inst));

    IFN(vkCreateAndroidSurfaceKHR);
    IFN(vkDestroySurfaceKHR);
    IFN(vkDestroyInstance);
    IFN(vkEnumeratePhysicalDevices);
    IFN(vkGetPhysicalDeviceProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceSurfaceSupportKHR);
    IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    IFN(vkCreateDevice);
    IFN(vkGetDeviceProcAddr);

    /* surface */
    VkAndroidSurfaceCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = win,
    };
    VkSurfaceKHR surf;
    STEP("vkCreateAndroidSurfaceKHR", vkCreateAndroidSurfaceKHR(inst, &sci, NULL, &surf));

    /* physical device + queue family */
    uint32_t pc = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &pc, &pd);
    if (pc == 0) { snprintf(out, n, "no physical devices"); return; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);

    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, NULL);
    if (qc > 16) qc = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qp);
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < qc; i++) {
        VkBool32 sup = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &sup);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { qf = i; break; }
    }
    if (qf == UINT32_MAX) { snprintf(out, n, "no graphics+present queue"); return; }

    /* device */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qf,
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
    VkDevice dev;
    STEP("vkCreateDevice", vkCreateDevice(pd, &dci, NULL, &dev));

    DFN(vkGetDeviceQueue);
    DFN(vkDestroyDevice);
    DFN(vkDeviceWaitIdle);
    DFN(vkCreateSwapchainKHR);
    DFN(vkDestroySwapchainKHR);
    DFN(vkGetSwapchainImagesKHR);
    DFN(vkAcquireNextImageKHR);
    DFN(vkQueuePresentKHR);
    DFN(vkQueueSubmit);
    DFN(vkQueueWaitIdle);
    DFN(vkCreateSemaphore);
    DFN(vkDestroySemaphore);
    DFN(vkCreateCommandPool);
    DFN(vkDestroyCommandPool);
    DFN(vkAllocateCommandBuffers);
    DFN(vkResetCommandBuffer);
    DFN(vkBeginCommandBuffer);
    DFN(vkEndCommandBuffer);
    DFN(vkCmdPipelineBarrier);
    DFN(vkCmdClearColorImage);

    /* swapchain */
    VkSurfaceCapabilitiesKHR caps;
    STEP("surface capabilities",
         vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        snprintf(out, n, "swapchain images can't be TRANSFER_DST");
        return;
    }

    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fc, NULL);
    if (fc > 32) fc = 32;
    VkSurfaceFormatKHR fmts[32];
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fc, fmts);
    if (fc == 0) { snprintf(out, n, "no surface formats"); return; }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 640; ext.height = 360; }

    uint32_t ic = caps.minImageCount + 1;
    if (caps.maxImageCount && ic > caps.maxImageCount) ic = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ca)) ca = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainCreateInfoKHR sc = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf,
        .minImageCount = ic,
        .imageFormat = fmts[0].format,
        .imageColorSpace = fmts[0].colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = ca,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swap;
    STEP("vkCreateSwapchainKHR", vkCreateSwapchainKHR(dev, &sc, NULL, &swap));

    uint32_t imc = 0;
    vkGetSwapchainImagesKHR(dev, swap, &imc, NULL);
    if (imc > 8) imc = 8;
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(dev, swap, &imc, imgs);

    /* command buffer + sync */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qf,
    };
    VkCommandPool pool;
    STEP("vkCreateCommandPool", vkCreateCommandPool(dev, &cpci, NULL, &pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb;
    STEP("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore sem_acquire, sem_render;
    STEP("vkCreateSemaphore", vkCreateSemaphore(dev, &semi, NULL, &sem_acquire));
    STEP("vkCreateSemaphore", vkCreateSemaphore(dev, &semi, NULL, &sem_render));

    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    /* frame loop: clear the image to a fading colour and present it */
    VkImageSubresourceRange rng = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    for (int f = 0; f < frames; f++) {
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, sem_acquire,
                                           VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            snprintf(out, n, "acquire failed: %d at frame %d", (int)r, f);
            return;
        }

        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cb, &bi);

        VkImageMemoryBarrier b1 = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imgs[idx],
            .subresourceRange = rng,
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, NULL, 0, NULL, 1, &b1);

        float t = (float)f / (float)frames;
        VkClearColorValue cc = {{ t, 1.0f - t, 0.5f, 1.0f }};
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &cc, 1, &rng);

        VkImageMemoryBarrier b2 = b1;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = 0;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, NULL, 0, NULL, 1, &b2);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sem_acquire,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &cb,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &sem_render,
        };
        r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) {
            snprintf(out, n, "queue submit failed: %d at frame %d", (int)r, f);
            return;
        }

        VkPresentInfoKHR pinfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sem_render,
            .swapchainCount = 1,
            .pSwapchains = &swap,
            .pImageIndices = &idx,
        };
        r = vkQueuePresentKHR(queue, &pinfo);
        if (r < 0) {
            snprintf(out, n, "present failed: %d at frame %d", (int)r, f);
            return;
        }
        vkQueueWaitIdle(queue);
    }

    /* cleanup */
    vkDeviceWaitIdle(dev);
    vkDestroySemaphore(dev, sem_render, NULL);
    vkDestroySemaphore(dev, sem_acquire, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroySwapchainKHR(dev, swap, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);

    snprintf(out, n, "OK: %d frames presented\n%s\nVulkan %u.%u.%u",
             frames, props.deviceName,
             VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));
}

/* ---------- JNI entry points ---------- */

JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Native_probe(JNIEnv *env, jobject thiz,
                                           jstring jtmp, jstring jhook,
                                           jstring jdir, jstring jname,
                                           jboolean custom) {
    char out[1024];
    const char *tmp  = (*env)->GetStringUTFChars(env, jtmp, NULL);
    const char *hook = (*env)->GetStringUTFChars(env, jhook, NULL);
    const char *dir  = (*env)->GetStringUTFChars(env, jdir, NULL);
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);

    void *vk = open_vk(tmp, hook, dir, name, custom);
    if (!vk) snprintf(out, sizeof out, "open failed: %s", dlerror());
    else probe_run(out, sizeof out, vk);

    (*env)->ReleaseStringUTFChars(env, jtmp, tmp);
    (*env)->ReleaseStringUTFChars(env, jhook, hook);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Native_render(JNIEnv *env, jobject thiz,
                                            jobject surface,
                                            jstring jtmp, jstring jhook,
                                            jstring jdir, jstring jname,
                                            jboolean custom) {
    char out[1024];
    const char *tmp  = (*env)->GetStringUTFChars(env, jtmp, NULL);
    const char *hook = (*env)->GetStringUTFChars(env, jhook, NULL);
    const char *dir  = (*env)->GetStringUTFChars(env, jdir, NULL);
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);

    void *vk = open_vk(tmp, hook, dir, name, custom);
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    if (!vk) snprintf(out, sizeof out, "open failed: %s", dlerror());
    else if (!win) snprintf(out, sizeof out, "no native window");
    else render_run(out, sizeof out, win, vk, 240);
    if (win) ANativeWindow_release(win);

    (*env)->ReleaseStringUTFChars(env, jtmp, tmp);
    (*env)->ReleaseStringUTFChars(env, jhook, hook);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (*env)->NewStringUTF(env, out);
}
