/* CAS (FidelityFX Contrast Adaptive Sharpening) applied to every frame the game presents.
 *
 * The shim's vkCreateSwapchainKHR / vkQueuePresentKHR / vkDestroySwapchainKHR wrappers call into this file.
 * Per swapchain, on the first present, this file creates: a private copy image, a full-screen pipeline running the
 * CAS fragment shader, and one pre-recorded command buffer per swapchain image. At present time the game's wait
 * semaphores are moved onto one extra submit that does:  swapchain image -> copy into private image ->
 * CAS shader draws private image back into the swapchain image. The present then waits on that submit.
 * Any failure switches CAS off for that swapchain and the frame is presented untouched.
 *
 * CAS algorithm: Copyright (c) 2019 Advanced Micro Devices, Inc., MIT licence (GPUOpen FidelityFX CAS).
 * The shader in cas_spv.h is a GLSL port of its CasFilter (see tools/cas.frag in the repository).
 */
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cas_spv.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GuestHost", __VA_ARGS__)
extern void *vkshim_real_proc(const char *name);
extern int vkshim_sgsr_create(long, long, long, long, long (*)(long, long, long, long, long, long, long, long), long *);
extern int vkshim_sgsr_present(long, long, long (*)(long, long), long *);
extern int vkshim_sgsr_destroy(long, long, long, void (*)(long, long, long, long));
extern long vkshim_sgsr_images(long, long, long, long, long (*)(long, long, long, long));
typedef long (*cas_f8)(long, long, long, long, long, long, long, long);
typedef long (*cas_f2)(long, long);
typedef void (*cas_f4v)(long, long, long, long);

#define MAXIMG 8
#define MAXSC 4
#define MAXQ 16
#define MAXDEV 4

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
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier; PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass; PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline; PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCmdPushConstants CmdPushConstants; PFN_vkCmdDraw CmdDraw;
    PFN_vkCreateSemaphore CreateSemaphore; PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkQueueSubmit QueueSubmit; PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties; PFN_vkDeviceWaitIdle DeviceWaitIdle;
} F;
static int g_fn_state = 0;                       /* 0 = not tried, 1 = ready, -1 = a function is missing */

static int cas_fn_init(void) {
    if (g_fn_state) return g_fn_state > 0;
    const char *missing = NULL;
#define LOAD(n) do { F.n = (PFN_vk##n)vkshim_real_proc("vk" #n); if (!F.n && !missing) missing = #n; } while (0)
    LOAD(CreateImage); LOAD(DestroyImage); LOAD(GetImageMemoryRequirements); LOAD(AllocateMemory); LOAD(FreeMemory);
    LOAD(BindImageMemory); LOAD(CreateImageView); LOAD(DestroyImageView); LOAD(CreateSampler); LOAD(DestroySampler);
    LOAD(CreateRenderPass); LOAD(DestroyRenderPass); LOAD(CreateFramebuffer); LOAD(DestroyFramebuffer);
    LOAD(CreateShaderModule); LOAD(DestroyShaderModule); LOAD(CreateDescriptorSetLayout); LOAD(DestroyDescriptorSetLayout);
    LOAD(CreatePipelineLayout); LOAD(DestroyPipelineLayout); LOAD(CreateGraphicsPipelines); LOAD(DestroyPipeline);
    LOAD(CreateDescriptorPool); LOAD(DestroyDescriptorPool); LOAD(AllocateDescriptorSets); LOAD(UpdateDescriptorSets);
    LOAD(CreateCommandPool); LOAD(DestroyCommandPool); LOAD(AllocateCommandBuffers); LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer); LOAD(CmdPipelineBarrier); LOAD(CmdCopyImage); LOAD(CmdBeginRenderPass); LOAD(CmdEndRenderPass);
    LOAD(CmdBindPipeline); LOAD(CmdBindDescriptorSets); LOAD(CmdPushConstants); LOAD(CmdDraw); LOAD(CreateSemaphore);
    LOAD(DestroySemaphore); LOAD(QueueSubmit); LOAD(GetSwapchainImagesKHR); LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(DeviceWaitIdle);
#undef LOAD
    if (missing) { LOGI("cas: off, the driver has no vk%s", missing); g_fn_state = -1; return 0; }
    g_fn_state = 1;
    return 1;
}

