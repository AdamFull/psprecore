#include "platform/gpu_vulkan.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/memory.hpp"
#include "ge_frag.h"
#include "ge_vert.h"
#include "gpu/texture.hpp"
#include "gpu/vertex_pipeline.hpp"

namespace ff7cc::platform {

namespace {

namespace ge = rt::ge;
using rt::gpu::ClipVertex;

constexpr int kFrames = 2;
constexpr VkDeviceSize kVertexBytes = 16u << 20;
constexpr VkDeviceSize kStagingBytes = 32u << 20;
constexpr int kTargetHeight = 512;  // render targets cover the GE's addressable 512 lines
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan GE renderer: ") + what + " failed (" + std::to_string(r) + ")");
}

// Fast non-cryptographic hash. Four independent lanes keep the multipliers busy (several bytes per cycle).
std::uint64_t hash_bytes(const void* data, std::size_t size, std::uint64_t h = 0x9E3779B97F4A7C15ull) {
    const auto* b = static_cast<const std::uint8_t*>(data);
    constexpr std::uint64_t kMul = 0xFF51AFD7ED558CCDull;
    std::size_t i = 0;
    if (size >= 32) {
        std::uint64_t lane[4] = {h, h ^ 0x2545F4914F6CDD1Dull, h ^ 0x94D049BB133111EBull, h ^ 0xBF58476D1CE4E5B9ull};
        for (; i + 32 <= size; i += 32)
            for (int k = 0; k < 4; ++k) {
                std::uint64_t w;
                std::memcpy(&w, b + i + k * 8, 8);
                lane[k] = (lane[k] ^ w) * kMul;
                lane[k] ^= lane[k] >> 29;
            }
        h = lane[0] ^ (lane[1] * 3) ^ (lane[2] * 5) ^ (lane[3] * 7);
    }
    for (; i + 8 <= size; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * kMul;
        h ^= h >> 32;
    }
    for (; i < size; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
    return (h ^ (h >> 31)) * kMul;
}

struct GpuVertex {
    float pos[4];
    float uvq[3];
    std::uint32_t color;
    std::uint32_t spec_fog;
};
static_assert(sizeof(GpuVertex) == 36);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    std::uint8_t* ptr = nullptr;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    int width = 0, height = 0;
    std::uint32_t mips = 1;
};

struct Target {
    std::uint32_t address = 0, stride = 0;
    int format = 3;
    int used_height = 272;  // lines the game has drawn to (scissor extent); textures past it are not ours
    Image color;
};

struct Texture {
    Image image;
    std::uint64_t content = 0;
    std::uint64_t last_frame = 0;
    std::uint64_t checked = 0;  // rt::guest_write_counter() when the data was last verified
};

// Everything that selects a VkPipeline. All fields are bytes so the struct hashes and compares as memory.
struct PipelineKey {
    std::uint8_t topology = 0;
    std::uint8_t blend = 0, src = 0, dst = 0, op = 0;
    std::uint8_t write_mask = 0xF;
    std::uint8_t depth_test = 0, depth_func = 0, depth_write = 0;
    std::uint8_t stencil = 0, stencil_func = 0, stencil_fail = 0, stencil_zfail = 0, stencil_pass = 0;
    std::uint8_t logic = 0, logic_op = 0;
    // Fragment specialization constants (shaders/ge.slang).
    std::uint8_t texture = 0, tex_func = 0, tex_alpha = 0, color_double = 0, fog = 0, alpha_test = 1, color_test = 1,
                 clear = 0, src_factor = 0, double_alpha = 0;
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(std::is_trivially_copyable_v<PipelineKey>);
constexpr char kPipelineKeysMagic[8] = {'F', 'F', '7', 'P', 'K', 'E', 'Y', '1'};
struct PipelineKeyHash {
    std::size_t operator()(const PipelineKey& k) const { return static_cast<std::size_t>(hash_bytes(&k, sizeof k)); }
};

struct FragmentConstants {
    float env[4];
    float fog[4];
    float fixed_a[4];
    std::uint32_t alpha_ref, alpha_mask, color_ref, color_mask;
};
static_assert(sizeof(FragmentConstants) == 64);

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE, draw = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    Buffer vertices, staging;
    VkDeviceSize vertex_used = 0, staging_used = 0;
    std::vector<Image> garbage;
    std::map<std::pair<VkImageView, VkSampler>, VkDescriptorSet> sets;
};

VkCompareOp compare_op(std::uint32_t f) {
    static constexpr VkCompareOp kOps[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
                                            VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                            VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    return kOps[f & 7];
}
VkStencilOp stencil_op(std::uint32_t op) {
    static constexpr VkStencilOp kOps[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                            VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                            VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP};
    return kOps[op & 7];
}

std::uint32_t pack_color(const float c[4]) {
    std::uint32_t out = 0;
    for (int i = 0; i < 4; ++i)
        out |= static_cast<std::uint32_t>(std::clamp(static_cast<int>(c[i] + 0.5f), 0, 255)) << (i * 8);
    return out;
}

}  // namespace

struct VulkanGeRenderer::Impl {
    VulkanContext vk;
    rt::GuestMemory& memory;
    int scale;
    int pending_scale;
    bool guest_copies = true;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
    std::filesystem::path cache_path;
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::uint64_t cache_dirty_frame = 0;  // frame of the last pipeline created since the cache was saved (0: saved)
    std::map<std::uint32_t, VkSampler> samplers;
    std::array<Frame, kFrames> frames;
    int current = 0;
    bool recording = false;
    std::uint64_t frame_number = 1;
    VulkanGeRenderer::Stats stats;

    std::map<std::uint32_t, Target> targets;   // by VRAM address
    std::map<std::uint64_t, Image> depths;     // by (address, width)
    std::unordered_map<std::uint64_t, Texture> textures;
    Image white;  // bound when texturing is off (the descriptor is always declared)
    Buffer readback;
    VkDeviceSize readback_size = 0;

    // Active rendering.
    Target* bound_target = nullptr;
    Image* bound_depth = nullptr;
    bool rendering = false;
    VkPipeline bound_pipeline = VK_NULL_HANDLE;

    rt::gpu::VertexPipeline vp;
    std::vector<ClipVertex> clip;
    std::vector<GpuVertex> out;

    Impl(const VulkanContext& context, rt::GuestMemory& mem, int s, std::filesystem::path cache_file)
        : vk(context), memory(mem), scale(std::max(1, s)), pending_scale(scale), cache_path(std::move(cache_file)) {}

    Frame& frame() { return frames[static_cast<std::size_t>(current)]; }

