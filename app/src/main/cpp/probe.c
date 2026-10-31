#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <android/native_window_jni.h>
#include <android/input.h>
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

/* Loads libvkshim.so and points all its stubs at the custom-driver loader. */
JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Native_bindShim(JNIEnv *env, jobject thiz) {
    char out[256];

    if (!g_custom) {
        snprintf(out, sizeof out, "custom driver is not open");
        return (*env)->NewStringUTF(env, out);
    }
    void *shim = dlopen("libvkshim.so", RTLD_NOW | RTLD_GLOBAL);
    if (!shim) {
        snprintf(out, sizeof out, "dlopen libvkshim.so failed: %s", dlerror());
        return (*env)->NewStringUTF(env, out);
    }
    int (*bind)(void *) = (int (*)(void *))dlsym(shim, "vkshim_bind");
    int (*total)(void) = (int (*)(void))dlsym(shim, "vkshim_total");
    if (!bind || !total) {
        snprintf(out, sizeof out, "shim is missing its bind functions");
        return (*env)->NewStringUTF(env, out);
    }
    int ok = bind(g_custom);
    snprintf(out, sizeof out, "shim bound %d of %d functions", ok, total());
    return (*env)->NewStringUTF(env, out);
}

#include <stdlib.h>

/* Opens the custom driver and publishes its handle so the shim can bind itself. */
JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Native_openCustom(JNIEnv *env, jobject thiz,
                                                jstring jtmp, jstring jhook,
                                                jstring jdir, jstring jname) {
    char out[256];
    const char *tmp  = (*env)->GetStringUTFChars(env, jtmp, NULL);
    const char *hook = (*env)->GetStringUTFChars(env, jhook, NULL);
    const char *dir  = (*env)->GetStringUTFChars(env, jdir, NULL);
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);

    void *vk = open_vk(tmp, hook, dir, name, 1);
    if (!vk) {
        snprintf(out, sizeof out, "open failed: %s", dlerror());
    } else {
        char hex[32];
        snprintf(hex, sizeof hex, "%llx", (unsigned long long)(uintptr_t)vk);
        setenv("VKSHIM_HANDLE", hex, 1);
        snprintf(out, sizeof out, "custom driver open");
    }

    (*env)->ReleaseStringUTFChars(env, jtmp, tmp);
    (*env)->ReleaseStringUTFChars(env, jhook, hook);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (*env)->NewStringUTF(env, out);
}

/* ---------- which classes can a native thread find? ---------- */
#include <pthread.h>
#include <stdarg.h>
#include <android/log.h>
#define GLOG(...) __android_log_print(ANDROID_LOG_INFO, "GuestHost", __VA_ARGS__)

static JavaVM *g_vm = NULL;
static char g_names[512];

static void *thread_probe_main(void *arg) {
    (void)arg;
    JNIEnv *env = NULL;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
    char buf[512];
    strncpy(buf, g_names, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        jclass c = (*env)->FindClass(env, tok);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        GLOG("native thread FindClass(%s) -> %s", tok, c ? "found" : "NOT FOUND");
        if (c) (*env)->DeleteLocalRef(env, c);
    }
    (*g_vm)->DetachCurrentThread(g_vm);
    return NULL;
}

/* Starts a plain native thread and reports which of the comma separated classes it can find. */
JNIEXPORT void JNICALL
Java_com_freecrane_adrenotest_Native_threadProbe(JNIEnv *env, jobject thiz, jstring jnames) {
    const char *n = (*env)->GetStringUTFChars(env, jnames, NULL);
    strncpy(g_names, n, sizeof g_names - 1); g_names[sizeof g_names - 1] = 0;
    (*env)->ReleaseStringUTFChars(env, jnames, n);
    (*env)->GetJavaVM(env, &g_vm);
    pthread_t t;
    if (pthread_create(&t, NULL, thread_probe_main, NULL) == 0) pthread_join(t, NULL);
}

/* ---------- FindClass that also knows the game's classes ----------
 * Native threads attached by the game look classes up with the boot loader, which cannot see
 * a hosted game's classes. We replace FindClass in the JNI function table with a version
 * that falls back to Class.forName(name, true, gameLoader). */
#include <sys/mman.h>
#include <unistd.h>

typedef jclass (*FindClassFn)(JNIEnv *, const char *);
static FindClassFn g_orig_find = NULL;
static jobject g_game_loader = NULL;
static jclass g_class_class = NULL;
static jmethodID g_for_name = NULL;

/* ring buffer of the most recent JNI lookups, dumped when the game passes null to GetObjectClass */
#define RING_N 96
static char g_ring[RING_N][200];
static unsigned g_ring_i = 0;
static void rec(const char *fmt, ...) {
    unsigned i = __atomic_fetch_add(&g_ring_i, 1, __ATOMIC_RELAXED) % RING_N;
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_ring[i], sizeof g_ring[i], fmt, ap);
    va_end(ap);
}
static void ring_dump(void) {
    unsigned n = g_ring_i;
    unsigned start = n > RING_N ? n - RING_N : 0;
    for (unsigned k = start; k < n; k++) GLOG("jni-trace %s", g_ring[k % RING_N]);
}

static jclass hooked_FindClass(JNIEnv *env, const char *name) {
    jclass c = g_orig_find(env, name);
    if (c) { rec("FindClass %s ok", name ? name : "?"); return c; }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!g_game_loader || !name) return NULL;
    char dotted[256];
    size_t i = 0;
    for (; name[i] && i < sizeof dotted - 1; i++) dotted[i] = name[i] == '/' ? '.' : name[i];
    dotted[i] = 0;
    jstring s = (*env)->NewStringUTF(env, dotted);
    jobject r = (*env)->CallStaticObjectMethod(env, g_class_class, g_for_name, s, JNI_TRUE, g_game_loader);
    (*env)->DeleteLocalRef(env, s);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); rec("FindClass %s NOT FOUND", name); return NULL; }
    rec("FindClass %s via game loader %s", name, r ? "ok" : "null");
    return (jclass)r;
}

typedef jmethodID (*GetMidFn)(JNIEnv *, jclass, const char *, const char *);
typedef jfieldID (*GetFidFn)(JNIEnv *, jclass, const char *, const char *);
typedef jobject (*GetSObjFn)(JNIEnv *, jclass, jfieldID);
typedef jobject (*GetObjFn)(JNIEnv *, jobject, jfieldID);
typedef jclass (*GetObjClsFn)(JNIEnv *, jobject);
static GetMidFn o_getmid, o_getsmid;
static GetFidFn o_getfid, o_getsfid;
static GetSObjFn o_getsobj;
static GetObjFn o_getobj;
static GetObjClsFn o_getobjcls;

#define MID_N 1024
static struct { jmethodID id; char txt[96]; } g_mids[MID_N];
static unsigned g_mid_i = 0;
static void mid_note(jmethodID id, const char *n, const char *sg) {
    unsigned i = __atomic_fetch_add(&g_mid_i, 1, __ATOMIC_RELAXED) % MID_N;
    g_mids[i].id = id;
    snprintf(g_mids[i].txt, sizeof g_mids[i].txt, "%s%s", n, sg);
}
static const char *mid_name(jmethodID id) {
    for (int i = 0; i < MID_N; i++) if (g_mids[i].id == id) return g_mids[i].txt;
    return "?";
}
static jclass g_wrap_cls = NULL;
static jmethodID g_wrap_mid = NULL;
static __thread int t_in_wrap = 0;

/* The game's native code reads its asset-pack manager out of a Java field. When that object is read, hand it a
 * manager that answers for packs that are really on the phone (see GuestAssetPacks). This is only safe when the
 * native code calls the manager through the AssetPackManager interface (a proxy is not the concrete class), so it
 * is switched on only after the native code is seen resolving "fetch" on an interface. */
#include <pthread.h>
static jclass g_mgr_cls = NULL;
static jmethodID g_cls_getname = NULL, g_cls_isiface = NULL;
static volatile int g_wrap_ok = 0;
static jobject g_last_orig = NULL, g_last_proxy = NULL;
static pthread_mutex_t g_wrap_lock = PTHREAD_MUTEX_INITIALIZER;

static struct { jmethodID id; const char *name; } g_pack_mids[16];
static int g_pack_n = 0;
static void note_pack_mid(JNIEnv *e, jclass c, const char *n, const char *sg, jmethodID mid) {
    static const char *names[] = {"fetch", "getPackLocation", "getPackStates", "getPackLocations", "getAssetLocation"};
    if (!n || !sg || !c || !g_cls_getname || !g_cls_isiface || t_in_wrap) return;
    int hit = 0;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(n, names[i])) hit = 1;
    if (!hit || !(strstr(sg, "assetpacks") || strstr(sg, "gms/tasks/Task"))) return;
    t_in_wrap = 1;
    jboolean ifc = (*e)->CallBooleanMethod(e, c, g_cls_isiface);
    jstring js = (jstring)(*e)->CallObjectMethod(e, c, g_cls_getname);
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    const char *u = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
    GLOG("asset packs: native code resolved %s%s on %s (interface=%d)", n, sg, u ? u : "?", (int)ifc);
    if (u) (*e)->ReleaseStringUTFChars(e, js, u);
    if (js) (*e)->DeleteLocalRef(e, js);
    if (mid && g_pack_n < 16) {
        const char *keep = !strcmp(n, "fetch") ? "fetch" : !strcmp(n, "getPackStates") ? "getPackStates" :
                           !strcmp(n, "getPackLocation") ? "getPackLocation" : NULL;
        if (keep) { g_pack_mids[g_pack_n].id = mid; g_pack_mids[g_pack_n].name = keep; g_pack_n++; }
    }
    t_in_wrap = 0;
}

