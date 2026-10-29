// C interface to the frame generation core, used by the Vulkan shim (sgsr.c). Runs on the game's own device and queue.
#include <android/log.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include "extract.hpp"
#include "trans.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GuestHost", __VA_ARGS__)
#define API extern "C" __attribute__((visibility("default")))

namespace {
bool g_perf = true;
bool g_ready = false;
}

// Returns 0 on success. flow: 0.25 .. 1.0 (lower = faster, less exact); perf: use the lighter "performance" shaders.
API int las_lsfg_init(void *gipa, void *inst, void *phys, void *dev, uint32_t family, void *queue,
                      const char *dll, float flow, int perf, int hdr) {
    if (g_ready) return 0;
    try {
        Extract::setDllPath(dll ? dll : "");
        Extract::extractShaders();
        auto loader = [](const std::string &name) {
            auto dx = Extract::getShader(name);
            return Extract::translateShader(dx);
        };
        if (flow < 0.25f) flow = 0.25f;
        if (flow > 1.0f) flow = 1.0f;
        g_perf = perf != 0;
        if (g_perf)
            LSFG_3_1P::initializeExternal(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa), static_cast<VkInstance>(inst),
                static_cast<VkPhysicalDevice>(phys), static_cast<VkDevice>(dev), family, static_cast<VkQueue>(queue),
                hdr != 0, 1.0f / flow, 1, loader);
        else
            LSFG_3_1::initializeExternal(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa), static_cast<VkInstance>(inst),
                static_cast<VkPhysicalDevice>(phys), static_cast<VkDevice>(dev), family, static_cast<VkQueue>(queue),
                hdr != 0, 1.0f / flow, 1, loader);
        g_ready = true;
        LOGI("lsfg: core ready (%s shaders, flow scale %.2f)", g_perf ? "performance" : "quality", flow);
        return 0;
    } catch (const std::exception &e) {
        LOGI("lsfg: init failed: %s", e.what());
    } catch (...) {
        LOGI("lsfg: init failed (unknown error)");
    }
    return -1;
}

// in0/in1: the two input images, out0: the generated frame. All are VkImage handles in GENERAL layout.
API int las_lsfg_ctx(uint64_t in0, uint64_t in1, uint64_t out0, uint32_t w, uint32_t h, int format) {
    if (!g_ready) return -1;
    try {
        std::vector<VkImage> outs{ reinterpret_cast<VkImage>(out0) };
        VkExtent2D ext{ w, h };
        int32_t id = g_perf
            ? LSFG_3_1P::createContextFromImages(reinterpret_cast<VkImage>(in0), reinterpret_cast<VkImage>(in1), outs, ext, static_cast<VkFormat>(format))
            : LSFG_3_1::createContextFromImages(reinterpret_cast<VkImage>(in0), reinterpret_cast<VkImage>(in1), outs, ext, static_cast<VkFormat>(format));
        LOGI("lsfg: context %d created (%ux%u)", id, w, h);
        return id;
    } catch (const std::exception &e) {
        LOGI("lsfg: context failed: %s", e.what());
    } catch (...) {
        LOGI("lsfg: context failed (unknown error)");
    }
    return -1;
}

// Queues the generation work on the game's queue (after the new input frame was copied in). Returns 0 on success.
API int las_lsfg_present(int id) {
    if (!g_ready) return -1;
    try {
        std::vector<int> none;
        if (g_perf) LSFG_3_1P::presentContext(id, -1, none);
        else LSFG_3_1::presentContext(id, -1, none);
        return 0;
    } catch (const std::exception &e) {
        LOGI("lsfg: present failed: %s", e.what());
    } catch (...) {
        LOGI("lsfg: present failed (unknown error)");
    }
    return -1;
}

API void las_lsfg_delete(int id) {
    if (!g_ready) return;
    try {
        if (g_perf) LSFG_3_1P::deleteContext(id);
        else LSFG_3_1::deleteContext(id);
    } catch (...) {
    }
}
