/* Snapdragon Game Super Resolution 1 (spatial upscaling) for the guest game, inside the Vulkan shim.
 *
 * The game believes its swapchain has the (small) size reported through the custom resolution setting. For such a
 * swapchain this file creates a REAL swapchain at the full surface size instead, and hands the game private
 * "proxy" images of the small size from vkGetSwapchainImagesKHR. The game renders into the proxy images as usual.
 * At vkQueuePresentKHR an extra submit upscales proxy image N into real image N with the SGSR shader, and the
 * present waits for it. Acquire is untouched: image N of the real swapchain is image N of the proxy set.
 *
 * Everything is built when the swapchain is created; if anything fails, the real swapchain is thrown away and the
 * game gets an ordinary swapchain (the Android compositor then scales it, as with the plain resolution setting).
 *
 * Shader: sgsr.frag, from github.com/SnapdragonGameStudios/snapdragon-gsr (BSD-3-Clause, Qualcomm Innovation Center).
 * Needs LAS_SGSR=1. It applies to any swapchain the game makes smaller than the screen, whether that size comes from the
 * custom resolution setting or from the game's own resolution option. */
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "sgsr_spv.h"
#include "fxaa_spv.h"
#include "fsr_spv.h"
#include "scale_spv.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GuestHost", __VA_ARGS__)
extern void *vkshim_real_proc(const char *name);
extern int vkshim_q_family(VkQueue q, uint32_t *family);
extern VkPhysicalDevice vkshim_phys_of(VkDevice d);
extern uint32_t vkshim_first_queue_family(int *ok);
typedef long (*sg_f8)(long, long, long, long, long, long, long, long);
typedef long (*sg_f2)(long, long);
typedef long (*sg_f4)(long, long, long, long);
typedef void (*sg_f4v)(long, long, long, long);

#define MAXIMG 16
#define MAXI (MAXIMG + 1)   /* per-image arrays: one more slot for the generated frame's intermediates */
#define MAXSC 4

static struct {
    PFN_vkCreateImage CreateImage; PFN_vkDestroyImage DestroyImage; PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkFreeMemory FreeMemory; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView; PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateSampler CreateSampler; PFN_vkDestroySampler DestroySampler;
    PFN_vkCreateRenderPass CreateRenderPass; PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer; PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateShaderModule CreateShaderModule; PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout; PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout; PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines; PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateDescriptorPool CreateDescriptorPool; PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets; PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateCommandPool CreateCommandPool; PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer; PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass; PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline; PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants; PFN_vkCmdDraw CmdDraw;
    PFN_vkCreateSemaphore CreateSemaphore; PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkQueueSubmit QueueSubmit; PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR; PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetCaps; PFN_vkGetPhysicalDeviceMemoryProperties GetMemProps;
    PFN_vkCmdBlitImage CmdBlitImage; PFN_vkQueueWaitIdle QueueWaitIdle; PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
} F;
static int g_fn_state = 0;                       /* 0 = not tried, 1 = ready, -1 = a function is missing */

static int fn_init(void) {
    if (g_fn_state) return g_fn_state > 0;
    const char *missing = NULL;
#define LD(field, name) do { *(void **)&F.field = vkshim_real_proc("vk" name); if (!F.field && !missing) missing = "vk" name; } while (0)
    LD(CreateImage, "CreateImage"); LD(DestroyImage, "DestroyImage"); LD(GetImageMemoryRequirements, "GetImageMemoryRequirements");
    LD(AllocateMemory, "AllocateMemory"); LD(FreeMemory, "FreeMemory"); LD(BindImageMemory, "BindImageMemory");
    LD(CreateImageView, "CreateImageView"); LD(DestroyImageView, "DestroyImageView");
    LD(CreateSampler, "CreateSampler"); LD(DestroySampler, "DestroySampler");
    LD(CreateRenderPass, "CreateRenderPass"); LD(DestroyRenderPass, "DestroyRenderPass");
    LD(CreateFramebuffer, "CreateFramebuffer"); LD(DestroyFramebuffer, "DestroyFramebuffer");
    LD(CreateShaderModule, "CreateShaderModule"); LD(DestroyShaderModule, "DestroyShaderModule");
    LD(CreateDescriptorSetLayout, "CreateDescriptorSetLayout"); LD(DestroyDescriptorSetLayout, "DestroyDescriptorSetLayout");
    LD(CreatePipelineLayout, "CreatePipelineLayout"); LD(DestroyPipelineLayout, "DestroyPipelineLayout");
    LD(CreateGraphicsPipelines, "CreateGraphicsPipelines"); LD(DestroyPipeline, "DestroyPipeline");
    LD(CreateDescriptorPool, "CreateDescriptorPool"); LD(DestroyDescriptorPool, "DestroyDescriptorPool");
    LD(AllocateDescriptorSets, "AllocateDescriptorSets"); LD(UpdateDescriptorSets, "UpdateDescriptorSets");
    LD(CreateCommandPool, "CreateCommandPool"); LD(DestroyCommandPool, "DestroyCommandPool");
    LD(AllocateCommandBuffers, "AllocateCommandBuffers");
    LD(BeginCommandBuffer, "BeginCommandBuffer"); LD(EndCommandBuffer, "EndCommandBuffer");
    LD(CmdPipelineBarrier, "CmdPipelineBarrier");
    LD(CmdBeginRenderPass, "CmdBeginRenderPass"); LD(CmdEndRenderPass, "CmdEndRenderPass");
    LD(CmdBindPipeline, "CmdBindPipeline"); LD(CmdBindDescriptorSets, "CmdBindDescriptorSets");
    LD(CmdPushConstants, "CmdPushConstants"); LD(CmdDraw, "CmdDraw");
    LD(CreateSemaphore, "CreateSemaphore"); LD(DestroySemaphore, "DestroySemaphore");
    LD(QueueSubmit, "QueueSubmit"); LD(GetSwapchainImagesKHR, "GetSwapchainImagesKHR");
    LD(DestroySwapchainKHR, "DestroySwapchainKHR"); LD(DeviceWaitIdle, "DeviceWaitIdle");
    LD(CmdBlitImage, "CmdBlitImage"); LD(QueueWaitIdle, "QueueWaitIdle"); LD(AcquireNextImageKHR, "AcquireNextImageKHR");
    LD(GetCaps, "GetPhysicalDeviceSurfaceCapabilitiesKHR"); LD(GetMemProps, "GetPhysicalDeviceMemoryProperties");
#undef LD
    if (missing) LOGI("sgsr: off, the driver has no %s", missing);
    g_fn_state = missing ? -1 : 1;
    return g_fn_state > 0;
}