static jobject maybe_wrap(JNIEnv *e, jobject r) {
    if (!r || !g_mgr_cls || !g_wrap_ok || t_in_wrap || !g_wrap_cls || !g_wrap_mid) return r;
    if (!(*e)->IsInstanceOf(e, r, g_mgr_cls)) return r;
    jobject out = r;
    pthread_mutex_lock(&g_wrap_lock);
    if (g_last_proxy && (*e)->IsSameObject(e, r, g_last_proxy)) {
        out = r;
    } else if (g_last_orig && g_last_proxy && (*e)->IsSameObject(e, r, g_last_orig)) {
        out = (*e)->NewLocalRef(e, g_last_proxy);
    } else {
        t_in_wrap = 1;
        jobject w = (*e)->CallStaticObjectMethod(e, g_wrap_cls, g_wrap_mid, r);
        t_in_wrap = 0;
        if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); GLOG("asset packs: wrapping the manager threw"); }
        else if (w) {
            if (g_last_orig) (*e)->DeleteGlobalRef(e, g_last_orig);
            if (g_last_proxy) (*e)->DeleteGlobalRef(e, g_last_proxy);
            g_last_orig = (*e)->NewGlobalRef(e, r);
            g_last_proxy = (*e)->NewGlobalRef(e, w);
            GLOG("asset packs: the game's native code now gets the local pack manager");
            out = w;
        }
    }
    pthread_mutex_unlock(&g_wrap_lock);
    return out;
}


/* Answer the pack manager's fetch / getPackStates / getPackLocation from the packs that are on the phone, whatever
 * class the native code resolved the method on. Returns NULL to let the original call go ahead. */
static jmethodID g_local_mid = NULL;
static const char *pack_mid_name(jmethodID m) {
    for (int i = 0; i < g_pack_n; i++) if (g_pack_mids[i].id == m) return g_pack_mids[i].name;
    return NULL;
}
static jobject try_local(JNIEnv *e, jobject o, jmethodID m, jobject arg) {
    const char *nm = pack_mid_name(m);
    if (!nm || !g_wrap_cls || !g_local_mid || t_in_wrap || !g_mgr_cls || !o) return NULL;
    if (!(*e)->IsInstanceOf(e, o, g_mgr_cls)) return NULL;
    t_in_wrap = 1;
    jstring js = (*e)->NewStringUTF(e, nm);
    jobject r = (*e)->CallStaticObjectMethod(e, g_wrap_cls, g_local_mid, o, js, arg);
    t_in_wrap = 0;
    (*e)->DeleteLocalRef(e, js);
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return NULL; }
    return r;
}

static jmethodID h_getmid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jmethodID r = o_getmid(e, c, n, sg); mid_note(r, n, sg); note_pack_mid(e, c, n, sg, r); rec("GetMethodID %s %s -> %p", n, sg, r); return r; }
static jmethodID h_getsmid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jmethodID r = o_getsmid(e, c, n, sg); mid_note(r, n, sg); rec("GetStaticMethodID %s %s -> %p", n, sg, r); return r; }
static jfieldID h_getfid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jfieldID r = o_getfid(e, c, n, sg); rec("GetFieldID %s %s -> %p", n, sg, r); return r; }
static jfieldID h_getsfid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jfieldID r = o_getsfid(e, c, n, sg); rec("GetStaticFieldID %s %s -> %p", n, sg, r); return r; }
static jobject h_getsobj(JNIEnv *e, jclass c, jfieldID f) {
    jobject r = o_getsobj(e, c, f); rec("GetStaticObjectField %p -> %s", f, r ? "obj" : "NULL"); return maybe_wrap(e, r); }
static jobject h_getobj(JNIEnv *e, jobject o, jfieldID f) {
    jobject r = o_getobj(e, o, f); rec("GetObjectField %p -> %s", f, r ? "obj" : "NULL"); return maybe_wrap(e, r); }
static jclass h_getobjcls(JNIEnv *e, jobject o) {
    if (!o) {
        Dl_info di; memset(&di, 0, sizeof di);
        void *ra = __builtin_return_address(0);
        dladdr(ra, &di);
        GLOG("jni-trace GetObjectClass(NULL) from %s+0x%lx", di.dli_fname ? di.dli_fname : "?",
             (unsigned long)((char *)ra - (char *)di.dli_fbase));
        ring_dump();
    }
    return o_getobjcls(e, o);
}

static jobject after_obj(JNIEnv *e, const char *kind, jmethodID m, jobject r) {
    rec("%s %s -> %s", kind, mid_name(m), r ? "obj" : "NULL");
    if (!r && (*e)->ExceptionCheck(e)) {
        jthrowable t = (*e)->ExceptionOccurred(e);
        (*e)->ExceptionClear(e);
        jclass tc = (*e)->FindClass(e, "java/lang/Throwable");
        jmethodID ts = (*e)->GetMethodID(e, tc, "toString", "()Ljava/lang/String;");
        jstring js = (jstring)(*e)->CallObjectMethod(e, t, ts);
        if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        const char *u = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
        rec("   java exception: %.150s", u ? u : "?");
        if (u) (*e)->ReleaseStringUTFChars(e, js, u);
        (*e)->Throw(e, t);
    }
    return r;
}
typedef jobject (*CallSOV)(JNIEnv *, jclass, jmethodID, va_list);
typedef jobject (*CallOV)(JNIEnv *, jobject, jmethodID, va_list);
static CallSOV o_csov; static CallOV o_cov;
static jobject h_csov(JNIEnv *e, jclass c, jmethodID m, va_list ap) {
    jobject r = o_csov(e, c, m, ap); return after_obj(e, "CallStaticObjectMethodV", m, r); }
static jobject h_cov(JNIEnv *e, jobject o, jmethodID m, va_list ap) {
    if (pack_mid_name(m)) {
        va_list a2; va_copy(a2, ap); jobject arg = va_arg(a2, jobject); va_end(a2);
        jobject l = try_local(e, o, m, arg); if (l) return l;
    }
    jobject r = o_cov(e, o, m, ap); return after_obj(e, "CallObjectMethodV", m, r); }
static jobject h_csom(JNIEnv *e, jclass c, jmethodID m, ...) {
    va_list ap; va_start(ap, m);
    jobject r = o_csov ? o_csov(e, c, m, ap) : (*e)->CallStaticObjectMethodV(e, c, m, ap);
    va_end(ap); return after_obj(e, "CallStaticObjectMethod", m, r);
}
static jobject h_com(JNIEnv *e, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m);
    if (pack_mid_name(m)) {
        va_list a2; va_copy(a2, ap); jobject arg = va_arg(a2, jobject); va_end(a2);
        jobject l = try_local(e, o, m, arg); if (l) { va_end(ap); return l; }
    }
    jobject r = o_cov ? o_cov(e, o, m, ap) : (*e)->CallObjectMethodV(e, o, m, ap);
    va_end(ap); return after_obj(e, "CallObjectMethod", m, r);
}
typedef jobject (*CallSOA)(JNIEnv *, jclass, jmethodID, const jvalue *);
typedef jobject (*CallOA)(JNIEnv *, jobject, jmethodID, const jvalue *);
static CallSOA o_csoa; static CallOA o_coa;
static jobject h_csoa(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) {
    jobject r = o_csoa(e, c, m, a); return after_obj(e, "CallStaticObjectMethodA", m, r); }
static jobject h_coa(JNIEnv *e, jobject o, jmethodID m, const jvalue *a) {
    if (pack_mid_name(m) && a) { jobject l = try_local(e, o, m, a[0].l); if (l) return l; }
    jobject r = o_coa(e, o, m, a); return after_obj(e, "CallObjectMethodA", m, r); }