    // ---- resources -------------------------------------------------------------------------------------

    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback = false) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        Buffer b;
        VmaAllocationInfo info;
        check(vmaCreateBuffer(vk.allocator, &bci, &aci, &b.buffer, &b.memory, &info), "buffer");
        b.ptr = static_cast<std::uint8_t*>(info.pMappedData);
        return b;
    }

    Image make_image(int w, int h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mips = 1) {
        Image img;
        img.width = w;
        img.height = h;
        img.aspect = aspect;
        img.mips = mips;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        ici.mipLevels = mips;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        check(vmaCreateImage(vk.allocator, &ici, &aci, &img.image, &img.memory, nullptr), "image");
        if (!(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
            return img;  // transfer-only staging image: no view
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {aspect, 0, mips, 0, 1};
        check(vkCreateImageView(vk.device, &vci, nullptr, &img.view), "image view");
        return img;
    }

    void destroy(Image& img) {
        if (img.view) vkDestroyImageView(vk.device, img.view, nullptr);
        if (img.image) vmaDestroyImage(vk.allocator, img.image, img.memory);
        img = Image{};
    }

    // Conservative full barrier: correct for every transition this renderer makes.
    void transition(VkCommandBuffer cmd, Image& img, VkImageLayout to) {
        if (img.layout == to) return;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = img.layout;
        b.newLayout = to;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {img.aspect, 0, img.mips, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        img.layout = to;
    }

    // Reserves staging memory for this frame; null when the frame's staging buffer is full.
    std::uint8_t* staging(VkDeviceSize size, VkDeviceSize& offset) {
        Frame& f = frame();
        const VkDeviceSize aligned = (f.staging_used + 15) & ~VkDeviceSize{15};
        if (aligned + size > kStagingBytes) return nullptr;
        offset = aligned;
        f.staging_used = aligned + size;
        return f.staging.ptr + aligned;
    }

    void init() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1;
        dl.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(vk.device, &dl, nullptr, &set_layout), "descriptor set layout");
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FragmentConstants)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &set_layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(vk.device, &pl, nullptr, &layout), "pipeline layout");

        const auto module = [&](const unsigned char* code, std::size_t size) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = size;
            ci.pCode = reinterpret_cast<const std::uint32_t*>(code);
            VkShaderModule m;
            check(vkCreateShaderModule(vk.device, &ci, nullptr, &m), "shader module");
            return m;
        };
        vert = module(ge_vert, sizeof ge_vert);
        frag = module(ge_frag, sizeof ge_frag);
        load_pipeline_cache();

        for (Frame& f : frames) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.queueFamilyIndex = vk.queue_family;
            check(vkCreateCommandPool(vk.device, &pci, nullptr, &f.pool), "command pool");
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = f.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 2;
            VkCommandBuffer cmds[2];
            check(vkAllocateCommandBuffers(vk.device, &cai, cmds), "command buffers");
            f.upload = cmds[0];
            f.draw = cmds[1];
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(vk.device, &fci, nullptr, &f.fence), "fence");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 8192;
            dpi.poolSizeCount = 1;
            dpi.pPoolSizes = &size;
            check(vkCreateDescriptorPool(vk.device, &dpi, nullptr, &f.descriptors), "descriptor pool");
            f.vertices = make_buffer(kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            f.staging = make_buffer(kStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        }

        begin_frame();
        white = make_image(1, 1, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(frame().draw, white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue one{{1, 1, 1, 1}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(frame().draw, white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
        transition(frame().draw, white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    static std::vector<char> read_file(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    static void write_file(const std::filesystem::path& path, const void* data, std::size_t size) {
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size))) return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    }
    std::filesystem::path keys_path() const {
        std::filesystem::path p = cache_path;
        return p += ".keys";
    }

    // Pipelines are created when a draw first needs them, which takes milliseconds each and stalls the game (and
    // its audio) when an effect brings several at once. So the driver's pipeline cache is kept on disk (the driver
    // checks its header and ignores data from another GPU or driver), and every pipeline used before is created
    // again at start from the saved key list.
    void load_pipeline_cache() {
        if (cache_path.empty()) {
            VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            check(vkCreatePipelineCache(vk.device, &ci, nullptr, &cache), "pipeline cache");
            return;
        }
        const std::vector<char> data = read_file(cache_path);
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(vk.device, &ci, nullptr, &cache) != VK_SUCCESS) {
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            check(vkCreatePipelineCache(vk.device, &ci, nullptr, &cache), "pipeline cache");
        }
        const std::vector<char> keys = read_file(keys_path());
        constexpr std::size_t header = sizeof kPipelineKeysMagic + 4;
        std::uint32_t key_size = 0;
        if (keys.size() >= header) std::memcpy(&key_size, keys.data() + sizeof kPipelineKeysMagic, 4);
        if (keys.size() < header || std::memcmp(keys.data(), kPipelineKeysMagic, sizeof kPipelineKeysMagic) != 0 ||
            key_size != sizeof(PipelineKey))
            return;
        const auto start = std::chrono::steady_clock::now();
        std::size_t count = 0;
        for (std::size_t at = header; at + sizeof(PipelineKey) <= keys.size(); at += sizeof(PipelineKey), ++count) {
            PipelineKey k;
            std::memcpy(&k, keys.data() + at, sizeof k);
            pipeline(k);
        }
        cache_dirty_frame = 0;
        stats = {};
        std::fprintf(stderr, "Vulkan: %zu pipelines prepared in %.0f ms\n", count,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }

    void save_pipeline_cache() {
        cache_dirty_frame = 0;
        if (cache_path.empty()) return;
        std::size_t size = 0;
        if (vkGetPipelineCacheData(vk.device, cache, &size, nullptr) != VK_SUCCESS || !size) return;
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(vk.device, cache, &size, data.data()) != VK_SUCCESS) return;
        write_file(cache_path, data.data(), size);
        std::vector<char> keys(kPipelineKeysMagic, kPipelineKeysMagic + sizeof kPipelineKeysMagic);
        const std::uint32_t key_size = sizeof(PipelineKey);
        keys.insert(keys.end(), reinterpret_cast<const char*>(&key_size), reinterpret_cast<const char*>(&key_size) + 4);
        for (const auto& [k, _] : pipelines)
            keys.insert(keys.end(), reinterpret_cast<const char*>(&k), reinterpret_cast<const char*>(&k) + sizeof k);
        write_file(keys_path(), keys.data(), keys.size());
    }

    void shutdown() {
        vkDeviceWaitIdle(vk.device);
        if (cache_dirty_frame) save_pipeline_cache();
        vkDestroyPipelineCache(vk.device, cache, nullptr);
        for (Frame& f : frames) {
            for (Image& img : f.garbage) destroy(img);
            vmaDestroyBuffer(vk.allocator, f.vertices.buffer, f.vertices.memory);
            vmaDestroyBuffer(vk.allocator, f.staging.buffer, f.staging.memory);
            vkDestroyDescriptorPool(vk.device, f.descriptors, nullptr);
            vkDestroyFence(vk.device, f.fence, nullptr);
            vkDestroyCommandPool(vk.device, f.pool, nullptr);
        }
        for (auto& [_, t] : targets) destroy(t.color);
        for (auto& [_, d] : depths) destroy(d);
        for (auto& [_, t] : textures) destroy(t.image);
        destroy(white);
        if (readback.buffer) vmaDestroyBuffer(vk.allocator, readback.buffer, readback.memory);
        for (auto& [_, p] : pipelines) vkDestroyPipeline(vk.device, p, nullptr);
        for (auto& [_, s] : samplers) vkDestroySampler(vk.device, s, nullptr);
        vkDestroyShaderModule(vk.device, vert, nullptr);
        vkDestroyShaderModule(vk.device, frag, nullptr);
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
        vkDestroyDescriptorSetLayout(vk.device, set_layout, nullptr);
    }

    // ---- frames ----------------------------------------------------------------------------------------

    void begin_frame() {
        if (recording) return;
        if (pending_scale != scale) {
            // Render targets are rebuilt at the new size and re-seeded from guest VRAM on first use.
            vkDeviceWaitIdle(vk.device);
            for (auto& [_, t] : targets) destroy(t.color);
            for (auto& [_, d] : depths) destroy(d);
            targets.clear();
            depths.clear();
            scale = pending_scale;
        }
        Frame& f = frame();
        vkWaitForFences(vk.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        for (Image& img : f.garbage) destroy(img);
        f.garbage.clear();
        f.sets.clear();
        vkResetDescriptorPool(vk.device, f.descriptors, 0);
        vkResetCommandPool(vk.device, f.pool, 0);
        f.vertex_used = f.staging_used = 0;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.upload, &bi);
        vkBeginCommandBuffer(f.draw, &bi);
        recording = true;
        bound_pipeline = VK_NULL_HANDLE;
    }

    void submit_frame() {
        Frame& f = frame();
        end_rendering();
        vkEndCommandBuffer(f.upload);
        vkEndCommandBuffer(f.draw);
        vkResetFences(vk.device, 1, &f.fence);
        const VkCommandBuffer cmds[2] = {f.upload, f.draw};
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 2;
        si.pCommandBuffers = cmds;
        check(vkQueueSubmit(vk.queue, 1, &si, f.fence), "vkQueueSubmit");
        recording = false;
        current = (current + 1) % kFrames;
        ++frame_number;
        evict_textures();
    }

    void evict_textures() {
        for (auto it = textures.begin(); it != textures.end();) {
            if (frame_number - it->second.last_frame > 240) {
                frame().garbage.push_back(it->second.image);  // destroyed once this frame slot is reused
                it = textures.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- render targets ----------------------------------------------------------------------------------

    static int bytes_per_pixel(int format) { return format == 3 ? 4 : 2; }

    // Uploads guest VRAM pixels (x, y, w, h in PSP pixels) into a render target, scaled.
    void upload_region(Target& t, int x, int y, int w, int h) {
        x = std::clamp(x, 0, static_cast<int>(t.stride));
        y = std::clamp(y, 0, kTargetHeight);
        w = std::min(w, static_cast<int>(t.stride) - x);
        h = std::min(h, kTargetHeight - y);
        if (w <= 0 || h <= 0) return;
        VkDeviceSize offset = 0;
        auto* dst = reinterpret_cast<std::uint32_t*>(staging(static_cast<VkDeviceSize>(w) * h * 4, offset));
        if (!dst) return;
        const int bpp = bytes_per_pixel(t.format);
        for (int row = 0; row < h; ++row) {
            const std::uint8_t* src = memory.ptr(t.address + static_cast<std::uint32_t>(((y + row) * static_cast<int>(t.stride) + x) * bpp));
            for (int col = 0; col < w; ++col) {
                std::uint32_t p;
                if (bpp == 4) {
                    std::memcpy(&p, src + col * 4, 4);
                } else {
                    const std::uint32_t v = static_cast<std::uint32_t>(src[col * 2] | (src[col * 2 + 1] << 8));
                    p = t.format == 0 ? rt::gpu::decode_5650(v) : t.format == 1 ? rt::gpu::decode_5551(v) : rt::gpu::decode_4444(v);
                }
                dst[row * w + col] = p;
            }
        }
        end_rendering();
        const VkCommandBuffer cmd = frame().draw;
        Image tmp = make_image(w, h, kColorFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
        vkCmdCopyBufferToImage(cmd, frame().staging.buffer, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(cmd, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cmd, t.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x * scale, y * scale, 0};
        blit.dstOffsets[1] = {(x + w) * scale, (y + h) * scale, 1};
        vkCmdBlitImage(cmd, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);
        frame().garbage.push_back(tmp);
    }

    Target& target(std::uint32_t address, std::uint32_t stride, int format) {
        address = 0x04000000 | (address & 0x1FFFF0);
        stride = std::clamp<std::uint32_t>(stride, 64, 1024);
        auto it = targets.find(address);
        if (it != targets.end() && it->second.stride == stride) {
            it->second.format = format;
            return it->second;
        }
        if (it != targets.end()) {
            if (bound_target == &it->second) end_rendering();
            frame().garbage.push_back(it->second.color);
            targets.erase(it);
        }
        Target& t = targets[address];
        t.address = address;
        t.stride = stride;
        t.format = format;
        t.color = make_image(static_cast<int>(stride) * scale, kTargetHeight * scale, kColorFormat,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
        // Start from what the CPU left in VRAM.
        upload_region(t, 0, 0, static_cast<int>(stride), 272);
        return t;
    }

    Image& depth(std::uint32_t address, const Target& t) {
        const std::uint64_t key = (std::uint64_t{0x04000000u | (address & 0x1FFFF0)} << 16) | t.stride;
        auto it = depths.find(key);
        if (it != depths.end()) return it->second;
        Image& d = depths[key];
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        d = make_image(t.color.width, t.color.height, vk.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT, aspect);
        const VkCommandBuffer cmd = frame().draw;
        end_rendering();
        transition(cmd, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearDepthStencilValue zero{0.0f, 0};
        const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        return d;
    }

    void begin_rendering(Target& t, Image& d) {
        if (rendering && bound_target == &t && bound_depth == &d) return;
        end_rendering();
        const VkCommandBuffer cmd = frame().draw;
        transition(cmd, t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(cmd, d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = t.color.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo ds{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ds.imageView = d.view;
        ds.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ds.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ds.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {static_cast<std::uint32_t>(t.color.width), static_cast<std::uint32_t>(t.color.height)}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &ds;
        ri.pStencilAttachment = &ds;
        vk.begin_rendering(cmd, &ri);
        const VkViewport viewport{0, 0, static_cast<float>(t.color.width), static_cast<float>(t.color.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        rendering = true;
        bound_target = &t;
        bound_depth = &d;
        bound_pipeline = VK_NULL_HANDLE;
    }

    void end_rendering() {
        if (!rendering) return;
        vk.end_rendering(frame().draw);
        rendering = false;
    }

    // ---- textures ----------------------------------------------------------------------------------------

    VkSampler sampler(bool min_linear, bool mag_linear, bool clamp_u, bool clamp_v, bool mipmap, std::uint32_t levels) {
        const std::uint32_t key = (min_linear ? 1u : 0u) | (mag_linear ? 2u : 0u) | (clamp_u ? 4u : 0u) | (clamp_v ? 8u : 0u) |
                                  (mipmap ? 16u : 0u) | (levels << 8);
        auto& s = samplers[key];
        if (s) return s;
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV = clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.maxLod = mipmap ? static_cast<float>(levels - 1) : 0.0f;
        check(vkCreateSampler(vk.device, &ci, nullptr, &s), "sampler");
        return s;
    }

    // Texture from guest memory, decoded and uploaded once per content.
    Image* texture(const ge::State& s, const rt::gpu::TextureSampler& ts, std::uint32_t address) {
        std::uint64_t key = hash_bytes(&address, 4);
        const std::uint32_t params[] = {s.param(ge::TEXFORMAT), s.param(ge::TEXMODE), s.param(ge::TEXSIZE0),
                                        s.param(ge::TEXBUFWIDTH0), static_cast<std::uint32_t>(ts.levels())};
        key = hash_bytes(params, sizeof params, key);
        // The CLUT can change between draws (palette swaps); texture data only between batches, so its (much
        // larger) hash is computed once per batch.
        std::uint64_t clut = 0;
        if (ts.uses_clut()) {
            clut = hash_bytes(s.clut.data(), s.clut.size() * sizeof(s.clut[0]));
            const std::uint32_t clut_format = s.param(ge::CLUTFORMAT);
            clut = hash_bytes(&clut_format, 4, clut);
        }
        // Each palette gets its own cache entry: games draw the same texels with different CLUTs (palette swaps,
        // colour cycling), which would otherwise re-decode the texture on every switch.
        key = hash_bytes(&clut, sizeof clut, key);
        Texture& t = textures[key];
        t.last_frame = frame_number;
        // Re-hash the texels only when guest memory under them may have changed since the last check.
        std::uint64_t written = 0;
        for (int l = 0; l < ts.levels(); ++l) {
            const std::uint32_t level_address = ((s.param(static_cast<std::uint8_t>(ge::TEXBUFWIDTH0 + l)) & 0x0F0000) << 8) |
                                                (s.param(static_cast<std::uint8_t>(ge::TEXADDR0 + l)) & 0xFFFFF0);
            written = std::max(written, rt::guest_written_since(level_address, static_cast<std::uint32_t>(ts.level_size(l))));
        }
        if (t.image.image && written <= t.checked) return &t.image;
        std::uint64_t content = clut;
        for (int l = 0; l < ts.levels(); ++l) {
            content = hash_bytes(ts.level_data(l), ts.level_size(l), content);
            stats.hashed_bytes += ts.level_size(l);
        }
        t.checked = rt::guest_write_counter();
        if (t.image.image && t.content == content) return &t.image;
        ++stats.uploads;
        stats.uploaded_texels += static_cast<std::uint64_t>(ts.width(0)) * static_cast<std::uint64_t>(ts.height(0));
        if (t.image.image) frame().garbage.push_back(t.image);
        t.content = content;

        const std::uint32_t levels = static_cast<std::uint32_t>(ts.levels());
        t.image = make_image(ts.width(0), ts.height(0), kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, levels);
        const VkCommandBuffer cmd = frame().upload;
        transition(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (std::uint32_t l = 0; l < levels; ++l) {
            const int w = ts.width(static_cast<int>(l)), h = ts.height(static_cast<int>(l));
            VkDeviceSize offset = 0;
            auto* dst = reinterpret_cast<std::uint32_t*>(staging(static_cast<VkDeviceSize>(w) * h * 4, offset));
            if (!dst) break;  // staging full: the level stays undefined this frame (rare)
            ts.decode(static_cast<int>(l), dst);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            copy.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
            vkCmdCopyBufferToImage(cmd, frame().staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        transition(cmd, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return &t.image;
    }

    // A texture inside a render target: sample a scaled copy of that region (render-to-texture effects).
    Image* target_texture(Target& t, std::uint32_t address, int w, int h) {
        const int bpp = bytes_per_pixel(t.format);
        const std::uint32_t offset = (address - t.address) / static_cast<std::uint32_t>(bpp);
        const int x = static_cast<int>(offset % t.stride), y = static_cast<int>(offset / t.stride);
        const int cw = std::min(w, static_cast<int>(t.stride) - x), ch = std::min(h, kTargetHeight - y);
        if (cw <= 0 || ch <= 0) return nullptr;
        end_rendering();
        const VkCommandBuffer cmd = frame().draw;
        Image copy = make_image(w * scale, h * scale, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cmd, copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        transition(cmd, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {x * scale, y * scale, 0};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {static_cast<std::uint32_t>(cw * scale), static_cast<std::uint32_t>(ch * scale), 1};
        vkCmdCopyImage(cmd, t.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);
        transition(cmd, copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        frame().garbage.push_back(copy);
        return &frame().garbage.back();
    }

    Target* target_containing(std::uint32_t address) {
        address = rt::ge::vram_canonical(address);
        for (auto& [base, t] : targets) {
            const std::uint32_t size = t.stride * static_cast<std::uint32_t>(t.used_height * bytes_per_pixel(t.format));
            if (address >= base && address < base + size) return &t;
        }
        return nullptr;
    }

    VkDescriptorSet descriptor(VkImageView view, VkSampler smp) {
        Frame& f = frame();
        auto& set = f.sets[{view, smp}];
        if (set) return set;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.descriptors;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkDescriptorImageInfo info{smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
        return set;
    }

    // ---- pipelines ---------------------------------------------------------------------------------------

    VkPipeline pipeline(const PipelineKey& k) {
        auto& p = pipelines[k];
        if (p) return p;
        const std::uint32_t spec_data[10] = {k.texture, k.tex_func, k.tex_alpha, k.color_double, k.fog,
                                             k.alpha_test, k.color_test, k.clear, k.src_factor, k.double_alpha};
        VkSpecializationMapEntry entries[10];
        for (std::uint32_t i = 0; i < 10; ++i) entries[i] = {i, i * 4, 4};
        VkSpecializationInfo spec{10, entries, sizeof spec_data, spec_data};
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", &spec};

        const VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[4] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 28},
            {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 32},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = k.topology == 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                      : k.topology == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                        : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        VkPipelineViewportStateCreateInfo vp_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp_state.viewportCount = 1;
        vp_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.depthClampEnable = vk.depth_clamp ? VK_TRUE : VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;  // culled on the CPU with the PSP's screen-space rule
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = k.depth_test;
        ds.depthWriteEnable = k.depth_write;
        ds.depthCompareOp = compare_op(k.depth_func);
        ds.stencilTestEnable = k.stencil;
        ds.front.compareOp = compare_op(k.stencil_func);
        ds.front.failOp = stencil_op(k.stencil_fail);
        ds.front.depthFailOp = stencil_op(k.stencil_zfail);
        ds.front.passOp = stencil_op(k.stencil_pass);
        ds.back = ds.front;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = k.blend;
        blend.srcColorBlendFactor = static_cast<VkBlendFactor>(k.src);
        blend.dstColorBlendFactor = static_cast<VkBlendFactor>(k.dst);
        blend.colorBlendOp = static_cast<VkBlendOp>(k.op);
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = k.write_mask;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.logicOpEnable = k.logic;
        cb.logicOp = static_cast<VkLogicOp>(k.logic_op);
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                          VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                          VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamic));
        dyn.pDynamicStates = dynamic;
        VkPipelineRenderingCreateInfo rendering_info{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        const VkFormat color_format = kColorFormat;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachmentFormats = &color_format;
        rendering_info.depthAttachmentFormat = vk.depth_format;
        rendering_info.stencilAttachmentFormat = vk.depth_format;

        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &rendering_info;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp_state;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dyn;
        pci.layout = layout;
        const auto start = std::chrono::steady_clock::now();
        check(vkCreateGraphicsPipelines(vk.device, cache, 1, &pci, nullptr, &p), "graphics pipeline");
        cache_dirty_frame = frame_number;
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++stats.pipelines;
        stats.pipeline_ms += took;
        stats.worst_pipeline_ms = std::max(stats.worst_pipeline_ms, took);
        return p;
    }

    // ---- vertex assembly ---------------------------------------------------------------------------------

    float target_w = 512, target_h = kTargetHeight;

    // Drawing-space pixels (already projected) -> Vulkan clip space with w = 1.
    GpuVertex screen_vertex(float x, float y, float z, float s, float t, float q, const float* color, const float* spec, float fog) const {
        GpuVertex v{};
        v.pos[0] = 2.0f * x / target_w - 1.0f;
        v.pos[1] = 2.0f * y / target_h - 1.0f;
        v.pos[2] = std::clamp(z, 0.0f, 65535.0f) / 65535.0f;
        v.pos[3] = 1.0f;
        v.uvq[0] = s;
        v.uvq[1] = t;
        v.uvq[2] = q;
        const float sf[4] = {spec[0], spec[1], spec[2], fog * 255.0f};
        v.color = pack_color(color);
        v.spec_fog = pack_color(sf);
        return v;
    }

    // Transformed vertex -> Vulkan clip space, folding in the GE viewport so the GPU interpolates perspective-correct.
    GpuVertex clip_vertex(const ClipVertex& c, const float* color) const {
        GpuVertex v{};
        const float w = c.clip[3];
        v.pos[0] = c.clip[0] * (2.0f * vp.vp_scale[0] / target_w) + w * (2.0f * (vp.vp_center[0] - vp.offset_x) / target_w - 1.0f);
        v.pos[1] = c.clip[1] * (2.0f * vp.vp_scale[1] / target_h) + w * (2.0f * (vp.vp_center[1] - vp.offset_y) / target_h - 1.0f);
        v.pos[2] = c.clip[2] * (vp.vp_scale[2] / 65535.0f) + w * (vp.vp_center[2] / 65535.0f);
        v.pos[3] = w;
        v.uvq[0] = c.s;
        v.uvq[1] = c.t;
        v.uvq[2] = c.q;
        const float sf[4] = {c.spec[0], c.spec[1], c.spec[2], c.fog * 255.0f};
        v.color = pack_color(color);
        v.spec_fog = pack_color(sf);
        return v;
    }

    void screen_xy(const ClipVertex& c, float& x, float& y, float& z) const {
        if (vp.through) {
            x = c.clip[0];
            y = c.clip[1];
            z = c.clip[2];
            return;
        }
        const float iw = 1.0f / c.clip[3];
        x = c.clip[0] * iw * vp.vp_scale[0] + vp.vp_center[0] - vp.offset_x;
        y = c.clip[1] * iw * vp.vp_scale[1] + vp.vp_center[1] - vp.offset_y;
        z = c.clip[2] * iw * vp.vp_scale[2] + vp.vp_center[2];
    }

    bool cull = false, flat = false;
    int cull_mode = 0;

    void triangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
        const float* provoking = c.color;
        if (vp.through) {
            for (const ClipVertex* p : {&a, &b, &c})
                out.push_back(screen_vertex(p->clip[0], p->clip[1], p->clip[2], p->s, p->t, p->q, flat ? provoking : p->color, p->spec, p->fog));
            return;
        }
        if (cull && a.clip[3] > 0 && b.clip[3] > 0 && c.clip[3] > 0) {
            float ax, ay, az, bx, by, bz, cx, cy, cz;
            screen_xy(a, ax, ay, az);
            screen_xy(b, bx, by, bz);
            screen_xy(c, cx, cy, cz);
            const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
            if (cull_mode == 1 ? area < 0.0f : area > 0.0f) return;
        }
        // Near-plane clipping (z >= -w), as the software renderer does; the GPU clips the rest.
        ClipVertex in[8] = {a, b, c}, clipped[8];
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            const ClipVertex& p = in[i];
            const ClipVertex& q = in[(i + 1) % 3];
            const float dp = p.clip[2] + p.clip[3], dq = q.clip[2] + q.clip[3];
            if (dp >= 0.0f) clipped[n++] = p;
            if ((dp >= 0.0f) != (dq >= 0.0f)) {
                const float t = dp / (dp - dq);
                ClipVertex r;
                const auto lerp = [t](float x, float y) { return x + (y - x) * t; };
                for (int k = 0; k < 4; ++k) r.clip[k] = lerp(p.clip[k], q.clip[k]);
                r.s = lerp(p.s, q.s);
                r.t = lerp(p.t, q.t);
                r.q = lerp(p.q, q.q);
                for (int k = 0; k < 4; ++k) r.color[k] = lerp(p.color[k], q.color[k]);
                for (int k = 0; k < 3; ++k) r.spec[k] = lerp(p.spec[k], q.spec[k]);
                r.fog = lerp(p.fog, q.fog);
                clipped[n++] = r;
            }
        }
        for (int i = 1; i + 1 < n; ++i)
            for (const ClipVertex* p : {&clipped[0], &clipped[i], &clipped[i + 1]})
                out.push_back(clip_vertex(*p, flat ? provoking : p->color));
    }

    void sprite(const ClipVertex& a, const ClipVertex& b) {
        float x0, y0, z0, x1, y1, z1;
        screen_xy(a, x0, y0, z0);
        screen_xy(b, x1, y1, z1);
        const float u0 = a.s / a.q, v0 = a.t / a.q, u1 = b.s / b.q, v1 = b.t / b.q;
        const GpuVertex tl = screen_vertex(x0, y0, z1, u0, v0, 1, b.color, b.spec, b.fog);
        const GpuVertex tr = screen_vertex(x1, y0, z1, u1, v0, 1, b.color, b.spec, b.fog);
        const GpuVertex bl = screen_vertex(x0, y1, z1, u0, v1, 1, b.color, b.spec, b.fog);
        const GpuVertex br = screen_vertex(x1, y1, z1, u1, v1, 1, b.color, b.spec, b.fog);
        out.insert(out.end(), {tl, tr, bl, tr, br, bl});
    }

    void point_vertex(const ClipVertex& c) {
        float x, y, z;
        if (!vp.through && c.clip[3] <= 0) return;
        screen_xy(c, x, y, z);
        out.push_back(screen_vertex(x + 0.5f, y + 0.5f, z, c.s / c.q, c.t / c.q, 1, c.color, c.spec, c.fog));
    }

    // ---- draw --------------------------------------------------------------------------------------------

    void draw(const ge::State& s, ge::Prim prim, const std::vector<ge::Vertex>& vertices) {
        begin_frame();
        const std::uint32_t clear_bits = s.param(ge::CLEARMODE);
        const bool clear = clear_bits & 1;
        const bool through = s.format.through;
        PipelineKey key;
        FragmentConstants pc{};

        // Texture.
        rt::gpu::TextureSampler ts;
        const bool texturing = s.on(ge::TEXTUREMAPENABLE) && !clear;
        if (texturing) ts.setup(s, memory);
        const bool fog = s.on(ge::FOGENABLE) && !through && !clear;
        vp.configure(s, texturing, texturing ? ts.width() : 1, texturing ? ts.height() : 1, fog);
        flat = !(s.param(ge::SHADEMODE) & 1);
        cull = s.on(ge::CULLFACEENABLE) && !through;
        cull_mode = static_cast<int>(s.param(ge::CULL) & 1);

        const auto transform_start = std::chrono::steady_clock::now();
        clip.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) clip[i] = vp.transform(s, vertices[i]);

        Target& t = target(s.param(ge::FRAMEBUFPTR), s.param(ge::FRAMEBUFWIDTH) & 0x7FC, static_cast<int>(s.param(ge::FRAMEBUFPIXFORMAT) & 3));
        target_w = static_cast<float>(t.stride);
        target_h = static_cast<float>(kTargetHeight);

        out.clear();
        const std::size_t n = clip.size();
        switch (prim) {
            case ge::Prim::Points:
                key.topology = 2;
                for (std::size_t i = 0; i < n; ++i) point_vertex(clip[i]);
                break;
            case ge::Prim::Lines:
            case ge::Prim::LineStrip:
                key.topology = 1;
                for (std::size_t i = 0; i + 1 < n; i += prim == ge::Prim::Lines ? 2 : 1) {
                    point_vertex(clip[i]);
                    point_vertex(clip[i + 1]);
                }
                break;
            case ge::Prim::Triangles:
                for (std::size_t i = 0; i + 2 < n; i += 3) triangle(clip[i], clip[i + 1], clip[i + 2]);
                break;
            case ge::Prim::TriangleStrip:
                for (std::size_t i = 0; i + 2 < n; ++i) {
                    if (i & 1) triangle(clip[i + 1], clip[i], clip[i + 2]);
                    else triangle(clip[i], clip[i + 1], clip[i + 2]);
                }
                break;
            case ge::Prim::TriangleFan:
                for (std::size_t i = 1; i + 1 < n; ++i) triangle(clip[0], clip[i], clip[i + 1]);
                break;
            case ge::Prim::Sprites:
                for (std::size_t i = 0; i + 1 < n; i += 2) sprite(clip[i], clip[i + 1]);
                break;
        }
        stats.transform_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                             std::chrono::steady_clock::now() - transform_start).count());
        if (out.empty()) return;

        // Fixed-function state.
        std::uint32_t stencil_ref = 0, stencil_mask = 0xFF;
        float blend_constants[4] = {0, 0, 0, 0};
        if (clear) {
            key.clear = 1;
            key.write_mask = static_cast<std::uint8_t>(((clear_bits >> 8) & 1 ? 0x7 : 0) | ((clear_bits >> 9) & 1 ? 0x8 : 0));
            key.depth_test = 1;
            key.depth_func = 1;  // always
            key.depth_write = (clear_bits >> 10) & 1;
            if ((clear_bits >> 9) & 1) {
                key.stencil = 1;
                key.stencil_func = 1;
                key.stencil_fail = key.stencil_zfail = key.stencil_pass = 2;  // replace
                stencil_ref = out[0].color >> 24;
            }
        } else {
            if (texturing) {
                const std::uint32_t func = s.param(ge::TEXFUNC);
                key.texture = 1;
                key.tex_func = static_cast<std::uint8_t>(std::min<std::uint32_t>(func & 7, 4));
                key.tex_alpha = (func >> 8) & 1;
                key.color_double = (func >> 16) & 1;
                const std::uint32_t env = s.param(ge::TEXENVCOLOR);
                for (int i = 0; i < 3; ++i) pc.env[i] = static_cast<float>((env >> (i * 8)) & 0xFF) / 255.0f;
            }
            key.fog = fog;
            const std::uint32_t fog_color = s.param(ge::FOGCOLOR);
            for (int i = 0; i < 3; ++i) pc.fog[i] = static_cast<float>((fog_color >> (i * 8)) & 0xFF) / 255.0f;
            if (s.on(ge::ALPHATESTENABLE)) {
                const std::uint32_t at = s.param(ge::ALPHATEST);
                key.alpha_test = static_cast<std::uint8_t>(at & 7);
                pc.alpha_ref = (at >> 8) & 0xFF;
                pc.alpha_mask = (at >> 16) & 0xFF;
            }
            if (s.on(ge::COLORTESTENABLE)) {
                key.color_test = static_cast<std::uint8_t>(s.param(ge::COLORTEST) & 3);
                pc.color_ref = s.param(ge::COLORREF);
                pc.color_mask = s.param(ge::COLORTESTMASK);
            }
            if (s.on(ge::ZTESTENABLE)) {
                key.depth_test = 1;
                key.depth_func = static_cast<std::uint8_t>(s.param(ge::ZTEST) & 7);
                key.depth_write = !s.on(ge::ZWRITEDISABLE);
            }
            const bool stencil = s.on(ge::STENCILTESTENABLE);
            if (stencil) {
                const std::uint32_t st = s.param(ge::STENCILTEST), so = s.param(ge::STENCILOP);
                key.stencil = 1;
                key.stencil_func = static_cast<std::uint8_t>(st & 7);
                key.stencil_fail = static_cast<std::uint8_t>(so & 7);
                key.stencil_zfail = static_cast<std::uint8_t>((so >> 8) & 7);
                key.stencil_pass = static_cast<std::uint8_t>((so >> 16) & 7);
                stencil_ref = (st >> 8) & 0xFF;
                stencil_mask = (st >> 16) & 0xFF;
            }
            // Per-channel write masks; the alpha channel holds stencil and is only written with the stencil test.
            const std::uint32_t mask_rgb = s.param(ge::MASKRGB), mask_a = s.param(ge::MASKALPHA) & 0xFF;
            key.write_mask = 0;
            for (int i = 0; i < 3; ++i)
                if (((mask_rgb >> (i * 8)) & 0xFF) != 0xFF) key.write_mask |= static_cast<std::uint8_t>(1 << i);
            if (stencil && mask_a != 0xFF) key.write_mask |= 0x8;
            if (s.on(ge::ALPHABLENDENABLE)) {
                const std::uint32_t bm = s.param(ge::BLENDMODE);
                const std::uint32_t a = bm & 15, b = (bm >> 4) & 15, eq = (bm >> 8) & 7;
                const std::uint32_t fa = s.param(ge::BLENDFIXEDA), fb = s.param(ge::BLENDFIXEDB);
                key.blend = 1;
                static constexpr VkBlendFactor kSrc[10] = {
                    VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                    VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
                static constexpr VkBlendFactor kDst[10] = {
                    VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                    VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
                    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
                // Doubled source alpha (codes 6/7) has no Vulkan factor: the shader applies the source factor and,
                // for a doubled destination factor, outputs min(2a, 1) as alpha. Doubled destination alpha
                // (codes 8/9) is approximated with plain destination alpha.
                const bool dst_double = b == 6 || b == 7;
                if (a == 6 || a == 7) {
                    key.src = VK_BLEND_FACTOR_ONE;
                    key.src_factor = a == 6 ? 2 : 3;
                } else if (dst_double && (a == 2 || a == 3)) {
                    key.src = VK_BLEND_FACTOR_ONE;
                    key.src_factor = a == 2 ? 4 : 5;
                } else if (a < 10) {
                    key.src = static_cast<std::uint8_t>(kSrc[a]);
                } else if ((fa & 0xFFFFFF) == 0xFFFFFF) {
                    key.src = VK_BLEND_FACTOR_ONE;
                } else if ((fa & 0xFFFFFF) == 0) {
                    key.src = VK_BLEND_FACTOR_ZERO;
                } else {  // Vulkan has one constant color: apply BLENDFIXEDA in the shader
                    key.src = VK_BLEND_FACTOR_ONE;
                    key.src_factor = 1;
                    for (int i = 0; i < 3; ++i) pc.fixed_a[i] = static_cast<float>((fa >> (i * 8)) & 0xFF) / 255.0f;
                }
                if (dst_double) {
                    key.dst = b == 6 ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    key.double_alpha = 1;
                } else if (b < 10) {
                    key.dst = static_cast<std::uint8_t>(kDst[b]);
                } else if ((fb & 0xFFFFFF) == 0xFFFFFF) {
                    key.dst = VK_BLEND_FACTOR_ONE;
                } else if ((fb & 0xFFFFFF) == 0) {
                    key.dst = VK_BLEND_FACTOR_ZERO;
                } else {
                    key.dst = VK_BLEND_FACTOR_CONSTANT_COLOR;
                    for (int i = 0; i < 3; ++i) blend_constants[i] = static_cast<float>((fb >> (i * 8)) & 0xFF) / 255.0f;
                }
                static constexpr VkBlendOp kOps[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                                      VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD,
                                                      VK_BLEND_OP_ADD};
                key.op = static_cast<std::uint8_t>(kOps[eq]);
            }
            if (s.on(ge::LOGICOPENABLE) && vk.logic_op) {
                key.logic = 1;
                key.logic_op = static_cast<std::uint8_t>(s.param(ge::LOGICOP) & 15);  // GE and Vulkan orders match
                key.blend = 0;
            }
        }

        // Resources (may end the current rendering for copies), then the render pass.
        VkImageView view = white.view;
        VkSampler smp = sampler(false, false, false, false, false, 1);
        if (texturing) {
            const std::uint32_t tex_addr = rt::ge::vram_canonical(((s.param(ge::TEXBUFWIDTH0) & 0x0F0000) << 8) |
                                                                  (s.param(ge::TEXADDR0) & 0xFFFFF0));
            const auto texture_start = std::chrono::steady_clock::now();
            Image* img = nullptr;
            Target* rt_tex = ts.format() < 4 ? target_containing(tex_addr) : nullptr;
            if (rt_tex) img = target_texture(*rt_tex, tex_addr, ts.width(), ts.height());
            if (!img) img = texture(s, ts, tex_addr);
            stats.texture_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                               std::chrono::steady_clock::now() - texture_start).count());
            const std::uint32_t filter = s.param(ge::TEXFILTER), wrap = s.param(ge::TEXWRAP);
            if (img) {
                view = img->view;
                smp = sampler(filter & 1, (filter >> 8) & 1, wrap & 1, (wrap >> 8) & 1, (filter & 4) != 0, img->mips);
            }
        }
        Image& d = depth(s.param(ge::ZBUFPTR), t);
        begin_rendering(t, d);

        Frame& f = frame();
        const VkDeviceSize bytes = out.size() * sizeof(GpuVertex);
        if (f.vertex_used + bytes > kVertexBytes) return;  // out of vertex space this frame
        std::memcpy(f.vertices.ptr + f.vertex_used, out.data(), bytes);
        const VkCommandBuffer cmd = f.draw;
        const VkPipeline p = pipeline(key);
        if (p != bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound_pipeline = p;
        }
        const VkDescriptorSet set = descriptor(view, smp);
        if (!set) return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        const std::uint32_t sc1 = s.param(ge::SCISSOR1), sc2 = s.param(ge::SCISSOR2);
        const int x1 = static_cast<int>(sc1 & 0x3FF), y1 = static_cast<int>((sc1 >> 10) & 0x3FF);
        const int x2 = std::min(static_cast<int>(sc2 & 0x3FF), static_cast<int>(t.stride) - 1);
        const int y2 = std::min(static_cast<int>((sc2 >> 10) & 0x3FF), kTargetHeight - 1);
        t.used_height = std::max(t.used_height, y2 + 1);
        VkRect2D scissor{{x1 * scale, y1 * scale}, {0, 0}};
        if (x2 >= x1 && y2 >= y1)
            scissor.extent = {static_cast<std::uint32_t>((x2 - x1 + 1) * scale), static_cast<std::uint32_t>((y2 - y1 + 1) * scale)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_mask);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, clear ? 0xFF : ~s.param(ge::MASKALPHA) & 0xFF);
        vkCmdSetBlendConstants(cmd, blend_constants);
        const VkDeviceSize offset = f.vertex_used;
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.vertices.buffer, &offset);
        vkCmdDraw(cmd, static_cast<std::uint32_t>(out.size()), 1, 0, 0);
        f.vertex_used += (bytes + 255) & ~VkDeviceSize{255};
    }

    void transfer(const ge::State& s) {
        // Copy in guest memory like the hardware, then refresh any render target the destination overlaps.
        const std::uint32_t src_w = s.param(ge::TRANSFERSRCW), dst_w = s.param(ge::TRANSFERDSTW);
        const std::uint32_t src = rt::ge::vram_canonical(((src_w & 0xFF0000) << 8) | (s.param(ge::TRANSFERSRC) & 0xFFFFF0));
        const std::uint32_t dst = rt::ge::vram_canonical(((dst_w & 0xFF0000) << 8) | (s.param(ge::TRANSFERDST) & 0xFFFFF0));
        const std::uint32_t src_stride = src_w & 0xFFF8, dst_stride = dst_w & 0xFFF8;
        const std::uint32_t sp = s.param(ge::TRANSFERSRCPOS), dp = s.param(ge::TRANSFERDSTPOS), sz = s.param(ge::TRANSFERSIZE);
        const std::uint32_t sx = sp & 0x3FF, sy = (sp >> 10) & 0x3FF, dx = dp & 0x3FF, dy = (dp >> 10) & 0x3FF;
        const std::uint32_t w = (sz & 0x3FF) + 1, h = ((sz >> 10) & 0x3FF) + 1;
        const std::uint32_t bpp = (s.param(ge::TRANSFERSTART) & 1) ? 4 : 2;
        rt::mark_guest_written(dst + (dy * dst_stride + dx) * bpp, ((h - 1) * dst_stride + w) * bpp);
        if (guest_copies)
            for (std::uint32_t y = 0; y < h; ++y)
                std::memmove(memory.ptr(dst + ((dy + y) * dst_stride + dx) * bpp), memory.ptr(src + ((sy + y) * src_stride + sx) * bpp), w * bpp);
        if (Target* t = target_containing(dst); t && t->address == dst && t->stride == dst_stride) {
            begin_frame();
            upload_region(*t, static_cast<int>(dx), static_cast<int>(dy), static_cast<int>(w), static_cast<int>(h));
        }
    }

    VulkanGeRenderer::Output finish_frame(std::uint32_t address, std::uint32_t stride, int format,
                                          std::vector<std::uint32_t>* capture) {
        // Saved once no new pipeline has appeared for ~10 s (the app may be killed rather than closed on Android).
        if (cache_dirty_frame && frame_number - cache_dirty_frame > 300) save_pipeline_cache();
        begin_frame();
        Target& t = target(address, stride ? stride : 512, format);
        end_rendering();
        transition(frame().draw, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VulkanGeRenderer::Output o{t.color.image, 480 * scale, 272 * scale};
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(o.width) * static_cast<VkDeviceSize>(o.height) * 4;
        if (capture) {
            if (readback_size < bytes) {
                if (readback.buffer) {
                    vkDeviceWaitIdle(vk.device);
                    vmaDestroyBuffer(vk.allocator, readback.buffer, readback.memory);
                }
                readback = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                readback_size = bytes;
            }
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {static_cast<std::uint32_t>(o.width), static_cast<std::uint32_t>(o.height), 1};
            vkCmdCopyImageToBuffer(frame().draw, t.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        }
        const VkFence fence = frame().fence;
        submit_frame();
        if (capture) {
            vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vmaInvalidateAllocation(vk.allocator, readback.memory, 0, bytes);
            capture->resize(static_cast<std::size_t>(o.width) * static_cast<std::size_t>(o.height));
            std::memcpy(capture->data(), readback.ptr, bytes);
            for (auto& p : *capture) p |= 0xFF000000u;  // alpha holds stencil
        }
        bound_target = nullptr;
        bound_depth = nullptr;
        return o;
    }
};

VulkanGeRenderer::VulkanGeRenderer(const VulkanContext& vk, rt::GuestMemory& memory, int scale,
                                   std::filesystem::path pipeline_cache)
    : impl_(std::make_unique<Impl>(vk, memory, scale, std::move(pipeline_cache))) {
    impl_->init();
}

VulkanGeRenderer::~VulkanGeRenderer() { impl_->shutdown(); }

void VulkanGeRenderer::draw(const rt::ge::State& state, rt::ge::Prim prim, const std::vector<rt::ge::Vertex>& vertices) {
    impl_->draw(state, prim, vertices);
}

void VulkanGeRenderer::transfer(const rt::ge::State& state) { impl_->transfer(state); }

VulkanGeRenderer::Stats VulkanGeRenderer::take_stats() { return std::exchange(impl_->stats, Stats{}); }

void VulkanGeRenderer::set_scale(int scale) { impl_->pending_scale = std::max(1, scale); }

int VulkanGeRenderer::scale() const { return impl_->scale; }

void VulkanGeRenderer::set_guest_copies(bool on) { impl_->guest_copies = on; }

VulkanGeRenderer::Output VulkanGeRenderer::finish_frame(std::uint32_t address, std::uint32_t stride, int format,
                                                       std::vector<std::uint32_t>* capture) {
    return impl_->finish_frame(address, stride, format, capture);
}

}  // namespace ff7cc::platform
