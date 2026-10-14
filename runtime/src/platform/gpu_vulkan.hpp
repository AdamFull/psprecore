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
    VulkanGeRenderer(const VulkanContext& vk, rt::GuestMemory& memory, int scale);
    ~VulkanGeRenderer() override;

    void draw(const rt::ge::State& state, rt::ge::Prim prim, const std::vector<rt::ge::Vertex>& vertices) override;
    void transfer(const rt::ge::State& state) override;

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