static int patch_slot(void **slot, void *fn, void **orig) {
    if (*slot == fn) return 0;
    long pg = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)slot & ~(uintptr_t)(pg - 1));
    if (mprotect(page, pg, PROT_READ | PROT_WRITE) != 0) return -1;
    *orig = *slot;
    *slot = fn;
    mprotect(page, pg, PROT_READ);
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Native_installFindClassHook(JNIEnv *env, jobject thiz, jobject loader) {
    if (g_game_loader) {
        (*env)->DeleteGlobalRef(env, g_game_loader);
    }
    g_game_loader = (*env)->NewGlobalRef(env, loader);
    if (!g_class_class) {
        jclass k = (*env)->FindClass(env, "java/lang/Class");
        g_class_class = (jclass)(*env)->NewGlobalRef(env, k);
        g_for_name = (*env)->GetStaticMethodID(env, k, "forName",
            "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    }
    if (!g_wrap_cls) {
        jclass w = (*env)->FindClass(env, "com/freecrane/adrenotest/GuestAssetPacks");
        if (w) {
            g_wrap_cls = (jclass)(*env)->NewGlobalRef(env, w);
            g_wrap_mid = (*env)->GetStaticMethodID(env, w, "wrap", "(Ljava/lang/Object;)Ljava/lang/Object;");
            g_local_mid = (*env)->GetStaticMethodID(env, w, "localCall",
                "(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Ljava/lang/Object;");
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    if (!g_cls_getname) {
        g_cls_getname = (*env)->GetMethodID(env, g_class_class, "getName", "()Ljava/lang/String;");
        g_cls_isiface = (*env)->GetMethodID(env, g_class_class, "isInterface", "()Z");
    }
    if (!g_mgr_cls) {
        jstring nm = (*env)->NewStringUTF(env, "com.google.android.play.core.assetpacks.AssetPackManager");
        jobject k = (*env)->CallStaticObjectMethod(env, g_class_class, g_for_name, nm, JNI_TRUE, g_game_loader);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        else if (k) g_mgr_cls = (jclass)(*env)->NewGlobalRef(env, k);
        (*env)->DeleteLocalRef(env, nm);
        GLOG("asset packs: AssetPackManager class %s", g_mgr_cls ? "found" : "not found");
    }
    void **slot = (void **)&(*env)->FindClass;
    if (*slot == (void *)hooked_FindClass) return (*env)->NewStringUTF(env, "FindClass hook already installed");
    if (patch_slot(slot, (void *)hooked_FindClass, (void **)&g_orig_find) != 0)
        return (*env)->NewStringUTF(env, "FindClass hook failed: mprotect");
    /* Build 32 ran with these hooks on and was the one build that showed a gradient; the builds
     * without them stayed black. They are back on while we find out why. */
    patch_slot((void **)&(*env)->GetMethodID, (void *)h_getmid, (void **)&o_getmid);
    patch_slot((void **)&(*env)->GetStaticMethodID, (void *)h_getsmid, (void **)&o_getsmid);
    patch_slot((void **)&(*env)->GetFieldID, (void *)h_getfid, (void **)&o_getfid);
    patch_slot((void **)&(*env)->GetStaticFieldID, (void *)h_getsfid, (void **)&o_getsfid);
    /* Build 41: back on (after_obj now returns its result), so the ring shows which Java methods the game
     * calls from its asset-pack polling loop. The snapshot dumps the ring. */
    patch_slot((void **)&(*env)->GetStaticObjectField, (void *)h_getsobj, (void **)&o_getsobj);
    patch_slot((void **)&(*env)->GetObjectField, (void *)h_getobj, (void **)&o_getobj);
    patch_slot((void **)&(*env)->GetObjectClass, (void *)h_getobjcls, (void **)&o_getobjcls);
    patch_slot((void **)&(*env)->CallStaticObjectMethodV, (void *)h_csov, (void **)&o_csov);
    patch_slot((void **)&(*env)->CallObjectMethodV, (void *)h_cov, (void **)&o_cov);
    { void *dummy;
      patch_slot((void **)&(*env)->CallStaticObjectMethod, (void *)h_csom, &dummy);
      patch_slot((void **)&(*env)->CallObjectMethod, (void *)h_com, &dummy);
      patch_slot((void **)&(*env)->CallStaticObjectMethodA, (void *)h_csoa, (void **)&o_csoa);
      patch_slot((void **)&(*env)->CallObjectMethodA, (void *)h_coa, (void **)&o_coa); }
    return (*env)->NewStringUTF(env, "FindClass hook installed");
}


/* ---- Build 38: thread snapshot, to see where the game's threads are stuck ---- */
#include <unwind.h>
#include <signal.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <time.h>

static void *g_bt[48];
static volatile int g_bt_n = 0;
static volatile int g_bt_done = 0;

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *c, void *arg) {
    uintptr_t pc = _Unwind_GetIP(c);
    if (pc && g_bt_n < 48) g_bt[g_bt_n++] = (void *)pc;
    return g_bt_n >= 48 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
static void bt_handler(int sig) {
    g_bt_n = 0;
    _Unwind_Backtrace(bt_cb, NULL);
    g_bt_done = 1;
}

static void read_small(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t r = fread(out, 1, n - 1, f);
    out[r] = 0;
    fclose(f);
    for (size_t i = 0; i < r; i++) if (out[i] == '\n') out[i] = ' ';
}

JNIEXPORT void JNICALL
Java_com_freecrane_adrenotest_Native_snapshot(JNIEnv *env, jobject thiz, jstring jtag) {
    const char *tag = (*env)->GetStringUTFChars(env, jtag, NULL);
    GLOG("snap[%s]: begin", tag);

    /* 1. graphics-related libraries the process has loaded */
    FILE *m = fopen("/proc/self/maps", "r");
    if (m) {
        char line[600], seen[40][300]; int ns = 0;
        while (fgets(line, sizeof line, m)) {
            char *p = strchr(line, '/');
            if (!p) continue;
            p[strcspn(p, "\n")] = 0;
            if (!(strstr(p, "vulkan") || strstr(p, "libEGL") || strstr(p, "GLES") || strstr(p, "turnip") ||
                  strstr(p, "vk_") || strstr(p, "adreno") || strstr(p, "libgsl") || strstr(p, "librdr") ||
                  strstr(p, "libplaycore") || strstr(p, "gpu") || strstr(p, "libOpenSLES") || strstr(p, "libaaudio")))
                continue;
            int dup = 0;
            for (int i = 0; i < ns; i++) if (!strcmp(seen[i], p)) { dup = 1; break; }
            if (dup || ns >= 40) continue;
            snprintf(seen[ns++], 300, "%s", p);
            { const char *b = strrchr(p, '/'); GLOG("snap[%s]: lib %s", tag, b ? b + 1 : p); }
        }
        fclose(m);
    }

    /* 2. every thread: name, state, kernel wait point, native stack */
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = bt_handler; sigemptyset(&sa.sa_mask);
    int sig = SIGRTMIN + 4;
    sigaction(sig, &sa, NULL);
    pid_t pid = getpid(), me = (pid_t)syscall(SYS_gettid);
    DIR *d = opendir("/proc/self/task");
    struct dirent *de;
    int count = 0, idle = 0;
    while (d && (de = readdir(d))) {
        int tid = atoi(de->d_name);
        if (tid <= 0 || tid == me) continue;
        char path[64], comm[64], stat[400], wch[96];
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid); read_small(path, comm, sizeof comm);
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid); read_small(path, stat, sizeof stat);
        snprintf(path, sizeof path, "/proc/self/task/%d/wchan", tid); read_small(path, wch, sizeof wch);
        char *st = strrchr(stat, ')'); char state = st && st[1] == ' ' ? st[2] : '?';
        /* the runtime's own helper threads are not interesting */
        if (strstr(comm, "HeapTask") || strstr(comm, "ReferenceQueue") || strstr(comm, "FinalizerDaemon") ||
            strstr(comm, "FinalizerWatchd") || strstr(comm, "Jit thread") || strstr(comm, "Signal Catcher") ||
            strstr(comm, "binder:") || strstr(comm, "Profile Saver") || strstr(comm, "perfetto") ||
            strstr(comm, "RenderThread") || strstr(comm, "hwuiTask"))
            continue;
        if (++count > 40) break;
        g_bt_done = 0; g_bt_n = 0;
        syscall(SYS_tgkill, pid, tid, sig);
        for (int i = 0; i < 40 && !g_bt_done; i++) { struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL); }
        char buf[2400]; int o = snprintf(buf, sizeof buf, "snap[%s]: tid %d '%s' %c wchan=%s :", tag, tid, comm, state, wch);
        if (!g_bt_done) o += snprintf(buf + o, sizeof buf - o, " (no stack)");
        for (int i = 0; g_bt_done && i < g_bt_n && i < 22 && o < (int)sizeof buf - 160; i++) {
            Dl_info di;
            if (dladdr(g_bt[i], &di) && di.dli_fname) {
                const char *base = strrchr(di.dli_fname, '/'); base = base ? base + 1 : di.dli_fname;
                o += snprintf(buf + o, sizeof buf - o, " %s+0x%lx", base, (unsigned long)((char *)g_bt[i] - (char *)di.dli_fbase));
                if (di.dli_sname) o += snprintf(buf + o, sizeof buf - o, "(%.110s)", di.dli_sname);
            } else o += snprintf(buf + o, sizeof buf - o, " %p", g_bt[i]);
        }
        /* idle helpers (waiting for work, sleeping, parked) only add noise */
        if (strstr(buf, "JobThread") || strstr(buf, "sysThreadPool") || strstr(buf, "Thread4Park") ||
            strstr(buf, "Monitor4Wait") || strstr(buf, "Looper") || strstr(buf, "AudioTrackThread") ||
            strstr(buf, "SaveDataManager") || strstr(buf, "pgStreamerDeviceHandler") || strstr(buf, "(no stack)"))
            { idle++; continue; }
        GLOG("%s", buf);
    }
    if (d) closedir(d);
    GLOG("snap[%s]: %d idle helper threads left out", tag, idle);
    GLOG("snap[%s]: end, %d threads", tag, count);
    if (!strcmp(tag, "25s")) ring_dump();
    (*env)->ReleaseStringUTFChars(env, jtag, tag);
}

/* ---------- Build 54: log the file calls that fail inside the game's own library ----------
 * The game's data-read thread crashes on a null pointer, which usually means a file or device it
 * asked for could not be resolved. Linux does not log failed opens, so patch the game library's
 * import table (GOT) for the common file calls and log the failures (and the pack files it does open).
 */
#include <link.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <android/log.h>

#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
static int g_light = 0;
/* custom resolution (LAS_RES_W/H): the game is told its window is this size, and touch positions are scaled to match */
static int g_resw = 0, g_resh = 0, g_realw = 0, g_realh = 0;
static int32_t h_anw_w(ANativeWindow *w) {
    int32_t r = ANativeWindow_getWidth(w);
    if (g_resw > 0 && r > 0) { if (r != g_realw) { g_realw = r; GLOG("resolution: window width %d reported to the game as %d", r, g_resw); } return g_resw; }
    return r;
}
static int32_t h_anw_h(ANativeWindow *w) {
    int32_t r = ANativeWindow_getHeight(w);
    if (g_resh > 0 && r > 0) { if (r != g_realh) { g_realh = r; GLOG("resolution: window height %d reported to the game as %d", r, g_resh); } return g_resh; }
    return r;
}
static int g_keep = 0;                            /* LAS_KEEP=1: the picture keeps its ratio (black bars), so touches need the bars' offset too */
static float touch_scale(void) {                  /* real pixels per game pixel */
    float sx = (float)g_realw / (float)g_resw, sy = (float)g_realh / (float)g_resh;
    return sx < sy ? sx : sy;
}
static float rx(float v) {
    static int logged = 0;
    if (g_resw <= 0 || g_realw <= 0) return v;
    if (g_keep && g_realh > 0) {
        float s = touch_scale(), off = ((float)g_realw - (float)g_resw * s) * 0.5f;
        if (!logged) { logged = 1; GLOG("resolution: touch x mapped for a kept ratio (scale %.3f, bars %.0f px)", s, off); }
        return (v - off) / s;
    }
    if (!logged) { logged = 1; GLOG("resolution: touch x scaled by %d/%d", g_resw, g_realw); }
    return v * g_resw / g_realw;
}
static float ry(float v) {
    if (g_resh <= 0 || g_realh <= 0) return v;
    if (g_keep && g_realw > 0) { float s = touch_scale(), off = ((float)g_realh - (float)g_resh * s) * 0.5f; return (v - off) / s; }
    return v * g_resh / g_realh;
}
/* ---------- Virtual gamepad: made-up input events for native games ----------
 * A NativeActivity game reads its input straight from the system's input queue (AInputQueue_getEvent), so events pushed through
 * Java never reach it. The on-screen controller pushes made-up key / joystick events here instead. They are handed out by the hooked
 * AInputQueue_getEvent, and the hooked getters (AInputEvent_getType, AKeyEvent_getKeyCode, AMotionEvent_getAxisValue, ...) answer
 * for them. A pipe is added to the game's looper so that a made-up event wakes the game's input handling like a real one would.
 */
#include <android/looper.h>
#include <unistd.h>
#include <fcntl.h>
#define PAD_N 128
typedef struct { int type, source, device, action, key; int64_t t, down; float ax[32]; } PadEv;
static PadEv g_pev[PAD_N];
static volatile unsigned g_phead = 0, g_ptail = 0;
static int g_ppipe[2] = {-1, -1};
static ALooper *g_ploop = NULL;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_pad_used = 0;                 /* the game asked for input through the queue we hook */
static volatile int g_pad_seen = 0;
static inline int pad_is(const void *e) { return (const PadEv *)e >= g_pev && (const PadEv *)e < g_pev + PAD_N; }
static int64_t pad_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec; }
static void pad_push(const PadEv *ev) {
    pthread_mutex_lock(&g_pmu);
    if (g_phead - g_ptail >= PAD_N - 2) g_ptail++;             /* full: drop the oldest */
    g_pev[g_phead % PAD_N] = *ev;
    g_phead++;
    pthread_mutex_unlock(&g_pmu);
    if (g_ppipe[1] >= 0) { char c = 1; (void)!write(g_ppipe[1], &c, 1); }
    if (g_ploop) ALooper_wake(g_ploop);
}
static float g_axes_last[32];
JNIEXPORT jint JNICALL Java_com_freecrane_adrenotest_Native_padKey(JNIEnv *env, jobject thiz, jint action, jint code, jint device) {
    (void)env; (void)thiz;
    PadEv ev; memset(&ev, 0, sizeof ev);
    ev.type = AINPUT_EVENT_TYPE_KEY; ev.source = 0x401 /* gamepad */ | 0x101 /* keyboard */; ev.device = device; ev.action = action; ev.key = code;
    ev.t = pad_now(); ev.down = ev.t;
    pad_push(&ev);
    return g_pad_seen;
}
JNIEXPORT jint JNICALL Java_com_freecrane_adrenotest_Native_padAxes(JNIEnv *env, jobject thiz, jint device, jfloat lx, jfloat ly, jfloat rx_, jfloat ry_,
                                                                    jfloat lt, jfloat rt, jfloat hx, jfloat hy) {
    (void)env; (void)thiz;
    PadEv ev; memset(&ev, 0, sizeof ev);
    ev.type = AINPUT_EVENT_TYPE_MOTION; ev.source = 0x01000010; ev.device = device; ev.action = AMOTION_EVENT_ACTION_MOVE;
    ev.ax[AMOTION_EVENT_AXIS_X] = lx; ev.ax[AMOTION_EVENT_AXIS_Y] = ly;
    ev.ax[AMOTION_EVENT_AXIS_Z] = rx_; ev.ax[AMOTION_EVENT_AXIS_RZ] = ry_;
    ev.ax[AMOTION_EVENT_AXIS_LTRIGGER] = lt; ev.ax[AMOTION_EVENT_AXIS_BRAKE] = lt;
    ev.ax[AMOTION_EVENT_AXIS_RTRIGGER] = rt; ev.ax[AMOTION_EVENT_AXIS_GAS] = rt;
    ev.ax[AMOTION_EVENT_AXIS_HAT_X] = hx; ev.ax[AMOTION_EVENT_AXIS_HAT_Y] = hy;
    ev.t = pad_now(); ev.down = ev.t;
    pad_push(&ev);
    return g_pad_seen;
}
JNIEXPORT jint JNICALL Java_com_freecrane_adrenotest_Native_padStatus(JNIEnv *env, jobject thiz) { (void)env; (void)thiz; return (g_pad_used ? 1 : 0) | (g_pad_seen ? 2 : 0); }