/* ---------- frame generation core (liblsfgcore.so, loaded when LAS_LSFG=1) ---------- */
static struct {
    int (*init)(void *, void *, void *, void *, uint32_t, void *, int, float, int, int, const char *, char *, int);
    int (*ctx)(uint64_t, uint64_t, const uint64_t *, int, uint32_t, uint32_t, int, char *, int);
    int (*present)(int, char *, int);
    void (*del)(int);
    int state, ready, failed;
} LG;
static VkInstance g_inst;
static int g_lg_dev_ok = 1;

int vkshim_lsfg_wanted(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("LAS_LSFG"), *d = getenv("LAS_LSFG_DLL"); s = (e && *e == '1' && d && *d) ? 1 : 0; }
    return s && g_lg_dev_ok;
}

void vkshim_lsfg_instance(long pinst, long res) { if (res == 0 && pinst) g_inst = *(VkInstance *)pinst; }

/* Called before vkCreateDevice: add the features the frame generation shaders need to the game's device. */
void vkshim_lsfg_predevice(long phys, long pci) {
    static VkPhysicalDeviceVulkan12Features extra12;
    static VkPhysicalDeviceFeatures extraCore;
    if (!vkshim_lsfg_wanted() || !pci || !phys) return;
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)vkshim_real_proc("vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)vkshim_real_proc("vkGetPhysicalDeviceFeatures2KHR");
    if (!gf2) { LOGI("lsfg: off, no vkGetPhysicalDeviceFeatures2"); g_lg_dev_ok = 0; return; }
    VkPhysicalDeviceVulkan12Features s12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 s2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &s12 };
    gf2((VkPhysicalDevice)phys, &s2);
    LOGI("lsfg: GPU features: vulkanMemoryModel=%d deviceScope=%d shaderFloat16=%d extFormats=%d readNoFmt=%d writeNoFmt=%d int16=%d",
         (int)s12.vulkanMemoryModel, (int)s12.vulkanMemoryModelDeviceScope, (int)s12.shaderFloat16,
         (int)s2.features.shaderStorageImageExtendedFormats, (int)s2.features.shaderStorageImageReadWithoutFormat,
         (int)s2.features.shaderStorageImageWriteWithoutFormat, (int)s2.features.shaderInt16);
    if (!s12.vulkanMemoryModel) { LOGI("lsfg: off, this GPU driver has no vulkanMemoryModel"); g_lg_dev_ok = 0; return; }
    VkDeviceCreateInfo *ci = (VkDeviceCreateInfo *)pci;
    VkPhysicalDeviceFeatures2 *f2 = NULL; VkPhysicalDeviceVulkan12Features *v12 = NULL;
    for (VkBaseOutStructure *n = (VkBaseOutStructure *)ci->pNext; n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) f2 = (VkPhysicalDeviceFeatures2 *)n;
        else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) v12 = (VkPhysicalDeviceVulkan12Features *)n;
    }
    VkPhysicalDeviceFeatures *core = NULL;
    if (f2) core = &f2->features;
    else {
        extraCore = ci->pEnabledFeatures ? *ci->pEnabledFeatures : (VkPhysicalDeviceFeatures){ 0 };
        ci->pEnabledFeatures = &extraCore;
        core = &extraCore;
    }
    core->shaderStorageImageExtendedFormats |= s2.features.shaderStorageImageExtendedFormats;
    core->shaderStorageImageReadWithoutFormat |= s2.features.shaderStorageImageReadWithoutFormat;
    core->shaderStorageImageWriteWithoutFormat |= s2.features.shaderStorageImageWriteWithoutFormat;
    core->shaderInt16 |= s2.features.shaderInt16;
    if (v12) {
        v12->vulkanMemoryModel = VK_TRUE;
        v12->vulkanMemoryModelDeviceScope |= s12.vulkanMemoryModelDeviceScope;
        v12->shaderFloat16 |= s12.shaderFloat16;
    } else {
        memset(&extra12, 0, sizeof extra12);
        extra12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        extra12.vulkanMemoryModel = VK_TRUE;
        extra12.vulkanMemoryModelDeviceScope = s12.vulkanMemoryModelDeviceScope;
        extra12.shaderFloat16 = s12.shaderFloat16;
        extra12.pNext = (void *)ci->pNext;
        ci->pNext = &extra12;
    }
    LOGI("lsfg: device features added for frame generation");
}

static int lg_load(void) {
    if (LG.state) return LG.state > 0;
    const char *p = getenv("LAS_LSFG_LIB");
    void *h = p && *p ? dlopen(p, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (h) {
        *(void **)&LG.init = dlsym(h, "las_lsfg_init");
        *(void **)&LG.ctx = dlsym(h, "las_lsfg_context");
        *(void **)&LG.present = dlsym(h, "las_lsfg_present");
        *(void **)&LG.del = dlsym(h, "las_lsfg_delete");
    }
    if (!h || !LG.init || !LG.ctx || !LG.present || !LG.del) {
        LOGI("lsfg: off, can't load the frame generation library (%s): %s", p ? p : "no path", h ? (!LG.init ? "no las_lsfg_init" : !LG.ctx ? "no las_lsfg_context" : !LG.present ? "no las_lsfg_present" : "no las_lsfg_delete") : dlerror());
        LG.state = -1;
        return 0;
    }
    LG.state = 1;
    return 1;
}

typedef struct {
    int used, failed;
    VkSwapchainKHR sc; VkDevice dev; VkFormat fmt; VkExtent2D in, out; uint32_t n, family;
    VkImage rimg[MAXI], pimg[MAXI]; VkDeviceMemory pmem[MAXI];
    VkImageView pview[MAXI], rview[MAXI]; VkFramebuffer fb[MAXI];
    VkDescriptorSet ds[MAXI]; VkCommandBuffer cmd[MAXI]; VkSemaphore sem[MAXI];
    VkSampler samp; VkRenderPass rp; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
    VkDescriptorPool dp; VkCommandPool pool; VkShaderModule vs, fs;
    /* FXAA before the upscale: game image -> FXAA -> intermediate image (game size) -> SGSR */
    int fx; VkImage qimg[MAXI]; VkDeviceMemory qmem[MAXI]; VkImageView qview[MAXI]; VkFramebuffer qfb[MAXI];
    VkDescriptorSet dsq[MAXI]; VkRenderPass rpm; VkPipeline pipefx; VkShaderModule fvs, ffs;
    /* FSR 1 (mode 2): game image [-> FXAA] -> EASU -> full-size image -> RCAS -> swapchain image */
    VkRect2D rect;                       /* where the picture goes on the screen (smaller than the screen when the ratio is kept) */
    int mode; float rcas; VkImage uimg[MAXI]; VkDeviceMemory umem[MAXI]; VkImageView uview[MAXI]; VkFramebuffer ufb[MAXI];
    VkDescriptorSet dsu[MAXI]; VkPipeline pipee; VkShaderModule efs;
    float vinfo[4]; uint32_t easu[16], rcasc[4];      /* push constants, same for every pass of the chain */
    /* frame generation (LSFG): the proxy image is copied into fimg[], the generated frame lands in oimg and is run through
     * the same chain as a real frame, as "slot n" (cmdg[j] draws it into swapchain image j) */
    int lg, lgbad, ctx; VkQueue lgq; unsigned long fidx;
    VkImage fimg[2], oimg; VkDeviceMemory fmem[2], omem; VkImageView oview;
    VkCommandBuffer cpy[MAXIMG][2], cmdg[MAXIMG]; VkSemaphore semg[MAXIMG], acq[MAXIMG];
    unsigned long presents;
} Sg;
static Sg g_sg[MAXSC];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static Sg *find(VkSwapchainKHR sc) {
    Sg *r = NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAXSC; i++) if (g_sg[i].used && g_sg[i].sc == sc) { r = &g_sg[i]; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}

int vkshim_sgsr_enabled(void) {
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("LAS_SGSR"), *f = getenv("LAS_FSR"), *k = getenv("LAS_KEEP"), *w = getenv("LAS_RES_W");
        s = ((e && *e == '1') || (f && *f == '1') || (k && *k == '1' && w && atoi(w) > 0)) ? 1 : 0;
    }
    return s || vkshim_lsfg_wanted();
}

