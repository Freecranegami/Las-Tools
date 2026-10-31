// EpiShaders: runs ReShade effects (.fx files, compiled with ReShade's own open source compiler "reshadefx") as extra
// passes on the finished picture. Two users:
//   - the app (JNI): describe(root, file) lists an effect's settings so the UI can show them;
//   - the Vulkan shim (sgsr.c): epifx_create / epifx_upload / epifx_record / epifx_tick / epifx_destroy.
// Only effects that need no depth buffer are supported (a depth sampler gets a 1x1 "far" texture).
#include <android/log.h>
#include <jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "reshadefx/effect_codegen.hpp"
#include "reshadefx/effect_parser.hpp"
#include "reshadefx/effect_preprocessor.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#include "stb_image.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GuestHost", __VA_ARGS__)
#define API extern "C" __attribute__((visibility("default")))

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// compiling
// ---------------------------------------------------------------------------------------------------------------------

struct Loaded {
    std::string file;
    reshadefx::effect_module mod;
    std::map<std::string, std::string> spv;   // entry point -> SPIR-V binary
    std::string log;
};

bool compileEffect(const std::string &root, const std::string &file, uint32_t w, uint32_t h, bool wantCode, Loaded &out) {
    out.file = file;
    reshadefx::preprocessor pp;
    pp.add_macro_definition("__RESHADE__", "60000");
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", "20803");
    pp.add_macro_definition("__DEVICE__", "0");
    pp.add_macro_definition("__RENDERER__", "0x20000");
    pp.add_macro_definition("__APPLICATION__", "0");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(w));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(h));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", "0");
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
    pp.add_include_path(root + "/Shaders");
    pp.add_include_path(root + "/Textures");
    pp.append_string(
        "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
        "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
        "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
        "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
        "#define tex2Dgather0 tex2DgatherR\n#define tex2Dgather1 tex2DgatherG\n#define tex2Dgather2 tex2DgatherB\n#define tex2Dgather3 tex2DgatherA\n");
    bool ok = pp.append_file(root + "/Shaders/" + file);
    out.log += pp.errors();
    if (!ok) return false;
    std::unique_ptr<reshadefx::codegen> cg(reshadefx::create_codegen_spirv(true, false, false, false, true /* flip y: D3D style texcoords */));
    reshadefx::parser ps;
    ok = ps.parse(pp.output(), cg.get());
    out.log += ps.errors();
    if (!ok) return false;
    out.mod = cg->module();
    if (wantCode) {
        for (auto &e : out.mod.entry_points) {
            std::string bin, asmb, err;
            if (!cg->assemble_code_for_entry_point(e.first, bin, asmb, err)) { out.log += "assemble " + e.first + ": " + err + "\n"; return false; }
            out.spv[e.first] = std::move(bin);
        }
    }
    return true;
}

const reshadefx::annotation *ann(const std::vector<reshadefx::annotation> &a, const char *name) {
    for (auto &x : a) if (x.name == name) return &x;
    return nullptr;
}

std::string annStr(const std::vector<reshadefx::annotation> &a, const char *name) {
    auto *x = ann(a, name);
    return x ? x->value.string_data : std::string();
}

bool annNum(const std::vector<reshadefx::annotation> &a, const char *name, double &v) {
    auto *x = ann(a, name);
    if (!x) return false;
    switch (x->type.base) {
        case reshadefx::type::t_float: v = x->value.as_float[0]; return true;
        case reshadefx::type::t_int: v = x->value.as_int[0]; return true;
        case reshadefx::type::t_uint: case reshadefx::type::t_bool: v = x->value.as_uint[0]; return true;
        default: return false;
    }
}

std::string jesc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}

std::string num(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.9g", v);
    return b;
}

const char *baseName(const reshadefx::type &t) {
    switch (t.base) {
        case reshadefx::type::t_bool: return "bool";
        case reshadefx::type::t_int: case reshadefx::type::t_min16int: return "int";
        case reshadefx::type::t_uint: case reshadefx::type::t_min16uint: return "uint";
        default: return "float";
    }
}

// the effect's settings as JSON, for the app's UI
std::string describe(const std::string &root, const std::string &file) {
    Loaded L;
    bool ok = false;
    try { ok = compileEffect(root, file, 2400, 1080, false, L); } catch (...) { L.log += "exception\n"; }
    std::string j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",\"log\":\"" + jesc(L.log.substr(0, 600)) + "\",\"uniforms\":[";
    if (ok) {
        bool first = true;
        for (auto &u : L.mod.uniforms) {
            if (u.type.is_matrix() || u.type.is_array() || u.type.is_struct()) continue;
            if (ann(u.annotations, "source")) continue;                       // fed by the runtime (time, frame count, ...)
            std::string ui = annStr(u.annotations, "ui_type");
            if (ui == "none") continue;
            if (!first) j += ",";
            first = false;
            j += "{\"n\":\"" + jesc(u.name) + "\",\"t\":\"" + baseName(u.type) + "\",\"r\":" + std::to_string(u.type.rows);
            if (ui.empty()) ui = u.type.is_boolean() ? "checkbox" : "drag";
            j += ",\"ui\":\"" + jesc(ui) + "\"";
            double v;
            bool mn = annNum(u.annotations, "ui_min", v) || annNum(u.annotations, "min", v);
            if (mn) j += ",\"min\":" + num(v);
            if (annNum(u.annotations, "ui_max", v) || annNum(u.annotations, "max", v)) j += ",\"max\":" + num(v);
            if (annNum(u.annotations, "ui_step", v) || annNum(u.annotations, "step", v)) j += ",\"step\":" + num(v);
            std::string label = annStr(u.annotations, "ui_label");
            j += ",\"label\":\"" + jesc(label.empty() ? u.name : label) + "\"";
            j += ",\"tip\":\"" + jesc(annStr(u.annotations, "ui_tooltip")) + "\"";
            j += ",\"cat\":\"" + jesc(annStr(u.annotations, "ui_category")) + "\"";
            std::string items = annStr(u.annotations, "ui_items");
            if (!items.empty()) {
                j += ",\"items\":[";
                size_t p = 0; bool f2 = true;
                while (p < items.size()) {
                    size_t e = items.find('\0', p);
                    if (e == std::string::npos) e = items.size();
                    if (e > p) { if (!f2) j += ","; f2 = false; j += "\"" + jesc(items.substr(p, e - p)) + "\""; }
                    p = e + 1;
                }
                j += "]";
            }
            j += ",\"def\":[";
            unsigned n = std::max(1u, (unsigned)(u.type.rows * u.type.cols));
            for (unsigned i = 0; i < n && i < 4; i++) {
                if (i) j += ",";
                double d = 0;
                if (u.has_initializer_value) {
                    if (u.type.base == reshadefx::type::t_float || u.type.base == reshadefx::type::t_min16float) d = u.initializer_value.as_float[i];
                    else if (u.type.base == reshadefx::type::t_int || u.type.base == reshadefx::type::t_min16int) d = u.initializer_value.as_int[i];
                    else d = u.initializer_value.as_uint[i];
                }
                j += num(d);
            }
            j += "]}";
        }
    }
    j += "]}";
    return j;
}