static int h_aiq_getEvent(AInputQueue *q, AInputEvent **out) {
    g_pad_used = 1;
    pthread_mutex_lock(&g_pmu);
    if (g_ptail != g_phead) {
        *out = (AInputEvent *)&g_pev[g_ptail % PAD_N];
        g_ptail++;
        pthread_mutex_unlock(&g_pmu);
        char c; (void)!read(g_ppipe[0], &c, 1);
        if (!g_pad_seen) { g_pad_seen = 1; GLOG("pad: the game took its first made-up controller event"); }
        return 0;
    }
    pthread_mutex_unlock(&g_pmu);
    return AInputQueue_getEvent(q, out);
}
static int32_t h_aiq_has(AInputQueue *q) { return (g_ptail != g_phead) ? 1 : AInputQueue_hasEvents(q); }
static int32_t h_aiq_pre(AInputQueue *q, AInputEvent *e) { return pad_is(e) ? 0 : AInputQueue_preDispatchEvent(q, e); }
static void h_aiq_fin(AInputQueue *q, AInputEvent *e, int h) { if (!pad_is(e)) AInputQueue_finishEvent(q, e, h); }
static void h_aiq_attach(AInputQueue *q, ALooper *l, int ident, ALooper_callbackFunc cb, void *data) {
    AInputQueue_attachLooper(q, l, ident, cb, data);
    if (g_ppipe[0] < 0 && pipe(g_ppipe) == 0) { fcntl(g_ppipe[0], F_SETFL, O_NONBLOCK); fcntl(g_ppipe[1], F_SETFL, O_NONBLOCK); }
    if (g_ppipe[0] >= 0) {
        ALooper_addFd(l, g_ppipe[0], ident, ALOOPER_EVENT_INPUT, cb, data);
        g_ploop = l;
        GLOG("pad: the game's input queue was found (looper id %d, %s)", ident, cb ? "callback" : "polled");
    }
}
static void h_aiq_detach(AInputQueue *q) {
    AInputQueue_detachLooper(q);
    if (g_ploop && g_ppipe[0] >= 0) { ALooper_removeFd(g_ploop, g_ppipe[0]); g_ploop = NULL; }
}
static int32_t h_aie_type(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->type : AInputEvent_getType(e); }
static int32_t h_aie_dev(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->device : AInputEvent_getDeviceId(e); }
static int32_t h_aie_src(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->source : AInputEvent_getSource(e); }
static int32_t h_ake_action(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->action : AKeyEvent_getAction(e); }
static int32_t h_ake_flags(const AInputEvent *e) { return pad_is(e) ? 0 : AKeyEvent_getFlags(e); }
static int32_t h_ake_key(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->key : AKeyEvent_getKeyCode(e); }
static int32_t h_ake_scan(const AInputEvent *e) { return pad_is(e) ? 0 : AKeyEvent_getScanCode(e); }
static int32_t h_ake_meta(const AInputEvent *e) { return pad_is(e) ? 0 : AKeyEvent_getMetaState(e); }
static int32_t h_ake_rep(const AInputEvent *e) { return pad_is(e) ? 0 : AKeyEvent_getRepeatCount(e); }
static int64_t h_ake_down(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->down : AKeyEvent_getDownTime(e); }
static int64_t h_ake_time(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->t : AKeyEvent_getEventTime(e); }
static int32_t h_ame_action(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->action : AMotionEvent_getAction(e); }
static int32_t h_ame_flags(const AInputEvent *e) { return pad_is(e) ? 0 : AMotionEvent_getFlags(e); }
static int32_t h_ame_meta(const AInputEvent *e) { return pad_is(e) ? 0 : AMotionEvent_getMetaState(e); }
static int32_t h_ame_btn(const AInputEvent *e) { return pad_is(e) ? 0 : AMotionEvent_getButtonState(e); }
static int32_t h_ame_edge(const AInputEvent *e) { return pad_is(e) ? 0 : AMotionEvent_getEdgeFlags(e); }
static int64_t h_ame_down(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->down : AMotionEvent_getDownTime(e); }
static int64_t h_ame_time(const AInputEvent *e) { return pad_is(e) ? ((const PadEv *)e)->t : AMotionEvent_getEventTime(e); }
static size_t h_ame_count(const AInputEvent *e) { return pad_is(e) ? 1 : AMotionEvent_getPointerCount(e); }
static int32_t h_ame_pid(const AInputEvent *e, size_t i) { return pad_is(e) ? 0 : AMotionEvent_getPointerId(e, i); }
static int32_t h_ame_tool(const AInputEvent *e, size_t i) { return pad_is(e) ? 0 : AMotionEvent_getToolType(e, i); }
static float h_ame_axis(const AInputEvent *e, int32_t axis, size_t i) { return pad_is(e) ? ((axis >= 0 && axis < 32 && i == 0) ? ((const PadEv *)e)->ax[axis] : 0.0f) : AMotionEvent_getAxisValue(e, axis, i); }
static float h_ame_press(const AInputEvent *e, size_t i) { return pad_is(e) ? 0.0f : AMotionEvent_getPressure(e, i); }
static float h_ame_size(const AInputEvent *e, size_t i) { return pad_is(e) ? 0.0f : AMotionEvent_getSize(e, i); }
static size_t h_ame_hsize(const AInputEvent *e) { return pad_is(e) ? 0 : AMotionEvent_getHistorySize(e); }
static float h_ame_xp(const AInputEvent *e) { return pad_is(e) ? 1.0f : AMotionEvent_getXPrecision(e); }
static float h_ame_yp(const AInputEvent *e) { return pad_is(e) ? 1.0f : AMotionEvent_getYPrecision(e); }

