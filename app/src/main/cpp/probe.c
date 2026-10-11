#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <adrenotools/driver.h>

static void run(char *out, size_t n, const char *tmp, const char *hook,
                const char *dir, const char *name, int custom) {
    void *vk;
    if (custom) {
        vk = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                        tmp, hook, dir, name, NULL, NULL);
    } else {
        vk = dlopen("libvulkan.so", RTLD_NOW);
    }
    if (!vk) {
        snprintf(out, n, "open failed: %s", dlerror());
        return;
    }

    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) {
        snprintf(out, n, "no vkGetInstanceProcAddr");
        return;
    }

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
    if (r != VK_SUCCESS) {
        snprintf(out, n, "vkCreateInstance failed: %d", (int)r);
        return;
    }

    PFN_vkEnumeratePhysicalDevices enumd =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");

    uint32_t count = 1;
    VkPhysicalDevice dev;
    enumd(inst, &count, &dev);
    if (count == 0) {
        snprintf(out, n, "no physical devices");
        return;
    }

    VkPhysicalDeviceProperties p;
    props(dev, &p);
    snprintf(out, n, "%s\nVulkan %u.%u.%u\ndriverVersion 0x%x",
             p.deviceName,
             VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion),
             VK_API_VERSION_PATCH(p.apiVersion),
             p.driverVersion);
}

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

    run(out, sizeof out, tmp, hook, dir, name, custom);

    (*env)->ReleaseStringUTFChars(env, jtmp, tmp);
    (*env)->ReleaseStringUTFChars(env, jhook, hook);
    (*env)->ReleaseStringUTFChars(env, jdir, dir);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (*env)->NewStringUTF(env, out);
}
