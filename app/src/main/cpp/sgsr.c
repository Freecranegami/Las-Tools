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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "sgsr_spv.h"
#include "fxaa_spv.h"
#include "fsr_spv.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GuestHost", __VA_ARGS__)
extern void *vkshim_real_proc(const char *name);
extern int vkshim_q_family(VkQueue q, uint32_t *family);
extern VkPhysicalDevice vkshim_phys_of(VkDevice d);
extern uint32_t vkshim_first_queue_family(int *ok);
typedef long (*sg_f8)(long, long, long, long, long, long, long, long);
typedef long (*sg_f2)(long, long);
typedef long (*sg_f4)(long, long, long, long);
typedef void (*sg_f4v)(long, long, long, long);

#define MAXIMG 8
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
    LD(GetCaps, "GetPhysicalDeviceSurfaceCapabilitiesKHR"); LD(GetMemProps, "GetPhysicalDeviceMemoryProperties");
#undef LD
    if (missing) LOGI("sgsr: off, the driver has no %s", missing);
    g_fn_state = missing ? -1 : 1;
    return g_fn_state > 0;
}

typedef struct {
    int used, failed;
    VkSwapchainKHR sc; VkDevice dev; VkFormat fmt; VkExtent2D in, out; uint32_t n, family;
    VkImage rimg[MAXIMG], pimg[MAXIMG]; VkDeviceMemory pmem[MAXIMG];
    VkImageView pview[MAXIMG], rview[MAXIMG]; VkFramebuffer fb[MAXIMG];
    VkDescriptorSet ds[MAXIMG]; VkCommandBuffer cmd[MAXIMG]; VkSemaphore sem[MAXIMG];
    VkSampler samp; VkRenderPass rp; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
    VkDescriptorPool dp; VkCommandPool pool; VkShaderModule vs, fs;
    /* FXAA before the upscale: game image -> FXAA -> intermediate image (game size) -> SGSR */
    int fx; VkImage qimg[MAXIMG]; VkDeviceMemory qmem[MAXIMG]; VkImageView qview[MAXIMG]; VkFramebuffer qfb[MAXIMG];
    VkDescriptorSet dsq[MAXIMG]; VkRenderPass rpm; VkPipeline pipefx; VkShaderModule fvs, ffs;
    /* FSR 1 (mode 2): game image [-> FXAA] -> EASU -> full-size image -> RCAS -> swapchain image */
    int mode; float rcas; VkImage uimg[MAXIMG]; VkDeviceMemory umem[MAXIMG]; VkImageView uview[MAXIMG]; VkFramebuffer ufb[MAXIMG];
    VkDescriptorSet dsu[MAXIMG]; VkPipeline pipee; VkShaderModule efs;
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
    if (s < 0) { const char *e = getenv("LAS_SGSR"), *f = getenv("LAS_FSR"); s = ((e && *e == '1') || (f && *f == '1')) ? 1 : 0; }
    return s;
}