static float h_ame_x(const AInputEvent *e, size_t i) { return pad_is(e) ? ((const PadEv *)e)->ax[AMOTION_EVENT_AXIS_X] : rx(AMotionEvent_getX(e, i)); }
static float h_ame_y(const AInputEvent *e, size_t i) { return pad_is(e) ? ((const PadEv *)e)->ax[AMOTION_EVENT_AXIS_Y] : ry(AMotionEvent_getY(e, i)); }
static float h_ame_rx(const AInputEvent *e, size_t i) { return pad_is(e) ? 0.0f : rx(AMotionEvent_getRawX(e, i)); }
static float h_ame_ry(const AInputEvent *e, size_t i) { return pad_is(e) ? 0.0f : ry(AMotionEvent_getRawY(e, i)); }                           /* LAS_LIGHT=1: no diagnostics */
#define FH_SEEN 2048
static volatile uint32_t g_seen[FH_SEEN];
static volatile int g_fail_logged = 0, g_ok_logged = 0;

static uint32_t fh_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h ? h : 1; }
static int fh_first(const char *s) {              /* true the first time this string is seen */
    uint32_t h = fh_hash(s), i = h % FH_SEEN;
    for (int n = 0; n < FH_SEEN; n++, i = (i + 1) % FH_SEEN) {
        if (g_seen[i] == h) return 0;
        if (g_seen[i] == 0) { g_seen[i] = h; return 1; }
    }
    return 0;
}
static int fh_boring(const char *p) {
    static const char *skip[] = { "/proc/", "/sys/", "/dev/", "/system/", "/vendor/", "/apex/", "/product/", "/odm/", NULL };
    for (int i = 0; skip[i]; i++) if (!strncmp(p, skip[i], strlen(skip[i]))) return 1;
    return 0;
}
/* at most 3 failures logged per directory, so a probe over hundreds of files does not bury the log */
#define FH_DIRS 512
static volatile uint32_t g_dirh[FH_DIRS], g_dirn[FH_DIRS];
static int fh_dir_ok(const char *p) {
    const char *sl = strrchr(p, '/');
    uint32_t h = 2166136261u;
    for (const char *q = p; q < sl; q++) h = (h ^ (uint8_t)*q) * 16777619u;
    h |= 1;
    uint32_t i = h % FH_DIRS;
    for (int n = 0; n < FH_DIRS; n++, i = (i + 1) % FH_DIRS) {
        if (g_dirh[i] == h) return ++g_dirn[i] <= 3;
        if (g_dirh[i] == 0) { g_dirh[i] = h; g_dirn[i] = 1; return 1; }
    }
    return 0;
}
#define OPEN_RING 1024
static struct { char name[168]; long rv; const char *fn; } g_oring[OPEN_RING];
static volatile unsigned g_oring_n = 0;
static void oring_add(const char *fn, const char *name, long rv) {
    unsigned i = __sync_fetch_and_add(&g_oring_n, 1) % OPEN_RING;
    strncpy(g_oring[i].name, name, sizeof g_oring[i].name - 1);
    g_oring[i].name[sizeof g_oring[i].name - 1] = 0;
    g_oring[i].rv = rv; g_oring[i].fn = fn;
}
static void fh_note(const char *fn, const char *p, int failed, int e, long rv) {
    if (g_light) return;
    if (p && !failed && strchr(p, '/') && !fh_boring(p) && strcmp(fn, "access") && strcmp(fn, "stat") && strcmp(fn, "lstat") && strcmp(fn, "fstatat"))
        oring_add(fn, p, rv);
    if (!p || !strchr(p, '/')) return;   /* shader-name lookups and the like are not paths */
    char key[300]; snprintf(key, sizeof key, "%s|%s", fn, p);
    if (failed) {
        if (e == EEXIST || e == EINTR || e == EAGAIN || fh_boring(p)) return;
        if (g_fail_logged >= 300 || !fh_dir_ok(p) || !fh_first(key)) return;
        g_fail_logged++;
        GLOG("native-fail %s(%.230s) errno=%d", fn, p, e);
    } else if (strstr(p, "/packs/") || strstr(p, "/guests/")) {
        if (g_ok_logged >= 250 || !fh_first(key)) return;
        g_ok_logged++;
        GLOG("native-open %s(%.230s) -> 0x%lx", fn, p, rv);
    }
}


/* ---------- Build 58: the game's Android/data/<pkg> and Android/obb/<pkg> live in our own folder ----------
 * The game writes its Vulkan cache and logs under /sdcard/Android/data/<its package>, which this app is
 * not allowed to touch (errno 13). Paths under those folders are mapped into <guest dir>/ext/{data,obb}.
 */
