#pragma once

// Vulkan GE renderer: draws GE primitives on the GPU at a configurable internal resolution.
//
// Vertex processing is shared with the software renderer (gpu/vertex_pipeline); the fragment pipeline is the
// Slang shader shaders/ge.slang plus fixed-function Vulkan blend/depth/stencil state. Render targets are kept
// on the GPU per VRAM address (scaled), textures are decoded on the CPU and cached by content hash, and
// textures that point into a render target sample a copy of it (render-to-texture effects).
//
// Unlike the software renderer it does not write rendered pixels back to guest VRAM.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "ff7cc/platform/vulkan_context.hpp"
#include "ff7cc/runtime/ge.hpp"

namespace ff7cc::rt {
class GuestMemory;
}

namespace ff7cc::platform {

class VulkanGeRenderer final : public rt::ge::Renderer {
public:
    // `pipeline_cache`: file keeping compiled pipelines between runs (empty: none), so effects seen before do not
    // stall the first frame they appear in.
    VulkanGeRenderer(const VulkanContext& vk, rt::GuestMemory& memory, int scale,
                     std::filesystem::path pipeline_cache = {});
    ~VulkanGeRenderer() override;

    void draw(const rt::ge::State& state, rt::ge::Prim prim, const std::vector<rt::ge::Vertex>& vertices) override;
    void transfer(const rt::ge::State& state) override;
    // Counters since the last call (performance diagnostics, --perf).
    struct Stats {
        std::uint64_t hashed_bytes = 0, uploads = 0, uploaded_texels = 0;
        std::uint64_t pipelines = 0;  // created
        double pipeline_ms = 0.0, worst_pipeline_ms = 0.0;
        std::uint64_t transform_ns = 0, texture_ns = 0;  // vertex transform + primitive assembly; texture lookup
    };
    Stats take_stats();

    // Changes the internal resolution scale from the next frame on (render targets are recreated).
    void set_scale(int scale);
    int scale() const;
    // Whether transfer() also performs the block copy in guest memory (off when another renderer already did).
    void set_guest_copies(bool on);

    struct Output {
        VkImage image = VK_NULL_HANDLE;  // TRANSFER_SRC_OPTIMAL until the next frame starts
        int width = 0, height = 0;       // displayed region (480x272 times the scale)
    };
    // Ends the frame and submits it; returns the render target holding framebuffer `address`. With `capture`,
    // also reads the displayed region back (RGBA8, Output::width x height); this waits for the GPU.
    Output finish_frame(std::uint32_t address, std::uint32_t stride, int format,
                        std::vector<std::uint32_t>* capture = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ff7cc::platform
