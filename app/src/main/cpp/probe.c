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
static jmethodID h_getmid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jmethodID r = o_getmid(e, c, n, sg); mid_note(r, n, sg); rec("GetMethodID %s %s -> %p", n, sg, r); return r; }
static jmethodID h_getsmid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jmethodID r = o_getsmid(e, c, n, sg); mid_note(r, n, sg); rec("GetStaticMethodID %s %s -> %p", n, sg, r); return r; }
static jfieldID h_getfid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jfieldID r = o_getfid(e, c, n, sg); rec("GetFieldID %s %s -> %p", n, sg, r); return r; }
static jfieldID h_getsfid(JNIEnv *e, jclass c, const char *n, const char *sg) {
    jfieldID r = o_getsfid(e, c, n, sg); rec("GetStaticFieldID %s %s -> %p", n, sg, r); return r; }
static jobject h_getsobj(JNIEnv *e, jclass c, jfieldID f) {
    jobject r = o_getsobj(e, c, f); rec("GetStaticObjectField %p -> %s", f, r ? "obj" : "NULL"); return r; }
static jobject h_getobj(JNIEnv *e, jobject o, jfieldID f) {
    jobject r = o_getobj(e, o, f); rec("GetObjectField %p -> %s", f, r ? "obj" : "NULL"); return r; }
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

static jclass g_wrap_cls = NULL;
static jmethodID g_wrap_mid = NULL;

static __thread int t_in_wrap = 0;
static jobject after_obj(JNIEnv *e, const char *kind, jmethodID m, jobject r) {
    rec("%s %s -> %s", kind, mid_name(m), r ? "obj" : "NULL");
    /* The game's native code gets its asset-pack manager from AssetPackManagerFactory.getInstance(Context).
     * Hand it a manager that answers for packs that are really on the phone (see GuestAssetPacks). */
    if (r && g_wrap_cls && g_wrap_mid && !t_in_wrap &&
        !strncmp(mid_name(m), "getInstance(Landroid/content/Context;)Lcom/google/android/play/core/assetpacks/AssetPackManager;", 96)) {
        t_in_wrap = 1;
        jobject w = (*e)->CallStaticObjectMethod(e, g_wrap_cls, g_wrap_mid, r);
        t_in_wrap = 0;
        if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); rec("   wrap threw"); }
        else if (w) { rec("   AssetPackManager wrapped"); GLOG("asset packs: native getInstance -> wrapped manager"); return w; }
    }
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
    jobject r = o_cov(e, o, m, ap); return after_obj(e, "CallObjectMethodV", m, r); }
static jobject h_csom(JNIEnv *e, jclass c, jmethodID m, ...) {
    va_list ap; va_start(ap, m);
    jobject r = o_csov ? o_csov(e, c, m, ap) : (*e)->CallStaticObjectMethodV(e, c, m, ap);
    va_end(ap); return after_obj(e, "CallStaticObjectMethod", m, r);
}
static jobject h_com(JNIEnv *e, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m);
    jobject r = o_cov ? o_cov(e, o, m, ap) : (*e)->CallObjectMethodV(e, o, m, ap);
    va_end(ap); return after_obj(e, "CallObjectMethod", m, r);
}
typedef jobject (*CallSOA)(JNIEnv *, jclass, jmethodID, const jvalue *);
typedef jobject (*CallOA)(JNIEnv *, jobject, jmethodID, const jvalue *);
static CallSOA o_csoa; static CallOA o_coa;
static jobject h_csoa(JNIEnv *e, jclass c, jmethodID m, const jvalue *a) {
    jobject r = o_csoa(e, c, m, a); return after_obj(e, "CallStaticObjectMethodA", m, r); }
static jobject h_coa(JNIEnv *e, jobject o, jmethodID m, const jvalue *a) {
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
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
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