/* ---------- tracked handles ---------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { VkQueue q; uint32_t family; } g_queues[MAXQ];
static int g_nq = 0;
static struct { VkDevice dev; VkPhysicalDevice phys; } g_devs[MAXDEV];
static int g_ndev = 0;

typedef struct {
    int used, ready, failed;
    VkSwapchainKHR sc; VkDevice dev; VkFormat fmt; VkExtent2D ext;
    uint32_t n; VkImage imgs[MAXIMG]; VkImageView views[MAXIMG]; VkFramebuffer fbs[MAXIMG];
    VkCommandBuffer cmds[MAXIMG]; VkSemaphore sems[MAXIMG];
    VkImage priv; VkDeviceMemory pmem; VkImageView pview; VkSampler samp;
    VkRenderPass rp; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
    VkDescriptorPool dp; VkDescriptorSet ds; VkCommandPool pool; VkShaderModule vs, fs;
    unsigned long presents;
} Cas;
static Cas g_cas[MAXSC];

static float cas_strength(void) {
    static float s = -2.0f;
    if (s < -1.0f) {
        const char *e = getenv("LAS_CAS");
        s = (e && *e) ? (float)atof(e) : 0.0f;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
    }
    return s;
}

void vkshim_cas_getqueue(long dev, long family, long pq) {
    (void)dev;
    if (!pq) return;
    VkQueue q = *(VkQueue *)pq;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nq; i++) if (g_queues[i].q == q) { g_queues[i].family = (uint32_t)family; q = NULL; break; }
    if (q && g_nq < MAXQ) { g_queues[g_nq].q = q; g_queues[g_nq].family = (uint32_t)family; g_nq++; }
    pthread_mutex_unlock(&g_mu);
}
void vkshim_cas_getqueue2(long dev, long pinfo, long pq) {
    if (!pinfo) return;
    /* VkDeviceQueueInfo2: sType@0 pNext@8 flags@16 queueFamilyIndex@20 queueIndex@24 */
    vkshim_cas_getqueue(dev, (long)*(uint32_t *)((char *)pinfo + 20), pq);
}
void vkshim_cas_device(long phys, long dev) {
    pthread_mutex_lock(&g_mu);
    if (g_ndev < MAXDEV) { g_devs[g_ndev].dev = (VkDevice)dev; g_devs[g_ndev].phys = (VkPhysicalDevice)phys; g_ndev++; }
    pthread_mutex_unlock(&g_mu);
}