/* 1 = SGSR, 2 = FSR 1, 3 = plain scaling (only used to keep the picture's ratio when no upscaler is chosen) */
static int sgsr_mode(void) {
    const char *f = getenv("LAS_FSR"), *e = getenv("LAS_SGSR");
    if (f && *f == '1') return 2;
    if (e && *e == '1') return 1;
    return 3;
}

/* frees everything except the real swapchain itself */
static void teardown(Sg *c) {
    VkDevice d = c->dev;
    if (c->pool) F.DestroyCommandPool(d, c->pool, NULL);           /* frees its command buffers */
    if (c->ctx > 0 && LG.del) LG.del(c->ctx);
    for (int i = 0; i < MAXIMG; i++) {
        if (c->semg[i]) F.DestroySemaphore(d, c->semg[i], NULL);
        if (c->acq[i]) F.DestroySemaphore(d, c->acq[i], NULL);
    }
    if (c->oview) F.DestroyImageView(d, c->oview, NULL);
    if (c->oimg) F.DestroyImage(d, c->oimg, NULL);
    if (c->omem) F.FreeMemory(d, c->omem, NULL);
    for (int k = 0; k < 2; k++) {
        if (c->fimg[k]) F.DestroyImage(d, c->fimg[k], NULL);
        if (c->fmem[k]) F.FreeMemory(d, c->fmem[k], NULL);
    }
    for (int i = 0; i < MAXI; i++) {
        if (c->sem[i]) F.DestroySemaphore(d, c->sem[i], NULL);
        if (c->fb[i]) F.DestroyFramebuffer(d, c->fb[i], NULL);
        if (c->rview[i]) F.DestroyImageView(d, c->rview[i], NULL);
        if (c->pview[i]) F.DestroyImageView(d, c->pview[i], NULL);
        if (c->pimg[i]) F.DestroyImage(d, c->pimg[i], NULL);
        if (c->pmem[i]) F.FreeMemory(d, c->pmem[i], NULL);
        if (c->ufb[i]) F.DestroyFramebuffer(d, c->ufb[i], NULL);
        if (c->uview[i]) F.DestroyImageView(d, c->uview[i], NULL);
        if (c->uimg[i]) F.DestroyImage(d, c->uimg[i], NULL);
        if (c->umem[i]) F.FreeMemory(d, c->umem[i], NULL);
        if (c->qfb[i]) F.DestroyFramebuffer(d, c->qfb[i], NULL);
        if (c->qview[i]) F.DestroyImageView(d, c->qview[i], NULL);
        if (c->qimg[i]) F.DestroyImage(d, c->qimg[i], NULL);
        if (c->qmem[i]) F.FreeMemory(d, c->qmem[i], NULL);
    }
    if (c->pipee) F.DestroyPipeline(d, c->pipee, NULL);
    if (c->efs) F.DestroyShaderModule(d, c->efs, NULL);
    if (c->pipefx) F.DestroyPipeline(d, c->pipefx, NULL);
    if (c->rpm) F.DestroyRenderPass(d, c->rpm, NULL);
    if (c->fvs) F.DestroyShaderModule(d, c->fvs, NULL);
    if (c->ffs) F.DestroyShaderModule(d, c->ffs, NULL);
    if (c->pipe) F.DestroyPipeline(d, c->pipe, NULL);
    if (c->dp) F.DestroyDescriptorPool(d, c->dp, NULL);
    if (c->pl) F.DestroyPipelineLayout(d, c->pl, NULL);
    if (c->dsl) F.DestroyDescriptorSetLayout(d, c->dsl, NULL);
    if (c->rp) F.DestroyRenderPass(d, c->rp, NULL);
    if (c->vs) F.DestroyShaderModule(d, c->vs, NULL);
    if (c->fs) F.DestroyShaderModule(d, c->fs, NULL);
    if (c->samp) F.DestroySampler(d, c->samp, NULL);
}

static uint32_t pick_memtype(VkDevice dev, uint32_t bits) {
    VkPhysicalDevice phys = vkshim_phys_of(dev);
    if (phys) {
        VkPhysicalDeviceMemoryProperties mp;
        F.GetMemProps(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    }
    for (uint32_t i = 0; i < 32; i++) if (bits & (1u << i)) return i;
    return 0;
}

#define ST(what, call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { LOGI("sgsr: %s failed (%d)", what, (int)r_); return -1; } } while (0)

static VkResult mk_pipe(Sg *c, VkShaderModule vs, VkShaderModule fs, VkRenderPass rp, VkRect2D area, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { (float)area.offset.x, (float)area.offset.y, (float)area.extent.width, (float)area.extent.height, 0, 1 };
    VkRect2D sc = area;
    VkPipelineViewportStateCreateInfo vs2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs2, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = c->pl, .renderPass = rp, .subpass = 0,
        .basePipelineIndex = -1 };
    return F.CreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gpi, NULL, out);
}

/* Records the chain for one picture: [FXAA] -> [EASU] -> final pass into swapchain framebuffer fbi.
 * slot < n: the game's proxy image (left in PRESENT_SRC by the game); slot n (gen): the generated frame (GENERAL). */