static char g_pkg[160], g_ext[400];
static int fh_mkdirs(const char *path) {          /* create every directory above the last '/' */
    char tmp[1024]; size_t n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) if (tmp[i] == '/') { tmp[i] = 0; mkdir(tmp, 0771); tmp[i] = '/'; }
    return 0;
}
static char g_fake[400];                          /* LAS_FAKE_DIR: generated cpuinfo / gpu_model, when spoofing */
static volatile int g_hw_logged = 0;
static const char *fh_redirect(const char *p, char *buf, size_t n, int create) {
    static const char *roots[] = { "/sdcard", "/storage/emulated/0", "/storage/self/primary", "/mnt/sdcard", "/data/media/0", NULL };
    if (!p || p[0] != '/') return p;
    if (p[1] == 'p' || p[1] == 's') {            /* device identity files: log who reads them, serve the spoofed copies */
        int cpu = !strcmp(p, "/proc/cpuinfo");
        int gpu = !strncmp(p, "/sys/class/kgsl/", 16) && strstr(p, "gpu_model");
        if (cpu || gpu || !strncmp(p, "/sys/devices/soc", 16) || !strncmp(p, "/sys/class/kgsl/", 16) || !strncmp(p, "/sys/devices/system/cpu/", 24)) {
            if (g_hw_logged < 80 && fh_first(p)) { g_hw_logged++; GLOG("native-hw open(%.200s)%s", p, (cpu || gpu) && g_fake[0] ? " [spoofed]" : ""); }
            if (g_fake[0]) {
                const char *f = cpu ? "cpuinfo" : gpu ? "gpu_model" : NULL;
                if (f) {
                    snprintf(buf, n, "%s/%s", g_fake, f);
                    if (access(buf, R_OK) == 0) return buf;
                }
            }
        }
    }
    if (!g_ext[0]) return p;
    for (int i = 0; roots[i]; i++) {
        size_t l = strlen(roots[i]);
        if (strncmp(p, roots[i], l)) continue;
        const char *q = p + l; const char *kind = NULL;
        if (!strncmp(q, "/Android/data/", 14)) { kind = "data"; q += 14; }
        else if (!strncmp(q, "/Android/obb/", 13)) { kind = "obb"; q += 13; }
        else continue;
        size_t pl = strlen(g_pkg);
        if (strncmp(q, g_pkg, pl) || (q[pl] != '/' && q[pl] != 0)) continue;
        snprintf(buf, n, "%s/%s%s", g_ext, kind, q + pl);
        if (create) fh_mkdirs(buf);
        return buf;
    }
    return p;
}
#define RD(p, c) char rb_[1024]; p = fh_redirect(p, rb_, sizeof rb_, c)

static int h_open(const char *p, int fl, ...) {
    RD(p, (fl & O_CREAT) != 0);
    va_list ap; va_start(ap, fl); mode_t m = (fl & O_CREAT) ? (mode_t)va_arg(ap, int) : 0; va_end(ap);
    int r = open(p, fl, m); int e = errno; fh_note("open", p, r < 0, e, r); errno = e; return r;
}
static int h_open2(const char *p, int fl) { RD(p, 0); int r = open(p, fl, 0); int e = errno; fh_note("open", p, r < 0, e, r); errno = e; return r; }
static int h_openat(int d, const char *p, int fl, ...) {
    RD(p, (fl & O_CREAT) != 0);
    va_list ap; va_start(ap, fl); mode_t m = (fl & O_CREAT) ? (mode_t)va_arg(ap, int) : 0; va_end(ap);
    int r = openat(d, p, fl, m); int e = errno; if (p && p[0] == '/') fh_note("openat", p, r < 0, e, r); errno = e; return r;
}
static int h_openat2(int d, const char *p, int fl) { RD(p, 0); int r = openat(d, p, fl, 0); int e = errno; if (p && p[0] == '/') fh_note("openat", p, r < 0, e, r); errno = e; return r; }
static int h_stat(const char *p, struct stat *s) { RD(p, 0); int r = stat(p, s); int e = errno; fh_note("stat", p, r < 0, e, r); errno = e; return r; }
static int h_lstat(const char *p, struct stat *s) { RD(p, 0); int r = lstat(p, s); int e = errno; fh_note("lstat", p, r < 0, e, r); errno = e; return r; }
static int h_fstatat(int d, const char *p, struct stat *s, int f) { RD(p, 0); int r = fstatat(d, p, s, f); int e = errno; if (p && p[0] == '/') fh_note("fstatat", p, r < 0, e, r); errno = e; return r; }
static int h_access(const char *p, int m) { RD(p, 0); int r = access(p, m); int e = errno; fh_note("access", p, r < 0, e, r); errno = e; return r; }
static FILE *h_fopen(const char *p, const char *m) { RD(p, (m && (strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+')))); FILE *r = fopen(p, m); int e = errno; fh_note("fopen", p, r == NULL, e, (long)r); errno = e; return r; }
static DIR *h_opendir(const char *p) { RD(p, 0); DIR *r = opendir(p); int e = errno; fh_note("opendir", p, r == NULL, e, (long)r); errno = e; return r; }

#include <android/asset_manager.h>
static volatile int g_aam_logged = 0;
static AAsset *h_aam_open(AAssetManager *m, const char *fn, int mode) {
    AAsset *r = AAssetManager_open(m, fn, mode);
    if (g_light) return r;
    if (fn && r) oring_add("asset", fn, (long)r);
    if (fn && g_aam_logged < 400) { g_aam_logged++; GLOG("native-asset open(%.200s) -> %p", fn, (void *)r); }
    return r;
}

static int h_mkdir(const char *p, mode_t m) { RD(p, 1); return mkdir(p, m); }
static int h_unlink(const char *p) { RD(p, 0); return unlink(p); }
static int h_rmdir(const char *p) { RD(p, 0); return rmdir(p); }
static int h_remove(const char *p) { RD(p, 0); return remove(p); }
static int h_rename(const char *a, const char *b) { char ra[1024], rb2[1024]; a = fh_redirect(a, ra, sizeof ra, 0); b = fh_redirect(b, rb2, sizeof rb2, 1); return rename(a, b); }


/* ---------- Build 60: device identity the game reads besides Vulkan ----------
 * System properties (SoC / board), OpenGL ES strings, eglGetProcAddress. Values come from LAS_* env set by the app.
 */
#include <dlfcn.h>
extern int __system_property_get(const char *name, char *value);
static int h_spg(const char *name, char *value) {
    int r = __system_property_get(name, value);
    if (!name) return r;
    static const struct { const char *prop, *env; } map[] = {
        {"ro.hardware", "LAS_HARDWARE"}, {"ro.hardware.chipname", "LAS_SOC_MODEL"}, {"ro.board.platform", "LAS_BOARD"},
        {"ro.product.board", "LAS_BOARD"}, {"ro.soc.model", "LAS_SOC_MODEL"}, {"ro.soc.manufacturer", "LAS_SOC_MFR"},
        {"ro.vendor.qti.soc_model", "LAS_SOC_MODEL"}, {NULL, NULL}
    };
    const char *sp = NULL;
    for (int i = 0; map[i].prop; i++)
        if (!strcmp(name, map[i].prop)) { const char *v = getenv(map[i].env); if (v && *v) sp = v; break; }
    if (!sp && !strncmp(name, "ro.product.", 11)) {      /* ro.product.[vendor.|odm.|system.]model etc. */
        const char *last = strrchr(name, '.') + 1;
        const char *e = !strcmp(last, "model") ? "LAS_MODEL" : !strcmp(last, "manufacturer") ? "LAS_MANUFACTURER" :
                        !strcmp(last, "brand") ? "LAS_BRAND" : !strcmp(last, "device") ? "LAS_DEVICE" :
                        !strcmp(last, "name") ? "LAS_DEVICE" : NULL;
        const char *v = e ? getenv(e) : NULL;
        if (v && *v) sp = v;
    }
    static volatile int logged = 0;
    if (logged < 60 && fh_first(name) && (sp || !strncmp(name, "ro.hardware", 11) || !strncmp(name, "ro.board", 8) || !strncmp(name, "ro.soc", 6) || !strncmp(name, "ro.product", 10) || strstr(name, "chip") || strstr(name, "gpu") || strstr(name, "soc"))) {
        logged++;
        GLOG("native-prop %s = \"%.60s\"%s%.60s", name, r > 0 ? value : "", sp ? " -> " : "", sp ? sp : "");
    }
    if (sp) { strncpy(value, sp, 91); value[91] = 0; return (int)strlen(value); }
    return r;
}
typedef const unsigned char *(*glgs_t)(unsigned);
static const unsigned char *h_glGetString(unsigned name) {
    static glgs_t real;
    if (!real) real = (glgs_t)dlsym(RTLD_DEFAULT, "glGetString");
    const unsigned char *r = real ? real(name) : NULL;
    const char *sp = name == 0x1F01 ? getenv("LAS_GL_RENDERER") : name == 0x1F00 ? getenv("LAS_GL_VENDOR") : NULL;
    static volatile int logged = 0;
    if (name >= 0x1F00 && name <= 0x1F02 && logged < 12 && fh_first(name == 0x1F01 ? "gl-r" : name == 0x1F00 ? "gl-v" : "gl-ver")) {
        logged++;
        GLOG("native-gl glGetString(0x%x) = \"%.80s\"%s%.80s", name, r ? (const char *)r : "(null)", sp && *sp ? " -> " : "", sp && *sp ? sp : "");
    }
    return (sp && *sp) ? (const unsigned char *)sp : r;
}
typedef void *(*egpa_t)(const char *);
static void *h_eglGetProcAddress(const char *n) {
    static egpa_t real;
    if (!real) real = (egpa_t)dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    if (n && !strcmp(n, "glGetString")) return (void *)h_glGetString;
    return real ? real(n) : NULL;
}

static const struct { const char *name; void *fn; } fh_tab[] = {
    {"__system_property_get", h_spg}, {"glGetString", h_glGetString}, {"eglGetProcAddress", h_eglGetProcAddress},
    {"mkdir", h_mkdir}, {"unlink", h_unlink}, {"rmdir", h_rmdir}, {"remove", h_remove}, {"rename", h_rename},
    {"AAssetManager_open", h_aam_open},
    {"ANativeWindow_getWidth", h_anw_w}, {"ANativeWindow_getHeight", h_anw_h},
    {"AMotionEvent_getX", h_ame_x}, {"AMotionEvent_getY", h_ame_y}, {"AMotionEvent_getRawX", h_ame_rx}, {"AMotionEvent_getRawY", h_ame_ry},
    {"AInputQueue_getEvent", h_aiq_getEvent}, {"AInputQueue_hasEvents", h_aiq_has}, {"AInputQueue_preDispatchEvent", h_aiq_pre},
    {"AInputQueue_finishEvent", h_aiq_fin}, {"AInputQueue_attachLooper", h_aiq_attach}, {"AInputQueue_detachLooper", h_aiq_detach},
    {"AInputEvent_getType", h_aie_type}, {"AInputEvent_getDeviceId", h_aie_dev}, {"AInputEvent_getSource", h_aie_src},
    {"AKeyEvent_getAction", h_ake_action}, {"AKeyEvent_getFlags", h_ake_flags}, {"AKeyEvent_getKeyCode", h_ake_key}, {"AKeyEvent_getScanCode", h_ake_scan},
    {"AKeyEvent_getMetaState", h_ake_meta}, {"AKeyEvent_getRepeatCount", h_ake_rep}, {"AKeyEvent_getDownTime", h_ake_down}, {"AKeyEvent_getEventTime", h_ake_time},
    {"AMotionEvent_getAction", h_ame_action}, {"AMotionEvent_getFlags", h_ame_flags}, {"AMotionEvent_getMetaState", h_ame_meta},
    {"AMotionEvent_getButtonState", h_ame_btn}, {"AMotionEvent_getEdgeFlags", h_ame_edge}, {"AMotionEvent_getDownTime", h_ame_down},
    {"AMotionEvent_getEventTime", h_ame_time}, {"AMotionEvent_getPointerCount", h_ame_count}, {"AMotionEvent_getPointerId", h_ame_pid},
    {"AMotionEvent_getToolType", h_ame_tool}, {"AMotionEvent_getAxisValue", h_ame_axis}, {"AMotionEvent_getPressure", h_ame_press},
    {"AMotionEvent_getSize", h_ame_size}, {"AMotionEvent_getHistorySize", h_ame_hsize}, {"AMotionEvent_getXPrecision", h_ame_xp}, {"AMotionEvent_getYPrecision", h_ame_yp},
    {"open", h_open}, {"__open_2", h_open2}, {"openat", h_openat}, {"__openat_2", h_openat2},
    {"stat", h_stat}, {"lstat", h_lstat}, {"fstatat", h_fstatat}, {"access", h_access},
    {"fopen", h_fopen}, {"opendir", h_opendir}, {NULL, NULL}
};


/* ---------- Build 64: kgsl ioctls of the Turnip driver ----------
 * Turnip reports "device lost" when a kernel call on /dev/kgsl-3d0 fails or the GPU reset status is not clean.
 * Failed calls (other than the retried EINTR/EAGAIN and normal timeouts) and a non-zero reset status are logged.
 */
#include <sys/ioctl.h>
static int h_ioctl(int fd, int req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    int r = ioctl(fd, req, arg); int e = errno;
    unsigned long cmd = (unsigned long)(unsigned int)req;
    if (r < 0 && e != EINTR && e != EAGAIN && e != ETIMEDOUT) {
        char key[64]; snprintf(key, sizeof key, "ioctl|%lx|%d", cmd, e);
        static volatile int n = 0;
        if (n < 60 && fh_first(key)) { n++; GLOG("native-ioctl fd=%d cmd=0x%lx failed errno=%d", fd, cmd, e); }
    } else if (r == 0 && cmd == 0xC0180902UL && arg) {          /* KGSL_IOCTL_DEVICE_GETPROPERTY */
        struct { unsigned int type; unsigned int pad; void *value; size_t size; } *gp = arg;
        if (gp->type == 9 && gp->size >= 4 && gp->value) {       /* KGSL_PROP_GPU_RESET_STAT */
            unsigned v = *(volatile unsigned *)gp->value;
            char key[48]; snprintf(key, sizeof key, "rst|%u", v);
            static volatile int m = 0;
            if (m < 20 && fh_first(key)) { m++; GLOG("native-ioctl kgsl GPU reset status = %u (0 = none, 1 = guilty, 2 = innocent, 3 = unknown)", v); }
        }
    }
    errno = e;
    return r;
}
static const struct { const char *name; void *fn; } fh_tab_drv[] = { {"ioctl", h_ioctl}, {NULL, NULL} };

static uintptr_t g_rdr_base;
static int fh_patch_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void)sz;
    if (!info->dlpi_name) return 0;
    int is_rdr = strstr(info->dlpi_name, "librdr.so") != NULL;
    int is_drv = strstr(info->dlpi_name, "/files/driver/") != NULL || strstr(info->dlpi_name, "/files/drivers/") != NULL;      /* the custom Vulkan driver */
    if (is_drv && g_light) return 0;                 /* light mode: no ioctl watching */
    if (!is_rdr && !is_drv && (!strstr(info->dlpi_name, "/guests/") || strstr(info->dlpi_name, "vkshim"))) return 0;
    const struct { const char *name; void *fn; } *tab = is_drv ? fh_tab_drv : fh_tab;
    uintptr_t base = info->dlpi_addr;
    if (is_rdr) g_rdr_base = base;
    const ElfW(Dyn) *dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(base + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) return 0;
    uintptr_t symtab = 0, strtab = 0, jmprel = 0, rela = 0; size_t pltsz = 0, relasz = 0;
#define FIX(v) ((v) >= base ? (uintptr_t)(v) : base + (uintptr_t)(v))
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_SYMTAB: symtab = FIX(dyn->d_un.d_ptr); break;
            case DT_STRTAB: strtab = FIX(dyn->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = FIX(dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltsz = dyn->d_un.d_val; break;
            case DT_RELA: rela = FIX(dyn->d_un.d_ptr); break;
            case DT_RELASZ: relasz = dyn->d_un.d_val; break;
        }
    }
    int *count = (int *)data;
    for (int pass = 0; pass < 2; pass++) {
        const ElfW(Rela) *r = (const ElfW(Rela) *)(pass ? rela : jmprel);
        size_t n = (pass ? relasz : pltsz) / sizeof(ElfW(Rela));
        if (!r || !symtab || !strtab) continue;
        for (size_t i = 0; i < n; i++) {
            uint32_t type = ELF64_R_TYPE(r[i].r_info), sym = ELF64_R_SYM(r[i].r_info);
            if (!sym || (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT)) continue;
            const char *nm = (const char *)strtab + ((const ElfW(Sym) *)symtab)[sym].st_name;
            for (int k = 0; tab[k].name; k++) {
                if (strcmp(nm, tab[k].name)) continue;
                void **slot = (void **)(base + r[i].r_offset);
                uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)4095;
                if (*slot == tab[k].fn) continue;
                if (mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE) == 0) { *slot = tab[k].fn; (*count)++; if (is_drv) GLOG("file hooks: ioctl hooked in the driver (%s)", info->dlpi_name); }
            }
        }
    }
    return 0;
}