static int queue_family(VkQueue q, uint32_t *out) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nq; i++) if (g_queues[i].q == q) { *out = g_queues[i].family; ok = 1; break; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
static VkPhysicalDevice phys_of(VkDevice d) {
    VkPhysicalDevice p = NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_ndev; i++) if (g_devs[i].dev == d) { p = g_devs[i].phys; break; }
    pthread_mutex_unlock(&g_mu);
    return p;
}
/* used by sgsr.c */
int vkshim_q_family(VkQueue q, uint32_t *family) { return queue_family(q, family); }
VkPhysicalDevice vkshim_phys_of(VkDevice d) { return phys_of(d); }
uint32_t vkshim_first_queue_family(int *ok) {
    uint32_t f = 0; *ok = 0;
    pthread_mutex_lock(&g_mu);
    if (g_nq > 0) { f = g_queues[0].family; *ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return f;
}

static Cas *find(VkSwapchainKHR sc) {
    for (int i = 0; i < MAXSC; i++) if (g_cas[i].used && g_cas[i].sc == sc) return &g_cas[i];
    return NULL;
}

/* ---------- teardown (also used for a half-built state) ---------- */
static void cas_teardown(Cas *c) {
    VkDevice d = c->dev;
    if (c->pool) F.DestroyCommandPool(d, c->pool, NULL);           /* frees its command buffers */
    for (uint32_t i = 0; i < MAXIMG; i++) {
        if (c->sems[i]) F.DestroySemaphore(d, c->sems[i], NULL);
        if (c->fbs[i]) F.DestroyFramebuffer(d, c->fbs[i], NULL);
        if (c->views[i]) F.DestroyImageView(d, c->views[i], NULL);
    }
    if (c->pipe) F.DestroyPipeline(d, c->pipe, NULL);
    if (c->dp) F.DestroyDescriptorPool(d, c->dp, NULL);
    if (c->pl) F.DestroyPipelineLayout(d, c->pl, NULL);
    if (c->dsl) F.DestroyDescriptorSetLayout(d, c->dsl, NULL);
    if (c->rp) F.DestroyRenderPass(d, c->rp, NULL);
    if (c->vs) F.DestroyShaderModule(d, c->vs, NULL);
    if (c->fs) F.DestroyShaderModule(d, c->fs, NULL);
    if (c->samp) F.DestroySampler(d, c->samp, NULL);
    if (c->pview) F.DestroyImageView(d, c->pview, NULL);
    if (c->priv) F.DestroyImage(d, c->priv, NULL);
    if (c->pmem) F.FreeMemory(d, c->pmem, NULL);
    VkSwapchainKHR sc = c->sc; VkDevice dev = c->dev; VkFormat fmt = c->fmt; VkExtent2D ext = c->ext; int failed = c->failed;
    memset(c, 0, sizeof *c);
    c->used = 1; c->failed = failed; c->sc = sc; c->dev = dev; c->fmt = fmt; c->ext = ext;
}

#define STEP(what, call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    LOGI("cas: off for this swapchain, %s failed (%d)", what, (int)r_); c->failed = 1; cas_teardown(c); return; } } while (0)

static uint32_t pick_memtype(VkDevice dev, uint32_t bits) {
    VkPhysicalDevice phys = phys_of(dev);
    if (phys) {
        VkPhysicalDeviceMemoryProperties mp;
        F.GetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    }
    for (uint32_t i = 0; i < 32; i++) if (bits & (1u << i)) return i;
    return 0;
}

static void cas_setup_impl(Cas *c, VkQueue queue) {
    VkDevice d = c->dev;
    uint32_t family;
    if (!queue_family(queue, &family)) { LOGI("cas: off, the present queue's family is unknown"); c->failed = 1; return; }

    uint32_t n = 0;
    STEP("get swapchain images (count)", F.GetSwapchainImagesKHR(d, c->sc, &n, NULL));
    if (n == 0 || n > MAXIMG) { LOGI("cas: off, %u swapchain images", n); c->failed = 1; return; }
    c->n = n;
    STEP("get swapchain images", F.GetSwapchainImagesKHR(d, c->sc, &n, c->imgs));

    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = c->fmt,
        .extent = { c->ext.width, c->ext.height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    STEP("create image", F.CreateImage(d, &ici, NULL, &c->priv));
    VkMemoryRequirements mr;
    F.GetImageMemoryRequirements(d, c->priv, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = pick_memtype(d, mr.memoryTypeBits) };
    STEP("allocate memory", F.AllocateMemory(d, &mai, NULL, &c->pmem));
    STEP("bind memory", F.BindImageMemory(d, c->priv, c->pmem, 0));

    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = c->fmt, .components = { 0, 0, 0, 0 }, .subresourceRange = rng };
    vci.image = c->priv;
    STEP("create private view", F.CreateImageView(d, &vci, NULL, &c->pview));
    for (uint32_t i = 0; i < n; i++) {
        vci.image = c->imgs[i];
        STEP("create swapchain view", F.CreateImageView(d, &vci, NULL, &c->views[i]));
    }

    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.25f, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    STEP("create sampler", F.CreateSampler(d, &sci, NULL, &c->samp));

    VkAttachmentDescription ad = { .format = c->fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
        .subpassCount = 1, .pSubpasses = &sp };
    STEP("create render pass", F.CreateRenderPass(d, &rpi, NULL, &c->rp));
    for (uint32_t i = 0; i < n; i++) {
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = c->rp, .attachmentCount = 1,
            .pAttachments = &c->views[i], .width = c->ext.width, .height = c->ext.height, .layers = 1 };
        STEP("create framebuffer", F.CreateFramebuffer(d, &fi, NULL, &c->fbs[i]));
    }

    VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
    STEP("create descriptor set layout", F.CreateDescriptorSetLayout(d, &dli, NULL, &c->dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &c->dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    STEP("create pipeline layout", F.CreatePipelineLayout(d, &pli, NULL, &c->pl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    STEP("create descriptor pool", F.CreateDescriptorPool(d, &dpi, NULL, &c->dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp,
        .descriptorSetCount = 1, .pSetLayouts = &c->dsl };
    STEP("allocate descriptor set", F.AllocateDescriptorSets(d, &dai, &c->ds));
    VkDescriptorImageInfo dii = { c->samp, c->pview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = sizeof cas_vs_spv; smi.pCode = cas_vs_spv;
    STEP("create vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->vs));
    smi.codeSize = sizeof cas_fs_spv; smi.pCode = cas_fs_spv;
    STEP("create fragment shader", F.CreateShaderModule(d, &smi, NULL, &c->fs));

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = c->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = c->fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)c->ext.width, (float)c->ext.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, c->ext };
    VkPipelineViewportStateCreateInfo vs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
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
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = c->pl, .renderPass = c->rp, .subpass = 0,
        .basePipelineIndex = -1 };
    STEP("create pipeline", F.CreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, NULL, &c->pipe));

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    STEP("create command pool", F.CreateCommandPool(d, &cpi, NULL, &c->pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = n };
    STEP("allocate command buffers", F.AllocateCommandBuffers(d, &cai, c->cmds));
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++) STEP("create semaphore", F.CreateSemaphore(d, &semi, NULL, &c->sems[i]));

    float sharp = cas_strength();
    for (uint32_t i = 0; i < n; i++) {
        VkCommandBuffer cb2 = c->cmds[i];
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        STEP("begin command buffer", F.BeginCommandBuffer(cb2, &bi));

        /* 1. swapchain image: PRESENT_SRC -> TRANSFER_SRC; private image: discard -> TRANSFER_DST */
        VkImageMemoryBarrier b1[2] = {
            { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->imgs[i], .subresourceRange = rng },
            { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->priv, .subresourceRange = rng } };
        F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b1);

        VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { c->ext.width, c->ext.height, 1 } };
        F.CmdCopyImage(cb2, c->imgs[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->priv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);

        /* 2. private image -> shader read; swapchain image -> colour attachment */
        VkImageMemoryBarrier b2[2] = {
            { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->priv, .subresourceRange = rng },
            { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->imgs[i], .subresourceRange = rng } };
        F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, NULL, 0, NULL, 2, b2);

        /* 3. CAS: private image -> swapchain image */
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rp, .framebuffer = c->fbs[i],
            .renderArea = { { 0, 0 }, c->ext } };
        F.CmdBeginRenderPass(cb2, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
        F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1, &c->ds, 0, NULL);
        F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &sharp);
        F.CmdDraw(cb2, 3, 1, 0, 0);
        F.CmdEndRenderPass(cb2);
        STEP("end command buffer", F.EndCommandBuffer(cb2));
    }
    c->ready = 1;
    LOGI("cas: ready: %u images, format %d, %ux%u, strength %.2f", n, (int)c->fmt, c->ext.width, c->ext.height, sharp);
}