static void rec(Sg *c, VkCommandBuffer cb2, uint32_t slot, uint32_t fbi, int gen, VkClearValue black, VkImageSubresourceRange rng) {
    VkImage src = gen ? c->oimg : c->pimg[slot];
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = gen ? (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
                             : (VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT),
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = src, .subresourceRange = rng };
    F.CmdPipelineBarrier(cb2, gen ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT) : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    if (c->fx) {                                           /* (mode 2 below reads this pass's output) */
        VkRenderPassBeginInfo rbq = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rpm, .framebuffer = c->qfb[slot],
            .renderArea = { { 0, 0 }, c->in }, .clearValueCount = 1, .pClearValues = &black };
        F.CmdBeginRenderPass(cb2, &rbq, VK_SUBPASS_CONTENTS_INLINE);
        F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipefx);
        F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1, &c->ds[slot], 0, NULL);
        F.CmdDraw(cb2, 3, 1, 0, 0);
        F.CmdEndRenderPass(cb2);
    }
    if (c->mode == 2) {                                    /* EASU: game (or FXAA) image -> full-size image */
        VkRenderPassBeginInfo rbu = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rpm, .framebuffer = c->ufb[slot],
            .renderArea = { { 0, 0 }, c->out }, .clearValueCount = 1, .pClearValues = &black };
        F.CmdBeginRenderPass(cb2, &rbu, VK_SUBPASS_CONTENTS_INLINE);
        F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipee);
        F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1, c->fx ? &c->dsq[slot] : &c->ds[slot], 0, NULL);
        F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, c->easu);
        F.CmdDraw(cb2, 3, 1, 0, 0);
        F.CmdEndRenderPass(cb2);
    }
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rp, .framebuffer = c->fb[fbi],
        .renderArea = { { 0, 0 }, c->out }, .clearValueCount = 1, .pClearValues = &black };
    F.CmdBeginRenderPass(cb2, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
    F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1,
                            c->mode == 2 ? &c->dsu[slot] : (c->fx ? &c->dsq[slot] : &c->ds[slot]), 0, NULL);
    if (c->mode == 2) F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c->rcasc);   /* RCAS: sharpen at full size */
    else F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c->vinfo);
    F.CmdDraw(cb2, 3, 1, 0, 0);
    F.CmdEndRenderPass(cb2);
    /* hand the image back: the game's in PRESENT_SRC, the generated one in GENERAL for the next generation */
    VkImageMemoryBarrier b2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = gen ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : 0,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = gen ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = src, .subresourceRange = rng };
    F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, gen ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &b2);
}