// ---------------------------------------------------------------------------------------------------------------------
// Vulkan access
// ---------------------------------------------------------------------------------------------------------------------

#define VKFUNCS(X) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines) \
    X(DestroyPipeline) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDraw) \
    X(CmdPipelineBarrier) X(CmdBlitImage) X(CmdCopyBufferToImage) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(QueueSubmit) X(CreateFence) X(DestroyFence) \
    X(WaitForFences)

struct Fn {
#define X(n) PFN_vk##n n = nullptr;
    VKFUNCS(X)
#undef X
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemProps = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetFormatProps = nullptr;
};

VkFormat texFormat(reshadefx::texture_format f) {
    using T = reshadefx::texture_format;
    switch (f) {
        case T::r8: return VK_FORMAT_R8_UNORM;
        case T::r16f: return VK_FORMAT_R16_SFLOAT;
        case T::r16: return VK_FORMAT_R16_UNORM;
        case T::r32f: return VK_FORMAT_R32_SFLOAT;
        case T::rg8: return VK_FORMAT_R8G8_UNORM;
        case T::rg16f: return VK_FORMAT_R16G16_SFLOAT;
        case T::rg16: return VK_FORMAT_R16G16_UNORM;
        case T::rg32f: return VK_FORMAT_R32G32_SFLOAT;
        case T::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case T::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
        case T::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case T::rgb10a2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case T::rg11b10f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkFormat srgbOf(VkFormat f) {
    if (f == VK_FORMAT_R8G8B8A8_UNORM) return VK_FORMAT_R8G8B8A8_SRGB;
    if (f == VK_FORMAT_B8G8R8A8_UNORM) return VK_FORMAT_B8G8R8A8_SRGB;
    return VK_FORMAT_UNDEFINED;
}

VkBlendFactor bf(reshadefx::blend_factor f) {
    using B = reshadefx::blend_factor;
    switch (f) {
        case B::zero: return VK_BLEND_FACTOR_ZERO;
        case B::one: return VK_BLEND_FACTOR_ONE;
        case B::source_color: return VK_BLEND_FACTOR_SRC_COLOR;
        case B::one_minus_source_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case B::dest_color: return VK_BLEND_FACTOR_DST_COLOR;
        case B::one_minus_dest_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case B::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case B::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case B::dest_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case B::one_minus_dest_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp bo(reshadefx::blend_op o) {
    using B = reshadefx::blend_op;
    switch (o) {
        case B::add: return VK_BLEND_OP_ADD;
        case B::subtract: return VK_BLEND_OP_SUBTRACT;
        case B::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case B::min: return VK_BLEND_OP_MIN;
        case B::max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkSamplerAddressMode am(reshadefx::texture_address_mode m) {
    using A = reshadefx::texture_address_mode;
    switch (m) {
        case A::wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case A::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case A::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// the runtime
// ---------------------------------------------------------------------------------------------------------------------

struct Img {
    VkImage im = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView v = VK_NULL_HANDLE, vs = VK_NULL_HANDLE;     // plain view, sRGB view (when the format has one)
    VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t w = 1, h = 1, levels = 1;
    bool owned = true;
};

struct Tex {
    reshadefx::texture t;
    int kind = 0;                 // 0 plain/render target, 1 = BackBuffer (COLOR), 2 = depth (DEPTH)
    Img img;
    std::vector<uint8_t> pix;     // CPU pixels for textures loaded from an image file
    bool hasPix = false;
};

struct Pass {
    VkRenderPass rp = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl1 = VK_NULL_HANDLE;
    VkDescriptorSet ds1 = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool toBack = false, toOut = false;     // renders into the picture (toOut: the last one, straight into the swapchain image)
    int colorIn = 0;                        // which image holds the picture this pass reads: 0 = A, 1 = B
    int colorOut = 0;                       // for passes that render into the picture (not toOut): 0 = A, 1 = B
    std::vector<int> targets;               // texture indexes
    uint32_t vw = 0, vh = 0, nv = 3;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool clear = false;
    std::vector<int> mipTargets;
    std::string name;
};

struct Effect {
    std::string file;
    Loaded L;
    std::vector<Tex> tex;
    std::vector<VkSampler> samp;
    std::vector<Pass> passes;
    VkBuffer ubo = VK_NULL_HANDLE; VkDeviceMemory uboMem = VK_NULL_HANDLE; uint8_t *map = nullptr; uint32_t uboSize = 16;
    VkDescriptorSetLayout dsl0 = VK_NULL_HANDLE;
    VkDescriptorSet ds0 = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    struct Dyn { uint32_t offset; std::string src; double mn, mx; unsigned comps; };
    std::vector<Dyn> dyn;
    std::vector<uint8_t> uboCpu;
};

struct Preset {
    std::vector<std::string> effects;
    std::map<std::string, std::map<std::string, std::vector<double>>> values;
};

Preset readPreset(const std::string &path) {
    Preset p;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        if (cmd == "effect") { std::string n; is >> n; if (!n.empty()) p.effects.push_back(n); }
        else if (cmd == "set") {
            std::string e, u; is >> e >> u;
            std::vector<double> v; double d;
            while (is >> d) v.push_back(d);
            if (!e.empty() && !u.empty()) p.values[e][u] = v;
        }
    }
    return p;
}

struct Fx {
    Fn f;
    VkDevice dev = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImage imgA = VK_NULL_HANDLE; Img A, B, dummyDepth;
    VkRenderPass finalRp = VK_NULL_HANDLE;
    VkRenderPass rpBack = VK_NULL_HANDLE;                 // render pass used for pipelines of passes that render into the picture
    std::vector<std::unique_ptr<Effect>> fx;
    uint64_t frame = 0;
    std::chrono::steady_clock::time_point t0, last;
    bool uploaded = false;

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return 0xFFFFFFFFu;
    }
    uint32_t memTypeAny(uint32_t bits, VkMemoryPropertyFlags want) const {
        uint32_t t = memType(bits, want);
        if (t != 0xFFFFFFFFu) return t;
        for (uint32_t i = 0; i < 32; i++) if (bits & (1u << i)) return i;
        return 0;
    }
};

bool makeImage(Fx &x, Img &o, VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags usage, bool srgbView) {
    o.fmt = fmt; o.w = w; o.h = h; o.levels = levels;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkFormat sv = srgbView ? srgbOf(fmt) : VK_FORMAT_UNDEFINED;
    if (sv != VK_FORMAT_UNDEFINED) ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = { w, h, 1 }; ci.mipLevels = levels; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (x.f.CreateImage(x.dev, &ci, nullptr, &o.im) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    x.f.GetImageMemoryRequirements(x.dev, o.im, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size; ai.memoryTypeIndex = x.memTypeAny(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (x.f.AllocateMemory(x.dev, &ai, nullptr, &o.mem) != VK_SUCCESS) return false;
    if (x.f.BindImageMemory(x.dev, o.im, o.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o.im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    if (x.f.CreateImageView(x.dev, &vi, nullptr, &o.v) != VK_SUCCESS) return false;
    if (sv != VK_FORMAT_UNDEFINED) {
        vi.format = sv;
        if (x.f.CreateImageView(x.dev, &vi, nullptr, &o.vs) != VK_SUCCESS) return false;
    }
    return true;
}

void freeImage(Fx &x, Img &o) {
    if (!o.owned) { if (o.v) x.f.DestroyImageView(x.dev, o.v, nullptr); if (o.vs) x.f.DestroyImageView(x.dev, o.vs, nullptr); o = Img(); return; }
    if (o.vs) x.f.DestroyImageView(x.dev, o.vs, nullptr);
    if (o.v) x.f.DestroyImageView(x.dev, o.v, nullptr);
    if (o.im) x.f.DestroyImage(x.dev, o.im, nullptr);
    if (o.mem) x.f.FreeMemory(x.dev, o.mem, nullptr);
    o = Img();
}

int findTex(Effect &e, const std::string &name) {
    for (size_t i = 0; i < e.tex.size(); i++) if (e.tex[i].t.unique_name == name) return (int)i;
    for (size_t i = 0; i < e.tex.size(); i++) if (e.tex[i].t.name == name) return (int)i;
    return -1;
}

void putUniform(Effect &e, const reshadefx::uniform &u, const std::vector<double> &vals) {
    unsigned comps = std::max(1u, (unsigned)(u.type.rows * u.type.cols));
    if (u.type.is_matrix() || u.type.is_array() || u.type.is_struct()) return;
    for (unsigned i = 0; i < comps && i < 4; i++) {
        double v = i < vals.size() ? vals[i] : (vals.empty() ? 0.0 : vals.back());
        uint32_t off = u.offset + i * 4;
        if (off + 4 > e.uboCpu.size()) break;
        if (u.type.base == reshadefx::type::t_float || u.type.base == reshadefx::type::t_min16float) {
            float fv = (float)v; memcpy(&e.uboCpu[off], &fv, 4);
        } else if (u.type.base == reshadefx::type::t_bool) {
            uint32_t b = v != 0.0; memcpy(&e.uboCpu[off], &b, 4);
        } else if (u.type.base == reshadefx::type::t_uint || u.type.base == reshadefx::type::t_min16uint) {
            uint32_t b = v < 0 ? 0u : (uint32_t)llround(v); memcpy(&e.uboCpu[off], &b, 4);
        } else {
            int32_t b = (int32_t)llround(v); memcpy(&e.uboCpu[off], &b, 4);
        }
    }
}

std::vector<double> initialOf(const reshadefx::uniform &u) {
    std::vector<double> v;
    unsigned comps = std::max(1u, (unsigned)(u.type.rows * u.type.cols));
    for (unsigned i = 0; i < comps && i < 4; i++) {
        double d = 0;
        if (u.has_initializer_value) {
            if (u.type.base == reshadefx::type::t_float || u.type.base == reshadefx::type::t_min16float) d = u.initializer_value.as_float[i];
            else if (u.type.base == reshadefx::type::t_int || u.type.base == reshadefx::type::t_min16int) d = u.initializer_value.as_int[i];
            else d = u.initializer_value.as_uint[i];
        }
        v.push_back(d);
    }
    return v;
}

std::vector<uint8_t> loadPixels(const std::string &path, uint32_t w, uint32_t h, reshadefx::texture_format fmt) {
    int iw = 0, ih = 0, ic = 0;
    unsigned char *d = stbi_load(path.c_str(), &iw, &ih, &ic, 4);
    unsigned ch = fmt == reshadefx::texture_format::r8 ? 1 : (fmt == reshadefx::texture_format::rg8 ? 2 : 4);
    std::vector<uint8_t> out((size_t)w * h * ch, 0);
    if (!d) return out;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t xx = 0; xx < w; xx++) {
            // bilinear when the picture has another size than the texture
            float fx = w > 1 ? (float)xx * (iw - 1) / (float)(w - 1) : 0, fy = h > 1 ? (float)y * (ih - 1) / (float)(h - 1) : 0;
            int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, iw - 1), y1 = std::min(y0 + 1, ih - 1);
            float ax = fx - x0, ay = fy - y0;
            for (unsigned c = 0; c < ch; c++) {
                float p00 = d[(y0 * iw + x0) * 4 + c], p10 = d[(y0 * iw + x1) * 4 + c], p01 = d[(y1 * iw + x0) * 4 + c], p11 = d[(y1 * iw + x1) * 4 + c];
                float v = (p00 * (1 - ax) + p10 * ax) * (1 - ay) + (p01 * (1 - ax) + p11 * ax) * ay;
                out[((size_t)y * w + xx) * ch + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
    stbi_image_free(d);
    return out;
}

bool buildEffect(Fx &x, const std::string &root, const std::string &file, const Preset &pre, Effect &e, std::string &why) {
    e.file = file;
    std::string base = file.substr(0, file.size() - 3);
    if (!compileEffect(root, file, x.w, x.h, true, e.L)) { why = "compile: " + e.L.log.substr(0, 300); return false; }
    auto &m = e.L.mod;
    if (m.techniques.empty()) { why = "no technique"; return false; }
    for (auto &t : m.techniques) for (auto &p : t.passes) if (!p.cs_entry_point.empty()) { why = "uses compute shaders"; return false; }
    if (!m.storages.empty()) { why = "uses storage images"; return false; }

    // uniforms: defaults, then the user's values
    uint32_t sz = std::max(16u, (m.total_uniform_size + 15u) & ~15u);
    e.uboSize = sz;
    e.uboCpu.assign(sz, 0);
    auto pv = pre.values.find(base);
    for (auto &u : m.uniforms) {
        const reshadefx::annotation *src = ann(u.annotations, "source");
        if (src) {
            Effect::Dyn d; d.offset = u.offset; d.src = src->value.string_data; d.comps = std::max(1u, (unsigned)(u.type.rows * u.type.cols));
            double v;
            d.mn = annNum(u.annotations, "min", v) ? v : 0; d.mx = annNum(u.annotations, "max", v) ? v : 32767;
            e.dyn.push_back(d);
            continue;
        }
        std::vector<double> vals = initialOf(u);
        if (pv != pre.values.end()) { auto it = pv->second.find(u.name); if (it != pv->second.end() && !it->second.empty()) vals = it->second; }
        putUniform(e, u, vals);
    }

    // textures
    for (auto &t : m.textures) {
        Tex tx; tx.t = t;
        if (t.semantic == "COLOR") tx.kind = 1;
        else if (t.semantic == "DEPTH") tx.kind = 2;
        e.tex.push_back(std::move(tx));
    }
    for (auto &tx : e.tex) {
        if (tx.kind != 0) continue;
        VkFormat fm = texFormat(tx.t.format);
        uint32_t lv = std::max<uint32_t>(1, tx.t.levels);
        VkImageUsageFlags us = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!makeImage(x, tx.img, fm, std::max<uint32_t>(1, tx.t.width), std::max<uint32_t>(1, tx.t.height), lv, us, true)) { why = "texture " + tx.t.name; return false; }
        std::string src = annStr(tx.t.annotations, "source");
        if (!src.empty()) {
            reshadefx::texture_format tf = tx.t.format == reshadefx::texture_format::unknown ? reshadefx::texture_format::rgba8 : tx.t.format;
            if (tf == reshadefx::texture_format::rgba8 || tf == reshadefx::texture_format::r8 || tf == reshadefx::texture_format::rg8) {
                tx.pix = loadPixels(root + "/Textures/" + src, tx.img.w, tx.img.h, tf);
                tx.hasPix = true;
            }
        }
    }

    // samplers
    for (auto &s : m.samplers) {
        VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        unsigned fl = (unsigned)s.filter;
        bool minL = fl & 0x10, magL = fl & 0x04, mipL = fl & 0x01;
        if (s.filter == reshadefx::filter_mode::anisotropic) { minL = magL = mipL = true; }
        sc.minFilter = minL ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; sc.magFilter = magL ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sc.mipmapMode = mipL ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU = am(s.address_u); sc.addressModeV = am(s.address_v); sc.addressModeW = am(s.address_w);
        sc.mipLodBias = s.lod_bias; sc.minLod = std::max(0.0f, s.min_lod); sc.maxLod = std::min(s.max_lod, 16.0f);
        sc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler vs = VK_NULL_HANDLE;
        if (x.f.CreateSampler(x.dev, &sc, nullptr, &vs) != VK_SUCCESS) { why = "sampler"; return false; }
        e.samp.push_back(vs);
    }

    // uniform buffer
    {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = sz; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (x.f.CreateBuffer(x.dev, &bi, nullptr, &e.ubo) != VK_SUCCESS) { why = "ubo"; return false; }
        VkMemoryRequirements mr; x.f.GetBufferMemoryRequirements(x.dev, e.ubo, &mr);
        uint32_t mt = x.memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == 0xFFFFFFFFu) { why = "no host memory"; return false; }
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
        if (x.f.AllocateMemory(x.dev, &ai, nullptr, &e.uboMem) != VK_SUCCESS) { why = "ubo memory"; return false; }
        x.f.BindBufferMemory(x.dev, e.ubo, e.uboMem, 0);
        if (x.f.MapMemory(x.dev, e.uboMem, 0, VK_WHOLE_SIZE, 0, (void **)&e.map) != VK_SUCCESS) { why = "map"; return false; }
        memcpy(e.map, e.uboCpu.data(), sz);
    }

    // descriptor set 0 (the uniform buffer) and the pool for everything of this effect
    size_t npass = 0, nsamp = 0;
    for (auto &t : m.techniques) { npass += t.passes.size(); for (auto &p : t.passes) nsamp += p.sampler_bindings.size(); }
    {
        VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; li.bindingCount = 1; li.pBindings = &b0;
        if (x.f.CreateDescriptorSetLayout(x.dev, &li, nullptr, &e.dsl0) != VK_SUCCESS) { why = "dsl0"; return false; }
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)std::max<size_t>(1, nsamp) } };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; pi.maxSets = (uint32_t)(npass + 1); pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        if (x.f.CreateDescriptorPool(x.dev, &pi, nullptr, &e.pool) != VK_SUCCESS) { why = "pool"; return false; }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; ai.descriptorPool = e.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &e.dsl0;
        if (x.f.AllocateDescriptorSets(x.dev, &ai, &e.ds0) != VK_SUCCESS) { why = "ds0"; return false; }
        VkDescriptorBufferInfo bi = { e.ubo, 0, sz };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w.dstSet = e.ds0; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi;
        x.f.UpdateDescriptorSets(x.dev, 1, &w, 0, nullptr);
    }

    // passes
    for (auto &t : m.techniques) {
        for (auto &rp : t.passes) {
            Pass p;
            p.name = t.name + "/" + rp.name;
            if (rp.ps_entry_point.empty() || rp.vs_entry_point.empty()) { why = "pass without shaders"; return false; }
            for (int i = 0; i < 8; i++) {
                if (rp.render_target_names[i].empty()) {
                    if (i == 0) p.toBack = true;
                    break;
                }
                int ti = findTex(e, rp.render_target_names[i]);
                if (ti < 0 || e.tex[ti].kind != 0) { why = "render target " + rp.render_target_names[i]; return false; }
                p.targets.push_back(ti);
            }
            p.clear = rp.clear_render_targets;
            p.topo = rp.topology == reshadefx::primitive_topology::triangle_strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP :
                     rp.topology == reshadefx::primitive_topology::line_list ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST :
                     rp.topology == reshadefx::primitive_topology::line_strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP :
                     rp.topology == reshadefx::primitive_topology::point_list ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            p.nv = rp.num_vertices;
            p.vw = rp.viewport_width; p.vh = rp.viewport_height;
            if (p.toBack) { if (!p.vw) p.vw = x.w; if (!p.vh) p.vh = x.h; }
            else {
                auto &t0 = e.tex[p.targets[0]];
                if (!p.vw) p.vw = t0.img.w;
                if (!p.vh) p.vh = t0.img.h;
                if (rp.generate_mipmaps) for (int ti : p.targets) if (e.tex[ti].img.levels > 1) p.mipTargets.push_back(ti);
            }

            // shaders
            auto mk = [&](const std::string &entry, VkShaderModule &out) -> bool {
                auto it = e.L.spv.find(entry);
                if (it == e.L.spv.end()) return false;
                VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                si.codeSize = it->second.size(); si.pCode = reinterpret_cast<const uint32_t *>(it->second.data());
                return x.f.CreateShaderModule(x.dev, &si, nullptr, &out) == VK_SUCCESS;
            };
            if (!mk(rp.vs_entry_point, p.vs) || !mk(rp.ps_entry_point, p.fs)) { why = "shader module"; return false; }

            // descriptor set 1: the samplers this pass uses
            std::vector<VkDescriptorSetLayoutBinding> lb;
            for (auto &sb : rp.sampler_bindings)
                lb.push_back({ sb.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
            VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; li.bindingCount = (uint32_t)lb.size(); li.pBindings = lb.data();
            if (x.f.CreateDescriptorSetLayout(x.dev, &li, nullptr, &p.dsl1) != VK_SUCCESS) { why = "dsl1"; return false; }
            VkDescriptorSetLayout both[2] = { e.dsl0, p.dsl1 };
            VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 2; pli.pSetLayouts = both;
            if (x.f.CreatePipelineLayout(x.dev, &pli, nullptr, &p.pl) != VK_SUCCESS) { why = "pipeline layout"; return false; }
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dai.descriptorPool = e.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &p.dsl1;
            if (x.f.AllocateDescriptorSets(x.dev, &dai, &p.ds1) != VK_SUCCESS) { why = "ds1"; return false; }

            // attachments
            std::vector<VkFormat> afmt;
            if (p.toBack) afmt.push_back(x.fmt);
            else for (int ti : p.targets) afmt.push_back(e.tex[ti].img.fmt);
            std::vector<VkAttachmentDescription> ad;
            std::vector<VkAttachmentReference> ar;
            bool blend0 = rp.blend_enable[0];
            for (size_t i = 0; i < afmt.size(); i++) {
                VkAttachmentDescription a = {};
                a.format = afmt[i]; a.samples = VK_SAMPLE_COUNT_1_BIT;
                bool blend = rp.blend_enable[i];
                a.loadOp = p.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : (blend ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                a.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ad.push_back(a);
                ar.push_back({ (uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
            }
            (void)blend0;
            VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = (uint32_t)ar.size(); sp.pColorAttachments = ar.data();
            VkSubpassDependency dp[2] = {};
            dp[0].srcSubpass = VK_SUBPASS_EXTERNAL; dp[0].dstSubpass = 0;
            dp[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dp[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dp[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            dp[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            dp[1].srcSubpass = 0; dp[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dp[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dp[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
            dp[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dp[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            ri.attachmentCount = (uint32_t)ad.size(); ri.pAttachments = ad.data(); ri.subpassCount = 1; ri.pSubpasses = &sp; ri.dependencyCount = 2; ri.pDependencies = dp;
            if (x.f.CreateRenderPass(x.dev, &ri, nullptr, &p.rp) != VK_SUCCESS) { why = "render pass"; return false; }

            // pipeline
            VkPipelineShaderStageCreateInfo st[2] = {};
            st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = p.vs; st[0].pName = rp.vs_entry_point.c_str();
            st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = p.fs; st[1].pName = rp.ps_entry_point.c_str();
            VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; ia.topology = p.topo;
            VkViewport vp = { 0, 0, (float)p.vw, (float)p.vh, 0, 1 };
            VkRect2D sc = { { 0, 0 }, { p.vw, p.vh } };
            VkPipelineViewportStateCreateInfo vs2 = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; vs2.viewportCount = 1; vs2.pViewports = &vp; vs2.scissorCount = 1; vs2.pScissors = &sc;
            VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            std::vector<VkPipelineColorBlendAttachmentState> cba(afmt.size());
            for (size_t i = 0; i < afmt.size(); i++) {
                auto &c = cba[i];
                c.blendEnable = rp.blend_enable[i] ? VK_TRUE : VK_FALSE;
                c.srcColorBlendFactor = bf(rp.source_color_blend_factor[i]); c.dstColorBlendFactor = bf(rp.dest_color_blend_factor[i]); c.colorBlendOp = bo(rp.color_blend_op[i]);
                c.srcAlphaBlendFactor = bf(rp.source_alpha_blend_factor[i]); c.dstAlphaBlendFactor = bf(rp.dest_alpha_blend_factor[i]); c.alphaBlendOp = bo(rp.alpha_blend_op[i]);
                c.colorWriteMask = rp.render_target_write_mask[i] & 0xF;
            }
            VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; cb.attachmentCount = (uint32_t)cba.size(); cb.pAttachments = cba.data();
            VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vs2; gi.pRasterizationState = &rs;
            gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.layout = p.pl; gi.renderPass = p.rp; gi.subpass = 0; gi.basePipelineIndex = -1;
            if (x.f.CreateGraphicsPipelines(x.dev, VK_NULL_HANDLE, 1, &gi, nullptr, &p.pipe) != VK_SUCCESS) { why = "pipeline " + p.name; return false; }

            // framebuffer for render-target passes (the ones that render into the picture get theirs at record time / below)
            if (!p.toBack) {
                std::vector<VkImageView> views;
                for (int ti : p.targets) {
                    VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    vc.image = e.tex[ti].img.im; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = e.tex[ti].img.fmt; vc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    VkImageView v;
                    if (x.f.CreateImageView(x.dev, &vc, nullptr, &v) != VK_SUCCESS) { why = "target view"; return false; }
                    views.push_back(v);
                }
                VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                fi.renderPass = p.rp; fi.attachmentCount = (uint32_t)views.size(); fi.pAttachments = views.data();
                fi.width = e.tex[p.targets[0]].img.w; fi.height = e.tex[p.targets[0]].img.h; fi.layers = 1;
                // the framebuffer keeps the views alive; they are destroyed with it (views are tiny, keep them in the effect)
                for (auto v : views) { Img extra; extra.v = v; extra.owned = false; e.tex[p.targets[0]].img.vs = e.tex[p.targets[0]].img.vs; (void)extra; }
                if (x.f.CreateFramebuffer(x.dev, &fi, nullptr, &p.fb) != VK_SUCCESS) { why = "framebuffer"; return false; }
                // remember the render views to free them
                for (auto v : views) { Tex dummy; dummy.kind = 3; dummy.img.v = v; dummy.img.owned = false; e.tex.push_back(std::move(dummy)); }
            }
            e.passes.push_back(std::move(p));
        }
    }
    return true;
}

// Decides, pass by pass, which of the two picture images (A: from the upscaler, B: ours) each pass reads and writes, and writes the
// descriptors. The last pass that writes the picture renders straight into the swapchain image.
bool wire(Fx &x, std::vector<Effect *> &list, std::string &why) {
    int cur = 0;                                // 0 = A holds the picture, 1 = B
    Pass *lastBack = nullptr;
    for (auto *e : list) for (auto &p : e->passes) if (p.toBack) lastBack = &p;
    for (auto *e : list) {
        for (auto &p : e->passes) {
            p.colorIn = cur;
            if (p.toBack) {
                if (&p == lastBack) p.toOut = true;
                else { p.colorOut = 1 - cur; cur = 1 - cur; }
            }
        }
    }
    for (auto *e : list) {
        auto &m = e->L.mod;
        size_t pi = 0;
        for (auto &t : m.techniques) {
            for (auto &rp : t.passes) {
                Pass &p = e->passes[pi++];
                std::vector<VkDescriptorImageInfo> ii(rp.sampler_bindings.size());
                std::vector<VkWriteDescriptorSet> ws(rp.sampler_bindings.size());
                for (size_t k = 0; k < rp.sampler_bindings.size(); k++) {
                    const auto &sb = rp.sampler_bindings[k];
                    const auto &s = m.samplers[sb.index];
                    int ti = findTex(*e, s.texture_name);
                    VkImageView v = VK_NULL_HANDLE;
                    bool wantSrgb = s.srgb;
                    if (ti >= 0 && e->tex[ti].kind == 1) {
                        Img &im = p.colorIn == 0 ? x.A : x.B;
                        v = wantSrgb && im.vs ? im.vs : im.v;
                    } else if (ti >= 0 && e->tex[ti].kind == 2) {
                        v = x.dummyDepth.v;
                    } else if (ti >= 0) {
                        Img &im = e->tex[ti].img;
                        v = wantSrgb && im.vs ? im.vs : im.v;
                    }
                    if (!v) { why = "sampler without texture: " + s.name; return false; }
                    ii[k] = { e->samp[sb.index], v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    ws[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    ws[k].dstSet = p.ds1; ws[k].dstBinding = sb.entry_point_binding; ws[k].descriptorCount = 1;
                    ws[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[k].pImageInfo = &ii[k];
                }
                if (!ws.empty()) x.f.UpdateDescriptorSets(x.dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
            }
        }
    }
    // passes that write the picture but are not the last: framebuffers on A or B (render pass of the pass itself)
    for (auto *e : list) {
        for (auto &p : e->passes) {
            if (!p.toBack || p.toOut) continue;
            Img &im = p.colorOut == 0 ? x.A : x.B;
            VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fi.renderPass = p.rp; fi.attachmentCount = 1; fi.pAttachments = &im.v; fi.width = x.w; fi.height = x.h; fi.layers = 1;
            if (x.f.CreateFramebuffer(x.dev, &fi, nullptr, &p.fb) != VK_SUCCESS) { why = "picture framebuffer"; return false; }
        }
    }
    return true;
}

void barrierImage(Fx &x, VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                  VkPipelineStageFlags ss, VkPipelineStageFlags ds, uint32_t base, uint32_t count) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1 };
    x.f.CmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// mip chain of an image whose level 0 was just written (all levels rest in SHADER_READ_ONLY)
void genMips(Fx &x, VkCommandBuffer cb, Img &im, bool level0Fresh, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess) {
    if (im.levels < 2) return;
    VkImageLayout l0 = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrierImage(x, cb, im.im, l0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level0Fresh ? srcAccess : 0, VK_ACCESS_TRANSFER_READ_BIT, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1);
    for (uint32_t l = 1; l < im.levels; l++) {
        barrierImage(x, cb, im.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, l, 1);
        VkImageBlit bl = {};
        bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1 }; bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
        bl.srcOffsets[1] = { (int32_t)std::max(1u, im.w >> (l - 1)), (int32_t)std::max(1u, im.h >> (l - 1)), 1 };
        bl.dstOffsets[1] = { (int32_t)std::max(1u, im.w >> l), (int32_t)std::max(1u, im.h >> l), 1 };
        x.f.CmdBlitImage(cb, im.im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        barrierImage(x, cb, im.im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, l - 1, 1);
        barrierImage(x, cb, im.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, l, 1);
    }
    barrierImage(x, cb, im.im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, im.levels - 1, 1);
}

struct Handle {
    Fx x;
    std::vector<Effect *> order;     // effects in use
};

void destroyEffect(Fx &x, Effect &e) {
    auto &f = x.f;
    for (auto &p : e.passes) {
        if (p.fb) f.DestroyFramebuffer(x.dev, p.fb, nullptr);
        if (p.pipe) f.DestroyPipeline(x.dev, p.pipe, nullptr);
        if (p.rp) f.DestroyRenderPass(x.dev, p.rp, nullptr);
        if (p.pl) f.DestroyPipelineLayout(x.dev, p.pl, nullptr);
        if (p.dsl1) f.DestroyDescriptorSetLayout(x.dev, p.dsl1, nullptr);
        if (p.vs) f.DestroyShaderModule(x.dev, p.vs, nullptr);
        if (p.fs) f.DestroyShaderModule(x.dev, p.fs, nullptr);
    }
    if (e.pool) f.DestroyDescriptorPool(x.dev, e.pool, nullptr);
    if (e.dsl0) f.DestroyDescriptorSetLayout(x.dev, e.dsl0, nullptr);
    for (auto s : e.samp) f.DestroySampler(x.dev, s, nullptr);
    for (auto &t : e.tex) {
        if (t.kind == 3) { if (t.img.v) f.DestroyImageView(x.dev, t.img.v, nullptr); continue; }
        if (t.kind == 0) freeImage(x, t.img);
    }
    if (e.map) f.UnmapMemory(x.dev, e.uboMem);
    if (e.ubo) f.DestroyBuffer(x.dev, e.ubo, nullptr);
    if (e.uboMem) f.FreeMemory(x.dev, e.uboMem, nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// C interface for the Vulkan shim
// ---------------------------------------------------------------------------------------------------------------------

// Returns a handle, or null when there is nothing to run (no effect enabled, or none could be set up).
API void *epifx_create(const char *root, const char *preset, void *gipa, void *inst, void *phys, void *dev,
                       uint32_t w, uint32_t h, int fmt, uint64_t imgA, void *finalRp) {
    try {
        Preset pre = readPreset(preset ? preset : "");
        if (pre.effects.empty()) return nullptr;
        auto h2 = std::make_unique<Handle>();
        Fx &x = h2->x;
        auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa);
        auto in = reinterpret_cast<VkInstance>(inst);
        x.dev = reinterpret_cast<VkDevice>(dev); x.phys = reinterpret_cast<VkPhysicalDevice>(phys);
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(in, "vkGetDeviceProcAddr"));
        if (!gdpa) { LOGI("epifx: off, no vkGetDeviceProcAddr"); return nullptr; }
#define X(n) x.f.n = reinterpret_cast<PFN_vk##n>(gdpa(x.dev, "vk" #n)); if (!x.f.n) { LOGI("epifx: off, missing vk" #n); return nullptr; }
        VKFUNCS(X)
#undef X
        x.f.GetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(in, "vkGetPhysicalDeviceMemoryProperties"));
        x.f.GetFormatProps = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(get(in, "vkGetPhysicalDeviceFormatProperties"));
        if (!x.f.GetMemProps) return nullptr;
        x.f.GetMemProps(x.phys, &x.mp);
        x.w = w; x.h = h; x.fmt = (VkFormat)fmt; x.imgA = (VkImage)imgA; x.finalRp = reinterpret_cast<VkRenderPass>(finalRp);
        if (srgbOf(x.fmt) == VK_FORMAT_UNDEFINED) { LOGI("epifx: off, the swapchain format (%d) is not 8-bit UNORM", fmt); return nullptr; }

        // picture images: A is the upscaler's image (views only), B is ours
        x.A.owned = false; x.A.im = x.imgA; x.A.fmt = x.fmt; x.A.w = w; x.A.h = h;
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = x.imgA; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = x.fmt; vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (x.f.CreateImageView(x.dev, &vi, nullptr, &x.A.v) != VK_SUCCESS) return nullptr;
        vi.format = srgbOf(x.fmt);
        if (x.f.CreateImageView(x.dev, &vi, nullptr, &x.A.vs) != VK_SUCCESS) return nullptr;
        if (!makeImage(x, x.B, x.fmt, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, true)) { LOGI("epifx: off, picture image failed"); return nullptr; }
        if (!makeImage(x, x.dummyDepth, VK_FORMAT_R8_UNORM, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) return nullptr;

        std::string r = root ? root : "";
        std::vector<Effect *> used;
        for (auto &name : pre.effects) {
            auto e = std::make_unique<Effect>();
            std::string why;
            auto t0 = std::chrono::steady_clock::now();
            if (!buildEffect(x, r, name + ".fx", pre, *e, why)) {
                LOGI("epifx: %s skipped: %s", name.c_str(), why.c_str());
                destroyEffect(x, *e);
                continue;
            }
            LOGI("epifx: %s ready (%d passes, %lld ms)", name.c_str(), (int)e->passes.size(),
                 (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            used.push_back(e.get());
            x.fx.push_back(std::move(e));
        }
        std::string why;
        bool anyBack = false;
        for (auto *e : used) for (auto &p : e->passes) if (p.toBack) anyBack = true;
        if (used.empty() || !anyBack) { LOGI("epifx: off, nothing to run"); return nullptr; }
        if (!wire(x, used, why)) { LOGI("epifx: off, %s", why.c_str()); return nullptr; }
        h2->order = used;
        x.t0 = x.last = std::chrono::steady_clock::now();
        LOGI("epifx: %d effects, %dx%d", (int)used.size(), (int)w, (int)h);
        return h2.release();
    } catch (const std::exception &ex) {
        LOGI("epifx: off, exception: %s", ex.what());
    } catch (...) {
        LOGI("epifx: off, unknown exception");
    }
    return nullptr;
}

// One-time: puts the textures in their resting layout and uploads the pictures (runs on the present thread, with the queue).
API int epifx_upload(void *hp, void *queue, uint32_t family) {
    auto *h = static_cast<Handle *>(hp);
    Fx &x = h->x;
    auto &f = x.f;
    try {
        // staging buffer with every picture
        size_t total = 4;
        for (auto *e : h->order) for (auto &t : e->tex) if (t.hasPix) total += t.pix.size() + 16;
        VkBuffer sb = VK_NULL_HANDLE; VkDeviceMemory sm = VK_NULL_HANDLE; uint8_t *map = nullptr;
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = total; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (f.CreateBuffer(x.dev, &bi, nullptr, &sb) != VK_SUCCESS) return -1;
        VkMemoryRequirements mr; f.GetBufferMemoryRequirements(x.dev, sb, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size;
        ai.memoryTypeIndex = x.memTypeAny(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (f.AllocateMemory(x.dev, &ai, nullptr, &sm) != VK_SUCCESS) return -1;
        f.BindBufferMemory(x.dev, sb, sm, 0);
        if (f.MapMemory(x.dev, sm, 0, VK_WHOLE_SIZE, 0, (void **)&map) != VK_SUCCESS) return -1;
        size_t off = 0;
        *map = 0xFF; off = 4;                                 // the 1x1 "far" depth texel

        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pci.queueFamilyIndex = family;
        VkCommandPool pool; if (f.CreateCommandPool(x.dev, &pci, nullptr, &pool) != VK_SUCCESS) return -1;
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer cb; f.AllocateCommandBuffers(x.dev, &cai, &cb);
        VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        f.BeginCommandBuffer(cb, &bgi);

        auto toDst = [&](Img &im) { barrierImage(x, cb, im.im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, im.levels); };
        auto toRead = [&](Img &im, uint32_t base, uint32_t cnt, VkImageLayout from, VkAccessFlags sa, VkPipelineStageFlags ss) {
            barrierImage(x, cb, im.im, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sa, VK_ACCESS_SHADER_READ_BIT, ss, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, base, cnt); };

        toDst(x.dummyDepth);
        { VkBufferImageCopy c = {}; c.bufferOffset = 0; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; c.imageExtent = { 1, 1, 1 };
          f.CmdCopyBufferToImage(cb, sb, x.dummyDepth.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c); }
        toRead(x.dummyDepth, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrierImage(x, cb, x.B.im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1);

        for (auto *e : h->order) {
            for (auto &t : e->tex) {
                if (t.kind != 0) continue;
                Img &im = t.img;
                if (t.hasPix && !t.pix.empty()) {
                    memcpy(map + off, t.pix.data(), t.pix.size());
                    toDst(im);
                    VkBufferImageCopy c = {}; c.bufferOffset = off; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; c.imageExtent = { im.w, im.h, 1 };
                    f.CmdCopyBufferToImage(cb, sb, im.im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                    off += t.pix.size() + 16;
                    off = (off + 15) & ~(size_t)15;
                    toRead(im, 0, im.levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    if (im.levels > 1) genMips(x, cb, im, true, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
                } else {
                    barrierImage(x, cb, im.im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, im.levels);
                }
            }
        }
        f.EndCommandBuffer(cb);
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; f.CreateFence(x.dev, &fci, nullptr, &fence);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        VkResult r = f.QueueSubmit(reinterpret_cast<VkQueue>(queue), 1, &si, fence);
        if (r == VK_SUCCESS) f.WaitForFences(x.dev, 1, &fence, VK_TRUE, 5000000000ull);
        f.DestroyFence(x.dev, fence, nullptr);
        f.DestroyCommandPool(x.dev, pool, nullptr);
        f.UnmapMemory(x.dev, sm); f.DestroyBuffer(x.dev, sb, nullptr); f.FreeMemory(x.dev, sm, nullptr);
        x.uploaded = r == VK_SUCCESS;
        return r == VK_SUCCESS ? 0 : -1;
    } catch (...) { return -1; }
}

// Records all passes. Call after the upscaler wrote its picture into image A (which is left in SHADER_READ_ONLY_OPTIMAL).
// The last pass renders into `outFramebuffer` (the swapchain image) with the render pass given to epifx_create.
API void epifx_record(void *hp, void *cbp, uint64_t outFramebuffer) {
    auto *h = static_cast<Handle *>(hp);
    Fx &x = h->x;
    auto &f = x.f;
    VkCommandBuffer cb = reinterpret_cast<VkCommandBuffer>(cbp);
    // the images and render targets are shared by every frame: wait for the previous frame's use first
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    f.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkClearValue clr[8]; memset(clr, 0, sizeof clr);
    for (auto *e : h->order) {
        for (auto &p : e->passes) {
            VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            if (p.toOut) { rb.renderPass = x.finalRp; rb.framebuffer = reinterpret_cast<VkFramebuffer>(outFramebuffer); }
            else { rb.renderPass = p.rp; rb.framebuffer = p.fb; }
            rb.renderArea = { { 0, 0 }, { p.toBack ? x.w : e->tex[p.targets[0]].img.w, p.toBack ? x.h : e->tex[p.targets[0]].img.h } };
            rb.clearValueCount = 8; rb.pClearValues = clr;
            f.CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            f.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipe);
            VkDescriptorSet ds[2] = { e->ds0, p.ds1 };
            f.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pl, 0, 2, ds, 0, nullptr);
            f.CmdDraw(cb, p.nv, 1, 0, 0);
            f.CmdEndRenderPass(cb);
            for (int ti : p.mipTargets) genMips(x, cb, e->tex[ti].img, true, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
    }
}

// Per frame: time, frame counter and random values for effects that ask for them.
API void epifx_tick(void *hp) {
    auto *h = static_cast<Handle *>(hp);
    Fx &x = h->x;
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - x.last).count();
    double timer = std::chrono::duration<double, std::milli>(now - x.t0).count();
    x.last = now;
    x.frame++;
    time_t tt = time(nullptr);
    struct tm lt; localtime_r(&tt, &lt);
    for (auto *e : h->order) {
        if (e->dyn.empty()) continue;
        for (auto &d : e->dyn) {
            if (d.offset + 4 > e->uboCpu.size()) continue;
            auto setF = [&](unsigned i, float v) { if (d.offset + i * 4 + 4 <= e->uboCpu.size()) memcpy(&e->uboCpu[d.offset + i * 4], &v, 4); };
            auto setU = [&](unsigned i, uint32_t v) { if (d.offset + i * 4 + 4 <= e->uboCpu.size()) memcpy(&e->uboCpu[d.offset + i * 4], &v, 4); };
            if (d.src == "frametime") setF(0, (float)ms);
            else if (d.src == "framecount") setU(0, (uint32_t)x.frame);
            else if (d.src == "timer") setF(0, (float)timer);
            else if (d.src == "random") setU(0, (uint32_t)(d.mn + (double)(rand() % 32768) / 32768.0 * (d.mx - d.mn)));
            else if (d.src == "date") { setF(0, (float)(lt.tm_year + 1900)); setF(1, (float)(lt.tm_mon + 1)); setF(2, (float)lt.tm_mday); setF(3, (float)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec)); }
            else if (d.src == "pingpong") { float t = (float)fmod(timer / 1000.0, 2.0); setF(0, t > 1 ? 2 - t : t); setF(1, 0); }
        }
        memcpy(e->map, e->uboCpu.data(), e->uboCpu.size());
    }
}

API void epifx_destroy(void *hp) {
    auto *h = static_cast<Handle *>(hp);
    if (!h) return;
    Fx &x = h->x;
    for (auto &e : x.fx) destroyEffect(x, *e);
    freeImage(x, x.B);
    freeImage(x, x.dummyDepth);
    if (x.A.v) x.f.DestroyImageView(x.dev, x.A.v, nullptr);
    if (x.A.vs) x.f.DestroyImageView(x.dev, x.A.vs, nullptr);
    delete h;
}

// ---------------------------------------------------------------------------------------------------------------------
// JNI for the app
// ---------------------------------------------------------------------------------------------------------------------

extern "C" JNIEXPORT jstring JNICALL Java_com_freecrane_adrenotest_Epi_describe(JNIEnv *env, jclass, jstring root, jstring file) {
    const char *r = env->GetStringUTFChars(root, nullptr);
    const char *fl = env->GetStringUTFChars(file, nullptr);
    std::string out = describe(r, fl);
    env->ReleaseStringUTFChars(root, r);
    env->ReleaseStringUTFChars(file, fl);
    return env->NewStringUTF(out.c_str());
}