/* The game's present thread has a small stack and creating the pipeline runs the driver's shader compiler,
 * which needs a lot of it (the first test crashed there with a stack overflow). So setup runs on its own thread. */
struct setup_arg { Cas *c; VkQueue q; };
static void *setup_thread(void *p) { struct setup_arg *a = p; cas_setup_impl(a->c, a->q); return NULL; }
static void cas_setup(Cas *c, VkQueue queue) {
    struct setup_arg a = { c, queue };
    pthread_attr_t at; pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, setup_thread, &a) != 0) {
        LOGI("cas: off, couldn't start the setup thread");
        c->failed = 1; pthread_attr_destroy(&at); return;
    }
    pthread_join(t, NULL);
    pthread_attr_destroy(&at);
}

/* ---------- custom resolution: what the game is told the surface size is ---------- */
typedef long (*cas_f3)(long, long, long);
static void res_apply(VkSurfaceCapabilitiesKHR *c) {
    const char *sw = getenv("LAS_RES_W"), *sh = getenv("LAS_RES_H");
    int w = sw ? atoi(sw) : 0, h = sh ? atoi(sh) : 0;
    if (w <= 0 || h <= 0 || c->currentExtent.width == 0xFFFFFFFFu) return;
    static int logged = 0;
    if (!logged) { logged = 1; LOGI("resolution: surface size %ux%u reported to the game as %dx%d", c->currentExtent.width, c->currentExtent.height, w, h); }
    c->currentExtent.width = (uint32_t)w; c->currentExtent.height = (uint32_t)h;
    if (c->minImageExtent.width > (uint32_t)w) c->minImageExtent.width = (uint32_t)w;
    if (c->minImageExtent.height > (uint32_t)h) c->minImageExtent.height = (uint32_t)h;
    if (c->maxImageExtent.width < (uint32_t)w) c->maxImageExtent.width = (uint32_t)w;
    if (c->maxImageExtent.height < (uint32_t)h) c->maxImageExtent.height = (uint32_t)h;
}
long vkshim_res_caps(long phys, long surf, long pcaps, cas_f3 real) {
    long r = real(phys, surf, pcaps);
    if (r == VK_SUCCESS && pcaps) res_apply((VkSurfaceCapabilitiesKHR *)pcaps);
    return r;
}
long vkshim_res_caps2(long phys, long pinfo, long pcaps, cas_f3 real) {
    long r = real(phys, pinfo, pcaps);
    if (r == VK_SUCCESS && pcaps) res_apply(&((VkSurfaceCapabilities2KHR *)pcaps)->surfaceCapabilities);
    return r;
}