/* proxy images, views, pipeline and one pre-recorded command buffer per image; returns 0 on success */
static int build(Sg *c, VkImageUsageFlags gameUsage) {
    VkDevice d = c->dev;
    uint32_t n = c->n, n1 = n + (c->lg ? 1 : 0);
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkClearValue black; memset(&black, 0, sizeof black);
    VkImageUsageFlags fgUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (uint32_t i = 0; i < n1; i++) {
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = c->fmt,
            .extent = { c->in.width, c->in.height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = gameUsage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                         (c->lg ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = c->fmt, .components = { 0, 0, 0, 0 }, .subresourceRange = rng };
        if (i < n) {
            ST("create proxy image", F.CreateImage(d, &ici, NULL, &c->pimg[i]));
            F.GetImageMemoryRequirements(d, c->pimg[i], &mr);
            mai.allocationSize = mr.size; mai.memoryTypeIndex = pick_memtype(d, mr.memoryTypeBits);
            ST("allocate proxy memory", F.AllocateMemory(d, &mai, NULL, &c->pmem[i]));
            ST("bind proxy memory", F.BindImageMemory(d, c->pimg[i], c->pmem[i], 0));
            vci.image = c->pimg[i];
            ST("create proxy view", F.CreateImageView(d, &vci, NULL, &c->pview[i]));
            vci.image = c->rimg[i];
            ST("create swapchain view", F.CreateImageView(d, &vci, NULL, &c->rview[i]));
        } else {
            /* slot n: the generated frame (float image the frame generation writes into), plus two inputs for it */
            VkImageCreateInfo fi = ici;
            fi.format = VK_FORMAT_R16G16B16A16_SFLOAT; fi.usage = fgUse;
            for (int k = 0; k < 3; k++) {
                VkImage *im = k < 2 ? &c->fimg[k] : &c->oimg;
                VkDeviceMemory *mm = k < 2 ? &c->fmem[k] : &c->omem;
                ST("create frame generation image", F.CreateImage(d, &fi, NULL, im));
                F.GetImageMemoryRequirements(d, *im, &mr);
                mai.allocationSize = mr.size; mai.memoryTypeIndex = pick_memtype(d, mr.memoryTypeBits);
                ST("allocate frame generation memory", F.AllocateMemory(d, &mai, NULL, mm));
                ST("bind frame generation memory", F.BindImageMemory(d, *im, *mm, 0));
            }
            VkImageViewCreateInfo ov = vci; ov.format = VK_FORMAT_R16G16B16A16_SFLOAT; ov.image = c->oimg;
            ST("create generated view", F.CreateImageView(d, &ov, NULL, &c->oview));
        }
        if (c->fx) {
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ST("create intermediate image", F.CreateImage(d, &ici, NULL, &c->qimg[i]));
            F.GetImageMemoryRequirements(d, c->qimg[i], &mr);
            mai.allocationSize = mr.size; mai.memoryTypeIndex = pick_memtype(d, mr.memoryTypeBits);
            ST("allocate intermediate memory", F.AllocateMemory(d, &mai, NULL, &c->qmem[i]));
            ST("bind intermediate memory", F.BindImageMemory(d, c->qimg[i], c->qmem[i], 0));
            vci.image = c->qimg[i];
            ST("create intermediate view", F.CreateImageView(d, &vci, NULL, &c->qview[i]));
        }
        if (c->mode == 2) {
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.extent.width = c->out.width; ici.extent.height = c->out.height;
            ST("create upscaled image", F.CreateImage(d, &ici, NULL, &c->uimg[i]));
            F.GetImageMemoryRequirements(d, c->uimg[i], &mr);
            mai.allocationSize = mr.size; mai.memoryTypeIndex = pick_memtype(d, mr.memoryTypeBits);
            ST("allocate upscaled memory", F.AllocateMemory(d, &mai, NULL, &c->umem[i]));
            ST("bind upscaled memory", F.BindImageMemory(d, c->uimg[i], c->umem[i], 0));
            vci.image = c->uimg[i];
            ST("create upscaled view", F.CreateImageView(d, &vci, NULL, &c->uview[i]));
        }
    }

    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.25f, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    ST("create sampler", F.CreateSampler(d, &sci, NULL, &c->samp));

    VkAttachmentDescription ad = { .format = c->fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,      /* black bars when the ratio is kept */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
        .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep };
    ST("create render pass", F.CreateRenderPass(d, &rpi, NULL, &c->rp));
    for (uint32_t i = 0; i < n; i++) {
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = c->rp, .attachmentCount = 1,
            .pAttachments = &c->rview[i], .width = c->out.width, .height = c->out.height, .layers = 1 };
        ST("create framebuffer", F.CreateFramebuffer(d, &fi, NULL, &c->fb[i]));
    }
    if (c->fx || c->mode == 2) {
        VkAttachmentDescription am = ad;
        am.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkSubpassDependency dm[2] = {
            { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
            { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT } };
        VkRenderPassCreateInfo rpm = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &am,
            .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dm };
        ST("create intermediate render pass", F.CreateRenderPass(d, &rpm, NULL, &c->rpm));
        for (uint32_t i = 0; i < n1; i++) {
            if (c->fx) {
                VkFramebufferCreateInfo fq = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = c->rpm, .attachmentCount = 1,
                    .pAttachments = &c->qview[i], .width = c->in.width, .height = c->in.height, .layers = 1 };
                ST("create intermediate framebuffer", F.CreateFramebuffer(d, &fq, NULL, &c->qfb[i]));
            }
            if (c->mode == 2) {
                VkFramebufferCreateInfo fu = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = c->rpm, .attachmentCount = 1,
                    .pAttachments = &c->uview[i], .width = c->out.width, .height = c->out.height, .layers = 1 };
                ST("create upscaled framebuffer", F.CreateFramebuffer(d, &fu, NULL, &c->ufb[i]));
            }
        }
    }

    VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
    ST("create descriptor set layout", F.CreateDescriptorSetLayout(d, &dli, NULL, &c->dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    ST("create pipeline layout", F.CreatePipelineLayout(d, &pli, NULL, &c->pl));
    uint32_t nsets = n1 * (1 + (c->fx ? 1 : 0) + (c->mode == 2 ? 1 : 0));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nsets };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = nsets, .poolSizeCount = 1, .pPoolSizes = &ps };
    ST("create descriptor pool", F.CreateDescriptorPool(d, &dpi, NULL, &c->dp));
    VkDescriptorSetLayout lays[3 * MAXI];
    VkDescriptorSet sets[3 * MAXI];
    for (uint32_t i = 0; i < nsets; i++) lays[i] = c->dsl;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp,
        .descriptorSetCount = nsets, .pSetLayouts = lays };
    ST("allocate descriptor sets", F.AllocateDescriptorSets(d, &dai, sets));
    for (uint32_t i = 0; i < n1; i++) {
        c->ds[i] = sets[i];                                   /* game image (slot n: the generated frame) */
        VkDescriptorImageInfo dii = { c->samp, i < n ? c->pview[i] : c->oview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds[i], .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
        F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);
        if (c->fx) {
            c->dsq[i] = sets[n1 + i];                         /* FXAA output */
            VkDescriptorImageInfo dq = { c->samp, c->qview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wds.dstSet = c->dsq[i]; wds.pImageInfo = &dq;
            F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);
        }
        if (c->mode == 2) {
            c->dsu[i] = sets[(c->fx ? 2 * n1 : n1) + i];      /* EASU output */
            VkDescriptorImageInfo du = { c->samp, c->uview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wds.dstSet = c->dsu[i]; wds.pImageInfo = &du;
            F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);
        }
    }

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkRect2D full = { { 0, 0 }, c->out };
    if (c->mode == 3) {
        smi.codeSize = sizeof sgsr_vs_spv; smi.pCode = sgsr_vs_spv;
        ST("create vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->vs));
        smi.codeSize = sizeof scale_fs_spv; smi.pCode = scale_fs_spv;
        ST("create scale shader", F.CreateShaderModule(d, &smi, NULL, &c->fs));
        ST("create scale pipeline", mk_pipe(c, c->vs, c->fs, c->rp, c->rect, &c->pipe));
    } else if (c->mode == 2) {
        smi.codeSize = sizeof fsr_vs_spv; smi.pCode = fsr_vs_spv;
        ST("create vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->vs));
        smi.codeSize = sizeof fsr_rcas_fs_spv; smi.pCode = fsr_rcas_fs_spv;
        ST("create RCAS shader", F.CreateShaderModule(d, &smi, NULL, &c->fs));
        smi.codeSize = sizeof fsr_easu_fs_spv; smi.pCode = fsr_easu_fs_spv;
        ST("create EASU shader", F.CreateShaderModule(d, &smi, NULL, &c->efs));
        ST("create RCAS pipeline", mk_pipe(c, c->vs, c->fs, c->rp, full, &c->pipe));
        ST("create EASU pipeline", mk_pipe(c, c->vs, c->efs, c->rpm, c->rect, &c->pipee));
    } else {
        smi.codeSize = sizeof sgsr_vs_spv; smi.pCode = sgsr_vs_spv;
        ST("create vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->vs));
        smi.codeSize = sizeof sgsr_fs_spv; smi.pCode = sgsr_fs_spv;
        ST("create fragment shader", F.CreateShaderModule(d, &smi, NULL, &c->fs));
        ST("create pipeline", mk_pipe(c, c->vs, c->fs, c->rp, c->rect, &c->pipe));
    }
    if (c->fx) {
        smi.codeSize = sizeof fxaa_vs_spv; smi.pCode = fxaa_vs_spv;
        ST("create FXAA vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->fvs));
        smi.codeSize = sizeof fxaa_fs_spv; smi.pCode = fxaa_fs_spv;
        ST("create FXAA fragment shader", F.CreateShaderModule(d, &smi, NULL, &c->ffs));
        ST("create FXAA pipeline", mk_pipe(c, c->fvs, c->ffs, c->rpm, (VkRect2D){ { 0, 0 }, c->in }, &c->pipefx));
    }

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->family };
    ST("create command pool", F.CreateCommandPool(d, &cpi, NULL, &c->pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = n };
    ST("allocate command buffers", F.AllocateCommandBuffers(d, &cai, c->cmd));
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++) ST("create semaphore", F.CreateSemaphore(d, &semi, NULL, &c->sem[i]));

    c->vinfo[0] = 1.0f / (float)c->in.width; c->vinfo[1] = 1.0f / (float)c->in.height; c->vinfo[2] = (float)c->in.width; c->vinfo[3] = (float)c->in.height;
    /* FSR 1 constants, the same as FsrEasuCon / FsrRcasCon in ffx_fsr1.h (input size = viewport size here) */
    {
        float iw = (float)c->in.width, ih = (float)c->in.height, ow = (float)c->rect.extent.width, oh = (float)c->rect.extent.height;
        float ox = (float)c->rect.offset.x, oy = (float)c->rect.offset.y;      /* EASU draws into the rect; the pixel position is made rect-relative here */
        float e[16] = { iw / ow, ih / oh, 0.5f * iw / ow - 0.5f - ox * iw / ow, 0.5f * ih / oh - 0.5f - oy * ih / oh,
                        1.0f / iw, 1.0f / ih, 1.0f / iw, -1.0f / ih,
                        -1.0f / iw, 2.0f / ih, 1.0f / iw, 2.0f / ih,
                        0.0f, 4.0f / ih, 0.0f, 0.0f };
        memcpy(c->easu, e, sizeof c->easu);                    /* the shader reads the raw bits of these floats */
        float sh = exp2f(-c->rcas);
        memcpy(&c->rcasc[0], &sh, 4);
    }
    for (uint32_t i = 0; i < n; i++) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        ST("begin command buffer", F.BeginCommandBuffer(c->cmd[i], &bi));
        rec(c, c->cmd[i], i, i, 0, black, rng);
        ST("end command buffer", F.EndCommandBuffer(c->cmd[i]));
    }
    if (c->lg) {
        VkCommandBuffer cb[4 * MAXIMG];
        VkCommandBufferAllocateInfo cg = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3 * n };
        ST("allocate frame generation command buffers", F.AllocateCommandBuffers(d, &cg, cb));
        for (uint32_t j = 0; j < n; j++) {
            c->cmdg[j] = cb[j]; c->cpy[j][0] = cb[n + j]; c->cpy[j][1] = cb[2 * n + j];
            ST("create semaphore", F.CreateSemaphore(d, &semi, NULL, &c->semg[j]));
            ST("create semaphore", F.CreateSemaphore(d, &semi, NULL, &c->acq[j]));
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            ST("begin command buffer", F.BeginCommandBuffer(c->cmdg[j], &bi));
            rec(c, c->cmdg[j], n, j, 1, black, rng);                  /* generated frame (slot n) -> swapchain image j */
            ST("end command buffer", F.EndCommandBuffer(c->cmdg[j]));
            /* copy of the game's image into the frame generation input (parity 0 / 1) */
            for (int k = 0; k < 2; k++) {
                VkCommandBuffer cb2 = c->cpy[j][k];
                ST("begin command buffer", F.BeginCommandBuffer(cb2, &bi));
                VkImageMemoryBarrier bb[2];
                memset(bb, 0, sizeof bb);
                for (int q = 0; q < 2; q++) { bb[q].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; bb[q].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bb[q].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb[q].subresourceRange = rng; }
                bb[0].image = c->pimg[j]; bb[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                bb[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; bb[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; bb[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bb[1].image = c->fimg[k]; bb[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; bb[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bb[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bb[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, bb);
                VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 } };
                bl.srcOffsets[1] = (VkOffset3D){ (int32_t)c->in.width, (int32_t)c->in.height, 1 };
                bl.dstOffsets[1] = bl.srcOffsets[1];
                F.CmdBlitImage(cb2, c->pimg[j], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->fimg[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
                bb[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; bb[0].dstAccessMask = 0;
                bb[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; bb[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                bb[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                bb[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bb[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, bb);
                ST("end command buffer", F.EndCommandBuffer(cb2));
            }
        }
    }
    return 0;
}

/* Creating the pipeline runs the driver's shader compiler, which needs more stack than some game threads have. */
struct build_arg { Sg *c; VkImageUsageFlags usage; int r; };
static void *build_thread(void *p) { struct build_arg *a = p; a->r = build(a->c, a->usage); return NULL; }
static int build_big_stack(Sg *c, VkImageUsageFlags usage) {
    struct build_arg a = { c, usage, -1 };
    pthread_attr_t at; pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, build_thread, &a) != 0) { LOGI("sgsr: couldn't start the setup thread"); pthread_attr_destroy(&at); return -1; }
    pthread_join(t, NULL);
    pthread_attr_destroy(&at);
    return a.r;
}

/* ---------- entry points ---------- */

/* Returns 1 when the swapchain was created here (result in *res); 0 when the caller should create it as usual. */
int vkshim_sgsr_create(long device, long pci, long palloc, long psc, sg_f8 real, long *res) {
    if (!vkshim_sgsr_enabled() || !pci || !psc || !fn_init()) return 0;
    VkSwapchainCreateInfoKHR ci = *(const VkSwapchainCreateInfoKHR *)pci;
    VkDevice dev = (VkDevice)device;
    const char *why = NULL;
    VkPhysicalDevice phys = vkshim_phys_of(dev);
    int fok = 0; uint32_t family = vkshim_first_queue_family(&fok);
    VkSurfaceCapabilitiesKHR caps;
    if (ci.flags != 0 || ci.imageArrayLayers != 1) why = "unusual swapchain flags";
    else if (ci.imageSharingMode != VK_SHARING_MODE_EXCLUSIVE) why = "concurrent sharing";
    else if (ci.preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) why = "rotated surface";
    else if (!phys) why = "physical device unknown";
    else if (!fok) why = "no queue seen yet";
    if (!why && F.GetCaps(phys, ci.surface, &caps) != VK_SUCCESS) why = "surface size unavailable";
    int wantMode = sgsr_mode();
    int wantLg = vkshim_lsfg_wanted() && lg_load();           /* frame generation needs our own swapchain whatever the sizes */
    if (!why && caps.currentExtent.width == 0xFFFFFFFFu) why = "surface size unknown";
    if (!why && !wantLg && caps.currentExtent.width == ci.imageExtent.width && caps.currentExtent.height == ci.imageExtent.height)
        why = "the game's swapchain is as big as the screen";
    if (!why && !wantLg && wantMode == 3 && (uint64_t)ci.imageExtent.width * caps.currentExtent.height == (uint64_t)ci.imageExtent.height * caps.currentExtent.width)
        why = "the game's picture already has the screen's ratio";
    if (!why && wantMode != 3 && (caps.currentExtent.width < ci.imageExtent.width || caps.currentExtent.height < ci.imageExtent.height))
        why = "the game's swapchain is bigger than the screen";
    int slot = -1;
    if (!why) {
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < MAXSC; i++) if (!g_sg[i].used) { slot = i; g_sg[i].used = 1; break; }
        pthread_mutex_unlock(&g_mu);
        if (slot < 0) why = "too many swapchains";
    }
    if (why) { LOGI("sgsr: not used for this swapchain: %s", why); return 0; }

    Sg *c = &g_sg[slot];
    memset(c, 0, sizeof *c);
    { const char *fe = getenv("LAS_FXAA"); c->fx = (fe && *fe == '1'); }
    { const char *ff = getenv("LAS_FSR"), *rs = getenv("LAS_RCAS");
      c->mode = wantMode; (void)ff;
      c->rcas = (rs && *rs) ? (float)atof(rs) : 0.2f;
      if (c->rcas < 0.0f) c->rcas = 0.0f;
      if (c->rcas > 2.0f) c->rcas = 2.0f; }
    c->rect = (VkRect2D){ { 0, 0 }, caps.currentExtent };
    { const char *kp = getenv("LAS_KEEP");
      if (kp && *kp == '1') {                                   /* keep the game's ratio: fit it inside the screen, black bars around */
          float sx = (float)caps.currentExtent.width / (float)ci.imageExtent.width, sy = (float)caps.currentExtent.height / (float)ci.imageExtent.height;
          float sc = sx < sy ? sx : sy;
          uint32_t rw = (uint32_t)((float)ci.imageExtent.width * sc + 0.5f), rh = (uint32_t)((float)ci.imageExtent.height * sc + 0.5f);
          if (rw > caps.currentExtent.width) rw = caps.currentExtent.width;
          if (rh > caps.currentExtent.height) rh = caps.currentExtent.height;
          c->rect.extent.width = rw; c->rect.extent.height = rh;
          c->rect.offset.x = (int32_t)((caps.currentExtent.width - rw) / 2); c->rect.offset.y = (int32_t)((caps.currentExtent.height - rh) / 2);
      } }
    c->lg = wantLg;
    c->used = 1; c->dev = dev; c->fmt = ci.imageFormat; c->in = ci.imageExtent; c->out = caps.currentExtent; c->family = family;

    VkSwapchainCreateInfoKHR rci = ci;
    rci.imageExtent = caps.currentExtent;
    rci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (c->lg) {                                           /* one image more: a generated frame is shown next to the game's */
        rci.minImageCount = ci.minImageCount + 1;
        if (caps.maxImageCount && rci.minImageCount > caps.maxImageCount) rci.minImageCount = caps.maxImageCount;
    }
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    long r = real(device, (long)&rci, palloc, (long)&sc, 0, 0, 0, 0);
    if (r != VK_SUCCESS) {
        LOGI("sgsr: the full-size swapchain failed (%d), falling back", (int)r);
        c->used = 0; return 0;
    }
    c->sc = sc;
    uint32_t n = 0;
    if (F.GetSwapchainImagesKHR(dev, sc, &n, NULL) != VK_SUCCESS || n == 0 || n > MAXIMG ||
        F.GetSwapchainImagesKHR(dev, sc, &n, c->rimg) != VK_SUCCESS) {
        LOGI("sgsr: swapchain images unusable (%u), falling back", n);
        F.DestroySwapchainKHR(dev, sc, NULL); c->used = 0; return 0;
    }
    c->n = n;
    if (build_big_stack(c, ci.imageUsage) != 0) {
        LOGI("sgsr: setup failed, falling back to the plain swapchain");
        F.DeviceWaitIdle(dev);
        teardown(c);
        F.DestroySwapchainKHR(dev, sc, NULL);
        memset(c, 0, sizeof *c);
        return 0;
    }
    *(VkSwapchainKHR *)psc = sc;
    *res = VK_SUCCESS;
    LOGI("sgsr: on (%s): game renders %ux%u, screen %ux%u, %u images, format %d%s", c->mode == 2 ? "FSR 1 EASU+RCAS" : (c->mode == 1 ? "SGSR 1" : "plain scaling"),
         c->in.width, c->in.height, c->out.width, c->out.height, n, (int)c->fmt, c->fx ? ", FXAA before the upscale" : "");
    if (c->lg) LOGI("lsfg: swapchain ready for frame generation (starts at the first present)");
    LOGI("sgsr: picture placed at %d,%d size %ux%u%s", c->rect.offset.x, c->rect.offset.y, c->rect.extent.width, c->rect.extent.height,
         (c->rect.extent.width == c->out.width && c->rect.extent.height == c->out.height) ? " (fills the screen)" : " (ratio kept, black bars)");
    if (c->mode == 2) LOGI("sgsr: RCAS sharpness %.2f stops (0 = sharpest)", c->rcas);
    return 1;
}

/* vkGetSwapchainImagesKHR: the game gets the proxy images of an SGSR swapchain. */
long vkshim_sgsr_images(long device, long swapchain, long pcount, long pimages, sg_f4 real) {
    Sg *c = swapchain ? find((VkSwapchainKHR)swapchain) : NULL;
    if (!c || !pcount) return real(device, swapchain, pcount, pimages);
    uint32_t *cnt = (uint32_t *)pcount;
    if (!pimages) { *cnt = c->n; return VK_SUCCESS; }
    uint32_t k = *cnt < c->n ? *cnt : c->n;
    for (uint32_t i = 0; i < k; i++) ((VkImage *)pimages)[i] = c->pimg[i];
    *cnt = k;
    return k < c->n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Frame generation starts at the first present (needs the queue): framegen init, the context, image layouts.
 * Runs on a big-stack thread because it compiles shaders. */
struct lg_arg { Sg *c; VkQueue q; int r; };
static void *lg_thread(void *p) {
    struct lg_arg *a = p; Sg *c = a->c;
    char err[400] = { 0 };
    a->r = -1;
    VkPhysicalDevice phys = vkshim_phys_of(c->dev);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)vkshim_real_proc("vkGetInstanceProcAddr");
    const char *dll = getenv("LAS_LSFG_DLL"), *pe = getenv("LAS_LSFG_PERF"), *fe = getenv("LAS_LSFG_FLOW");
    int perf = !(pe && *pe == '0');
    float flow = (fe && *fe) ? (float)atof(fe) : 0.5f;
    if (flow < 0.25f) flow = 0.25f;
    if (flow > 1.0f) flow = 1.0f;
    if (!phys || !gipa || !g_inst) { LOGI("lsfg: off, instance/device unknown"); return NULL; }
    if (!LG.ready) {
        LOGI("lsfg: starting (%s mode, flow scale %.2f)...", perf ? "performance" : "quality", flow);
        if (LG.init((void *)gipa, g_inst, phys, c->dev, c->family, a->q, perf, 1.0f / flow, 1, 0, dll, err, sizeof err) != 0) {
            LOGI("lsfg: off, init failed: %s", err); return NULL;
        }
        LG.ready = 1;
    }
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (F.AllocateCommandBuffers(c->dev, &cai, &cb) != VK_SUCCESS || F.BeginCommandBuffer(cb, &bi) != VK_SUCCESS) { LOGI("lsfg: off, setup command buffer"); return NULL; }
    VkImageMemoryBarrier bb[3];
    VkImage ims[3] = { c->fimg[0], c->fimg[1], c->oimg };
    memset(bb, 0, sizeof bb);
    for (int i = 0; i < 3; i++) {
        bb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; bb[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bb[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bb[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        bb[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb[i].image = ims[i];
        bb[i].subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
    F.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 3, bb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    if (F.EndCommandBuffer(cb) != VK_SUCCESS || F.QueueSubmit(a->q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS || F.QueueWaitIdle(a->q) != VK_SUCCESS) {
        LOGI("lsfg: off, couldn't set the image layouts"); return NULL;
    }
    uint64_t outs[1] = { (uint64_t)c->oimg };
    int id = LG.ctx((uint64_t)c->fimg[0], (uint64_t)c->fimg[1], outs, 1, c->in.width, c->in.height, (int)VK_FORMAT_R16G16B16A16_SFLOAT, err, sizeof err);
    if (id < 0) { LOGI("lsfg: off, context failed: %s", err); return NULL; }
    c->ctx = id;
    a->r = 0;
    LOGI("lsfg: on, 2x frame generation at %ux%u (context %d)", c->in.width, c->in.height, id);
    return NULL;
}
static int lg_start(Sg *c, VkQueue q) {
    struct lg_arg a = { c, q, -1 };
    pthread_attr_t at; pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    if (pthread_create(&t, &at, lg_thread, &a) != 0) { pthread_attr_destroy(&at); return -1; }
    pthread_join(t, NULL);
    pthread_attr_destroy(&at);
    return a.r;
}

/* Returns 1 when the present was handled here (result in *res). */
int vkshim_sgsr_present(long queue, long ppi, sg_f2 real, long *res) {
    const VkPresentInfoKHR *pi = (const VkPresentInfoKHR *)ppi;
    if (!pi || pi->swapchainCount != 1) return 0;
    Sg *c = find(pi->pSwapchains[0]);
    if (!c) return 0;
    uint32_t idx = pi->pImageIndices[0], fam;
    if (!c->failed && (idx >= c->n || pi->waitSemaphoreCount > 8 || !vkshim_q_family((VkQueue)queue, &fam) || fam != c->family)) {
        LOGI("sgsr: off, a present this code can't handle (image %u, %u wait semaphores)", idx, pi->waitSemaphoreCount);
        c->failed = 1;
    }
    if (!c->failed) {
        VkPipelineStageFlags stages[8];
        for (int i = 0; i < 8; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        int lg = 0;
        if (c->lg && !c->lgbad) {
            if (!c->lgq) {
                if (lg_start(c, (VkQueue)queue) == 0) c->lgq = (VkQueue)queue;
                else { LOGI("lsfg: frame generation is off, showing the game's own frames"); c->lgbad = 1; }
            } else if (c->lgq != (VkQueue)queue) { LOGI("lsfg: off, the game presents from another queue"); c->lgbad = 1; }
            lg = c->lg && !c->lgbad;
        }
        VkCommandBuffer cmds[2] = { c->cmd[idx], c->cmd[idx] };
        uint32_t ncmd = 1;
        if (lg) { cmds[0] = c->cpy[idx][c->fidx & 1]; cmds[1] = c->cmd[idx]; ncmd = 2; }
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = pi->waitSemaphoreCount,
            .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = stages, .commandBufferCount = ncmd,
            .pCommandBuffers = cmds, .signalSemaphoreCount = 1, .pSignalSemaphores = &c->sem[idx] };
        VkResult sr = F.QueueSubmit((VkQueue)queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_SUCCESS) {
            if (lg) {
                char err[300] = { 0 };
                if (LG.present(c->ctx, err, sizeof err) != 0) {
                    LOGI("lsfg: off, generation failed: %s", err); c->lgbad = 1;
                } else if (c->fidx > 0) {
                    /* the in-between frame goes out first, then the game's own frame */
                    uint32_t j = 0;
                    VkSemaphore as = c->acq[c->fidx % c->n];
                    VkResult ar = F.AcquireNextImageKHR(c->dev, c->sc, 100000000ull, as, VK_NULL_HANDLE, &j);
                    if ((ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) && j < c->n) {
                        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                        VkSubmitInfo sg = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &as,
                            .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &c->cmdg[j],
                            .signalSemaphoreCount = 1, .pSignalSemaphores = &c->semg[j] };
                        if (F.QueueSubmit((VkQueue)queue, 1, &sg, VK_NULL_HANDLE) == VK_SUCCESS) {
                            VkPresentInfoKHR pg = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &c->semg[j],
                                .swapchainCount = 1, .pSwapchains = &c->sc, .pImageIndices = &j };
                            real(queue, (long)&pg);
                            if (c->fidx == 1 || c->fidx == 300) LOGI("lsfg: generated frame %lu shown (image %u)", c->fidx, j);
                        } else { LOGI("lsfg: off, the generated frame's submit failed"); c->lgbad = 1; }
                    } else {
                        LOGI("lsfg: off, no free image for a generated frame (%d)", (int)ar);
                        c->lgbad = 1;
                    }
                }
                c->fidx++;
            }
            VkPresentInfoKHR p2 = *pi;
            p2.waitSemaphoreCount = 1;
            p2.pWaitSemaphores = &c->sem[idx];
            unsigned long k = ++c->presents;
            if (k == 1 || k == 2 || k == 300) LOGI("sgsr: frame %lu upscaled (image %u)", k, idx);
            *res = real(queue, (long)&p2);
            return 1;
        }
        LOGI("sgsr: off, the upscale submit failed (%d)", (int)sr);
        c->failed = 1;
    }
    /* nothing sensible can be shown any more; keep presenting so the game doesn't stall */
    *res = real(queue, ppi);
    return 1;
}

/* Returns 1 when the swapchain was one of ours (it is destroyed here). */
int vkshim_sgsr_destroy(long device, long swapchain, long palloc, sg_f4v real) {
    Sg *c = swapchain ? find((VkSwapchainKHR)swapchain) : NULL;
    if (!c) return 0;
    F.DeviceWaitIdle((VkDevice)device);
    teardown(c);
    real(device, swapchain, palloc, 0);
    pthread_mutex_lock(&g_mu);
    memset(c, 0, sizeof *c);
    pthread_mutex_unlock(&g_mu);
    LOGI("sgsr: swapchain destroyed");
    return 1;
}