static void *fh_thread(void *arg) {
    (void)arg;
    int found = 0;
    for (int i = 0; i < 3000; i++) {            /* ~3 min: the game's libraries load at different times */
        int count = 0;
        dl_iterate_phdr(fh_patch_cb, &count);
        if (count) GLOG("file hooks: patched %d imports in the game's libraries", count);
        if (g_rdr_base) found = 1;
        usleep(found && i > 1500 ? 100000 : 20000);
    }
    if (!found) GLOG("file hooks: librdr.so never appeared");
    return NULL;
}


/* ---------- Build 63: the game's and the driver's own stderr/stdout go to the log ----------
 * Mesa/Turnip prints why a device was lost (and the game prints its own messages) to stderr, which Android
 * normally throws away. Both descriptors are pointed at a non-blocking pipe (a full pipe never stalls the
 * game) and a thread forwards the lines.
 */
static int g_errpipe[2] = {-1, -1};
static void *errpipe_thread(void *arg) {
    (void)arg;
    char buf[4096], line[600]; size_t ll = 0; int logged = 0;
    for (;;) {
        ssize_t n = read(g_errpipe[0], buf, sizeof buf);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) { usleep(20000); continue; }
        if (n <= 0) return NULL;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || ll >= sizeof line - 1) {
                line[ll] = 0; ll = 0;
                if (line[0] && logged < 400 && fh_first(line)) { logged++; GLOG("native-stderr %.500s", line); }
            } else line[ll++] = c;
        }
    }
}
static void errpipe_install(void) {
    static int done = 0;
    if (done) return; done = 1;
    if (pipe(g_errpipe) != 0) return;
    fcntl(g_errpipe[1], F_SETFL, O_NONBLOCK);
    dup2(g_errpipe[1], 1); dup2(g_errpipe[1], 2);
    pthread_t t;
    if (pthread_create(&t, NULL, errpipe_thread, NULL) == 0) pthread_detach(t);
}