/* frees everything except the real swapchain itself */
static void teardown(Sg *c) {
    VkDevice d = c->dev;
    if (c->pool) F.DestroyCommandPool(d, c->pool, NULL);           /* frees its command buffers */
    for (int i = 0; i < MAXIMG; i++) {
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

static VkResult mk_pipe(Sg *c, VkShaderModule vs, VkShaderModule fs, VkRenderPass rp, VkExtent2D ext, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)ext.width, (float)ext.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, ext };
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

/* proxy images, views, pipeline and one pre-recorded command buffer per image; returns 0 on success */
static int build(Sg *c, VkImageUsageFlags gameUsage) {
    VkDevice d = c->dev;
    uint32_t n = c->n;
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (uint32_t i = 0; i < n; i++) {
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = c->fmt,
            .extent = { c->in.width, c->in.height, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = gameUsage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        ST("create proxy image", F.CreateImage(d, &ici, NULL, &c->pimg[i]));
        VkMemoryRequirements mr;
        F.GetImageMemoryRequirements(d, c->pimg[i], &mr);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = pick_memtype(d, mr.memoryTypeBits) };
        ST("allocate proxy memory", F.AllocateMemory(d, &mai, NULL, &c->pmem[i]));
        ST("bind proxy memory", F.BindImageMemory(d, c->pimg[i], c->pmem[i], 0));
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = c->fmt, .components = { 0, 0, 0, 0 }, .subresourceRange = rng };
        vci.image = c->pimg[i];
        ST("create proxy view", F.CreateImageView(d, &vci, NULL, &c->pview[i]));
        vci.image = c->rimg[i];
        ST("create swapchain view", F.CreateImageView(d, &vci, NULL, &c->rview[i]));
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
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
        for (uint32_t i = 0; i < n; i++) {
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
    uint32_t nsets = n * (1 + (c->fx ? 1 : 0) + (c->mode == 2 ? 1 : 0));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nsets };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = nsets, .poolSizeCount = 1, .pPoolSizes = &ps };
    ST("create descriptor pool", F.CreateDescriptorPool(d, &dpi, NULL, &c->dp));
    VkDescriptorSetLayout lays[3 * MAXIMG];
    VkDescriptorSet sets[3 * MAXIMG];
    for (uint32_t i = 0; i < nsets; i++) lays[i] = c->dsl;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = c->dp,
        .descriptorSetCount = nsets, .pSetLayouts = lays };
    ST("allocate descriptor sets", F.AllocateDescriptorSets(d, &dai, sets));
    for (uint32_t i = 0; i < n; i++) {
        c->ds[i] = sets[i];                                   /* game image */
        VkDescriptorImageInfo dii = { c->samp, c->pview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds[i], .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
        F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);
        if (c->fx) {
            c->dsq[i] = sets[n + i];                          /* FXAA output */
            VkDescriptorImageInfo dq = { c->samp, c->qview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wds.dstSet = c->dsq[i]; wds.pImageInfo = &dq;
            F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);
        }
        if (c->mode == 2) {
            c->dsu[i] = sets[(c->fx ? 2 * n : n) + i];        /* EASU output */
            VkDescriptorImageInfo du = { c->samp, c->uview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wds.dstSet = c->dsu[i]; wds.pImageInfo = &du;
            F.UpdateDescriptorSets(d, 1, &wds, 0, NULL);
        }
    }

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    if (c->mode == 2) {
        smi.codeSize = sizeof fsr_vs_spv; smi.pCode = fsr_vs_spv;
        ST("create vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->vs));
        smi.codeSize = sizeof fsr_rcas_fs_spv; smi.pCode = fsr_rcas_fs_spv;
        ST("create RCAS shader", F.CreateShaderModule(d, &smi, NULL, &c->fs));
        smi.codeSize = sizeof fsr_easu_fs_spv; smi.pCode = fsr_easu_fs_spv;
        ST("create EASU shader", F.CreateShaderModule(d, &smi, NULL, &c->efs));
        ST("create RCAS pipeline", mk_pipe(c, c->vs, c->fs, c->rp, c->out, &c->pipe));
        ST("create EASU pipeline", mk_pipe(c, c->vs, c->efs, c->rpm, c->out, &c->pipee));
    } else {
        smi.codeSize = sizeof sgsr_vs_spv; smi.pCode = sgsr_vs_spv;
        ST("create vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->vs));
        smi.codeSize = sizeof sgsr_fs_spv; smi.pCode = sgsr_fs_spv;
        ST("create fragment shader", F.CreateShaderModule(d, &smi, NULL, &c->fs));
        ST("create pipeline", mk_pipe(c, c->vs, c->fs, c->rp, c->out, &c->pipe));
    }
    if (c->fx) {
        smi.codeSize = sizeof fxaa_vs_spv; smi.pCode = fxaa_vs_spv;
        ST("create FXAA vertex shader", F.CreateShaderModule(d, &smi, NULL, &c->fvs));
        smi.codeSize = sizeof fxaa_fs_spv; smi.pCode = fxaa_fs_spv;
        ST("create FXAA fragment shader", F.CreateShaderModule(d, &smi, NULL, &c->ffs));
        ST("create FXAA pipeline", mk_pipe(c, c->fvs, c->ffs, c->rpm, c->in, &c->pipefx));
    }

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->family };
    ST("create command pool", F.CreateCommandPool(d, &cpi, NULL, &c->pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = n };
    ST("allocate command buffers", F.AllocateCommandBuffers(d, &cai, c->cmd));
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++) ST("create semaphore", F.CreateSemaphore(d, &semi, NULL, &c->sem[i]));

    float vinfo[4] = { 1.0f / (float)c->in.width, 1.0f / (float)c->in.height, (float)c->in.width, (float)c->in.height };
    /* FSR 1 constants, the same as FsrEasuCon / FsrRcasCon in ffx_fsr1.h (input size = viewport size here) */
    uint32_t easu[16], rcas[4] = { 0, 0, 0, 0 };
    {
        float iw = (float)c->in.width, ih = (float)c->in.height, ow = (float)c->out.width, oh = (float)c->out.height;
        float e[16] = { iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f,
                        1.0f / iw, 1.0f / ih, 1.0f / iw, -1.0f / ih,
                        -1.0f / iw, 2.0f / ih, 1.0f / iw, 2.0f / ih,
                        0.0f, 4.0f / ih, 0.0f, 0.0f };
        memcpy(easu, e, sizeof easu);                          /* the shader reads the raw bits of these floats */
        float sh = exp2f(-c->rcas);
        memcpy(&rcas[0], &sh, 4);
    }
    for (uint32_t i = 0; i < n; i++) {
        VkCommandBuffer cb2 = c->cmd[i];
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        ST("begin command buffer", F.BeginCommandBuffer(cb2, &bi));
        /* the game leaves its image in PRESENT_SRC; make it readable by the shader */
        VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->pimg[i], .subresourceRange = rng };
        F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);

        if (c->fx) {                                           /* (mode 2 below reads this pass's output) */
            VkRenderPassBeginInfo rbq = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rpm, .framebuffer = c->qfb[i],
                .renderArea = { { 0, 0 }, c->in } };
            F.CmdBeginRenderPass(cb2, &rbq, VK_SUBPASS_CONTENTS_INLINE);
            F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipefx);
            F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1, &c->ds[i], 0, NULL);
            F.CmdDraw(cb2, 3, 1, 0, 0);
            F.CmdEndRenderPass(cb2);
        }
        if (c->mode == 2) {                                    /* EASU: game (or FXAA) image -> full-size image */
            VkRenderPassBeginInfo rbu = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rpm, .framebuffer = c->ufb[i],
                .renderArea = { { 0, 0 }, c->out } };
            F.CmdBeginRenderPass(cb2, &rbu, VK_SUBPASS_CONTENTS_INLINE);
            F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipee);
            F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1, c->fx ? &c->dsq[i] : &c->ds[i], 0, NULL);
            F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, easu);
            F.CmdDraw(cb2, 3, 1, 0, 0);
            F.CmdEndRenderPass(cb2);
        }
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = c->rp, .framebuffer = c->fb[i],
            .renderArea = { { 0, 0 }, c->out } };
        F.CmdBeginRenderPass(cb2, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        F.CmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipe);
        F.CmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pl, 0, 1,
                                c->mode == 2 ? &c->dsu[i] : (c->fx ? &c->dsq[i] : &c->ds[i]), 0, NULL);
        if (c->mode == 2) F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rcas);   /* RCAS: sharpen at full size */
        else F.CmdPushConstants(cb2, c->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, vinfo);
        F.CmdDraw(cb2, 3, 1, 0, 0);
        F.CmdEndRenderPass(cb2);

        /* hand the image back in the layout the game expects */
        VkImageMemoryBarrier b2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = c->pimg[i], .subresourceRange = rng };
        F.CmdPipelineBarrier(cb2, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        ST("end command buffer", F.EndCommandBuffer(cb2));
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
    if (!why && (caps.currentExtent.width == 0xFFFFFFFFu || caps.currentExtent.width <= ci.imageExtent.width ||
                 caps.currentExtent.height <= ci.imageExtent.height)) why = "the game's swapchain is not smaller than the screen";
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
      c->mode = (ff && *ff == '1') ? 2 : 1;
      c->rcas = (rs && *rs) ? (float)atof(rs) : 0.2f;
      if (c->rcas < 0.0f) c->rcas = 0.0f;
      if (c->rcas > 2.0f) c->rcas = 2.0f; }
    c->used = 1; c->dev = dev; c->fmt = ci.imageFormat; c->in = ci.imageExtent; c->out = caps.currentExtent; c->family = family;

    VkSwapchainCreateInfoKHR rci = ci;
    rci.imageExtent = caps.currentExtent;
    rci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
    LOGI("sgsr: on (%s): game renders %ux%u, screen %ux%u, %u images, format %d%s", c->mode == 2 ? "FSR 1 EASU+RCAS" : "SGSR 1",
         c->in.width, c->in.height, c->out.width, c->out.height, n, (int)c->fmt, c->fx ? ", FXAA before the upscale" : "");
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
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = pi->waitSemaphoreCount,
            .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = stages, .commandBufferCount = 1,
            .pCommandBuffers = &c->cmd[idx], .signalSemaphoreCount = 1, .pSignalSemaphores = &c->sem[idx] };
        VkResult sr = F.QueueSubmit((VkQueue)queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_SUCCESS) {
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