/* ---------- entry points called from the generated shim ---------- */
long vkshim_cas_create(long device, long pci, long palloc, long psc, cas_f8 real) {
    { long sr; if (vkshim_sgsr_create(device, pci, palloc, psc, real, &sr)) return sr; }
    if (cas_strength() <= 0.0f || !pci || !psc || !cas_fn_init())
        return real(device, pci, palloc, psc, 0, 0, 0, 0);
    VkSwapchainCreateInfoKHR ci = *(const VkSwapchainCreateInfoKHR *)pci;
    int slot = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAXSC; i++) if (!g_cas[i].used) { slot = i; break; }
    pthread_mutex_unlock(&g_mu);
    if (slot < 0 || ci.imageArrayLayers != 1 || (ci.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR))
        return real(device, pci, palloc, psc, 0, 0, 0, 0);
    /* the copy needs to read the images and the CAS pass draws into them */
    ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    long r = real(device, (long)&ci, palloc, psc, 0, 0, 0, 0);
    if (r != VK_SUCCESS) {
        LOGI("cas: swapchain with extra usage flags failed (%d), creating it untouched", (int)r);
        return real(device, pci, palloc, psc, 0, 0, 0, 0);
    }
    Cas *c = &g_cas[slot];
    memset(c, 0, sizeof *c);
    c->used = 1; c->sc = *(VkSwapchainKHR *)psc; c->dev = (VkDevice)device; c->fmt = ci.imageFormat; c->ext = ci.imageExtent;
    LOGI("cas: swapchain created: format %d, %ux%u, min images %u (setup happens at the first present)",
         (int)ci.imageFormat, ci.imageExtent.width, ci.imageExtent.height, ci.minImageCount);
    return r;
}

long vkshim_cas_present(long queue, long ppi, cas_f2 real) {
    { long sr; if (vkshim_sgsr_present(queue, ppi, real, &sr)) return sr; }
    const VkPresentInfoKHR *pi = (const VkPresentInfoKHR *)ppi;
    if (pi && pi->swapchainCount == 1 && pi->waitSemaphoreCount <= 8 && cas_fn_init()) {
        Cas *c = find(pi->pSwapchains[0]);
        if (c && !c->failed) {
            if (!c->ready) cas_setup(c, (VkQueue)queue);
            uint32_t idx = pi->pImageIndices[0];
            if (c->ready && idx < c->n) {
                VkPipelineStageFlags stages[8];
                for (int i = 0; i < 8; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = pi->waitSemaphoreCount,
                    .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = stages, .commandBufferCount = 1,
                    .pCommandBuffers = &c->cmds[idx], .signalSemaphoreCount = 1, .pSignalSemaphores = &c->sems[idx] };
                VkResult sr = F.QueueSubmit((VkQueue)queue, 1, &si, VK_NULL_HANDLE);
                if (sr == VK_SUCCESS) {
                    VkPresentInfoKHR p2 = *pi;
                    p2.waitSemaphoreCount = 1;
                    p2.pWaitSemaphores = &c->sems[idx];
                    unsigned long k = ++c->presents;
                    if (k == 1 || k == 2 || k == 300) LOGI("cas: frame %lu sharpened (image %u)", k, idx);
                    return real(queue, (long)&p2);
                }
                LOGI("cas: off, the extra submit failed (%d)", (int)sr);
                c->failed = 1;      /* resources stay allocated until the swapchain is destroyed */
            }
        }
    }
    return real(queue, ppi);
}

void vkshim_cas_destroy(long device, long swapchain, long palloc, cas_f4v real) {
    if (vkshim_sgsr_destroy(device, swapchain, palloc, real)) return;
    Cas *c = swapchain ? find((VkSwapchainKHR)swapchain) : NULL;
    if (c) {
        if (g_fn_state > 0) {
            if (c->ready || c->failed) { F.DeviceWaitIdle((VkDevice)device); cas_teardown(c); }
        }
        pthread_mutex_lock(&g_mu);
        memset(c, 0, sizeof *c);
        pthread_mutex_unlock(&g_mu);
    }
    real(device, swapchain, palloc, 0);
}