JNIEXPORT void JNICALL
Java_com_freecrane_adrenotest_Native_installFileHooks(JNIEnv *env, jobject thiz, jstring jpkg, jstring jext) {
    (void)thiz;
    const char *pk = jpkg ? (*env)->GetStringUTFChars(env, jpkg, NULL) : NULL;
    const char *ex = jext ? (*env)->GetStringUTFChars(env, jext, NULL) : NULL;
    if (pk) { strncpy(g_pkg, pk, sizeof g_pkg - 1); (*env)->ReleaseStringUTFChars(env, jpkg, pk); }
    if (ex) { strncpy(g_ext, ex, sizeof g_ext - 1); (*env)->ReleaseStringUTFChars(env, jext, ex); }
    if (g_ext[0]) { char t[500]; snprintf(t, sizeof t, "%s/data/x", g_ext); fh_mkdirs(t); snprintf(t, sizeof t, "%s/obb/x", g_ext); fh_mkdirs(t); }
    { const char *fk = getenv("LAS_FAKE_DIR"); if (fk) strncpy(g_fake, fk, sizeof g_fake - 1); }
    GLOG("file hooks: Android/data and Android/obb of %s -> %s; fake dir %s", g_pkg, g_ext, g_fake);
    { const char *lt = getenv("LAS_LIGHT"); g_light = (lt && *lt == '1'); }
    { const char *rw = getenv("LAS_RES_W"), *rh = getenv("LAS_RES_H"); g_resw = rw ? atoi(rw) : 0; g_resh = rh ? atoi(rh) : 0; { const char *kp = getenv("LAS_KEEP"); g_keep = (kp && *kp == '1'); } if (g_resw <= 0 || g_resh <= 0) g_resw = g_resh = 0; }
    if (!g_light) errpipe_install();
    GLOG("file hooks: light mode %s", g_light ? "ON (diagnostics off)" : "off");
    pthread_t t;
    if (pthread_create(&t, NULL, fh_thread, NULL) == 0) pthread_detach(t);
}


/* ---------- Build 55: when the game library itself faults, log registers and the code around pc ----------
 * Disassembling these bytes offline shows which pointer was null and what the code was doing.
 * Faults outside the game library (ART's own null checks) are passed straight on.
 */
#include <signal.h>
#include <ucontext.h>
static struct sigaction g_old_segv, g_old_bus;
static volatile int g_crash_done = 0;

static int g_pipe[2] = {-1, -1};
/* copy memory that may be invalid without faulting: push it through a pipe, the kernel reports EFAULT */
static int safe_read(void *dst, const void *src, size_t n) {
    if (g_pipe[0] < 0 || n > 4096) return 0;
    if (write(g_pipe[1], src, n) != (ssize_t)n) return 0;
    return read(g_pipe[0], dst, n) == (ssize_t)n;
}
static void dump_mem(const char *label, uint64_t addr, size_t n) {
    uint8_t buf[128];
    if (n > sizeof buf || addr < 0x10000 || !safe_read(buf, (const void *)addr, n)) { GLOG("crash-mem %s @%llx unreadable", label, (unsigned long long)addr); return; }
    for (size_t o = 0; o < n; o += 32) {
        char line[100]; int k = 0;
        for (size_t j = o; j < o + 32 && j < n; j++) k += snprintf(line + k, sizeof line - k, "%02x", buf[j]);
        GLOG("crash-mem %s +0x%zx %s", label, o, line);
    }
}
static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    struct sigaction *old = (sig == SIGSEGV) ? &g_old_segv : &g_old_bus;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t base = g_rdr_base;

    /* Build 57: pgReadData::Worker calls device->Read(handle, offset, buf, size) for a queued request
     * whose device pointer is NULL (a read of vgd_android.rpf). The handle holds the file's own
     * descriptor at +0x18, so do the read the device would have done (pread) and resume the worker
     * after the call, with the byte count as the result. Only this exact fault is handled. */
    #define RDR_WORKER_FAULT  0x1a3ed84UL
    #define RDR_WORKER_RESUME 0x1a3ed98UL
    if (sig == SIGSEGV && base && pc == base + RDR_WORKER_FAULT && uc->uc_mcontext.regs[0] == 0 && si->si_addr == NULL) {
        uint64_t req = uc->uc_mcontext.regs[19], handle = uc->uc_mcontext.regs[1], off = uc->uc_mcontext.regs[2];
        int fd = -1; uint64_t dst = 0; uint32_t sz = 0;
        if (safe_read(&fd, (const void *)(handle + 0x18), 4) && safe_read(&dst, (const void *)(req + 0x18), 8) &&
            safe_read(&sz, (const void *)(req + 0x20), 4) && fd >= 0 && fd < 4096 && dst && sz <= (64u << 20)) {
            ssize_t n = pread(fd, (void *)dst, sz, (off_t)off);
            static volatile int cnt = 0;
            int c = ++cnt;
            if (c <= 20 || c % 200 == 0) {
                const char *nm = "?";
                unsigned tot = g_oring_n, m = tot < OPEN_RING ? tot : OPEN_RING;
                for (unsigned k = 0; k < m; k++) if (g_oring[k].rv == fd && !strcmp(g_oring[k].fn, "open")) nm = g_oring[k].name;
                GLOG("read-fix #%d: device was NULL, did pread(fd=%d [%.150s], off=0x%llx, size=%u) -> %ld", c, fd, nm,
                     (unsigned long long)off, sz, (long)n);
            }
            uc->uc_mcontext.regs[0] = (uint64_t)(int64_t)n;
            uc->uc_mcontext.pc = base + RDR_WORKER_RESUME;
            return;
        }
    }
    if (base && pc >= base && pc < base + 0x6000000 && !g_crash_done) {
        g_crash_done = 1;
        GLOG("crash-info sig=%d code=%d addr=%p tid=%d pc=librdr+0x%lx lr=librdr+0x%lx sp=%p base=%p",
             sig, si->si_code, si->si_addr, (int)gettid(), (unsigned long)(pc - base),
             (unsigned long)((uintptr_t)uc->uc_mcontext.regs[30] - base), (void *)uc->uc_mcontext.sp, (void *)base);
        for (int i = 0; i < 29; i += 4) {
            GLOG("crash-regs x%d=%llx x%d=%llx x%d=%llx x%d=%llx", i, (unsigned long long)uc->uc_mcontext.regs[i],
                 i + 1, (unsigned long long)uc->uc_mcontext.regs[i + 1],
                 i + 2, (unsigned long long)(i + 2 < 29 ? uc->uc_mcontext.regs[i + 2] : 0),
                 i + 3, (unsigned long long)(i + 3 < 29 ? uc->uc_mcontext.regs[i + 3] : 0));
        }
        {
            uint64_t want[3] = { uc->uc_mcontext.regs[1], 0, 0 };
            safe_read(&want[1], (const void *)(uc->uc_mcontext.regs[19] + 8), 8);
            safe_read(&want[2], (const void *)(uc->uc_mcontext.regs[19] + 0), 8);
            unsigned total = g_oring_n, cnt = total < OPEN_RING ? total : OPEN_RING, hits = 0;
            for (unsigned k = 0; k < cnt; k++) {
                long rv = g_oring[k].rv;
                for (int w = 0; w < 3; w++)
                    if (want[w] && (uint64_t)rv == want[w]) { GLOG("crash-match %s(%.160s) -> 0x%lx (field %d)", g_oring[k].fn, g_oring[k].name, rv, w); hits++; }
            }
            GLOG("crash-match: %u of %u recorded opens matched the request", hits, cnt);
            for (unsigned k = (cnt > 10 ? cnt - 10 : 0); k < cnt; k++) {
                unsigned idx = (total < OPEN_RING) ? k : (total - cnt + k) % OPEN_RING;
                GLOG("crash-recent %s(%.160s) -> 0x%lx", g_oring[idx].fn, g_oring[idx].name, g_oring[idx].rv);
            }
        }
        dump_mem("x19", uc->uc_mcontext.regs[19], 96);
        dump_mem("x1", uc->uc_mcontext.regs[1], 64);
        for (long off = -64; off < 32; off += 32) {
            const uint8_t *q = (const uint8_t *)(pc + off);
            char line[128]; int n = 0;
            for (int k = 0; k < 32; k++) n += snprintf(line + n, sizeof line - n, "%02x", q[k]);
            GLOG("crash-code +0x%lx %s", (unsigned long)(pc + off - base), line);
        }
    }
    /* hand the signal on: put the old handler back and re-run the fault, or call it directly */
    if (base && pc >= base && pc < base + 0x6000000) {
        sigaction(sig, old, NULL);
        return;
    }
    if (old->sa_flags & SA_SIGINFO) old->sa_sigaction(sig, si, ctx);
    else if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) { sigaction(sig, old, NULL); }
    else old->sa_handler(sig);
}

__attribute__((constructor)) static void crash_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (pipe(g_pipe) != 0) { g_pipe[0] = g_pipe[1] = -1; }
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}
