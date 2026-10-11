#include <jni.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

JNIEXPORT jstring JNICALL
Java_com_freecrane_adrenotest_Guest_vulkanInfo(JNIEnv *env, jobject thiz) {
    char out[512];

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        snprintf(out, sizeof out, "vkCreateInstance failed: %d", (int)r);
        return (*env)->NewStringUTF(env, out);
    }

    uint32_t count = 1;
    VkPhysicalDevice dev;
    vkEnumeratePhysicalDevices(inst, &count, &dev);
    if (count == 0) {
        snprintf(out, sizeof out, "no physical devices");
        vkDestroyInstance(inst, NULL);
        return (*env)->NewStringUTF(env, out);
    }

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(dev, &p);
    snprintf(out, sizeof out, "%s\nVulkan %u.%u.%u\ndriverVersion 0x%x",
             p.deviceName,
             VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion),
             VK_API_VERSION_PATCH(p.apiVersion),
             p.driverVersion);
    vkDestroyInstance(inst, NULL);
    return (*env)->NewStringUTF(env, out);
}
