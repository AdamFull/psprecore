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

#include "psprecore/platform/vulkan_context.hpp"
#include "psprecore/runtime/ge.hpp"
#include "psprecore/gpu/button_icons.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::rt {
class GuestMemory;
}

namespace psprecore::platform {

class VulkanGeRenderer final : public rt::ge::Renderer {
public:
    // `pipeline_cache`: file keeping compiled pipelines between runs (empty: none), so effects seen before do not
    // stall the first frame they appear in.
    VulkanGeRenderer(const VulkanContext& vk, rt::GuestMemory& memory, int scale,
                     nx::string pipeline_cache = {});
    ~VulkanGeRenderer() override;

    void draw(const rt::ge::State& state, rt::ge::Prim prim, const nx::vector<rt::ge::Vertex>& vertices) override;
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
    // Multisample anti-aliasing: 1 (off), 2, 4 or 8 samples per pixel, from the next frame on (clamped to what the
    // GPU supports; render targets are recreated).
    void set_msaa(int samples);
    int msaa() const;
    // The game's own button icons drawn as another controller's (psprecore/gpu/button_icons.hpp); cached textures are rebuilt.
    void set_button_icons(rt::gpu::IconStyle style);
    // Drops every cached texture: they are decoded (and replaced, psprecore/gpu/texture_replacement.hpp) again.
    void reload_textures();
    // Whether transfer() also performs the block copy in guest memory (off when another renderer already did).
    void set_guest_copies(bool on);
    // Drops draws until turned off again (fast-forwarding: only the frames that are shown are rendered). Nothing the
    // game sees changes: rendered pixels never reach guest memory, and transfers still copy there.
    void set_skip_draws(bool skip);
    // Fills a display of this aspect ratio (width / height; 0: off, the PSP's 480x272 picture): framebuffers that
    // are displayed get margins, left and right for a wider display, top and bottom for a narrower one. 3D drawn
    // with a full-screen scissor extends into them (the scene shows more at the sides, or above and below, with the
    // same view in the other direction), clears and solid full-screen fills cover them, other 2D stays centred.
    // Frames without 3D (movies, menus) are shown without the margins. What the game itself culls outside its own
    // view does not appear in them.
    void set_fill_aspect(float aspect);
    // Frame interpolation: the draws into displayed framebuffers are recorded, so render_between() can draw frames
    // between the last two the game finished (more frames than the game makes, its own speed unchanged).
    void set_interpolation(bool on);

    struct Output {
        VkImage image = VK_NULL_HANDLE;  // TRANSFER_SRC_OPTIMAL until the next frame starts
        int width = 0, height = 0;       // displayed region (480x272 times the scale, plus margins when filling)
        int x = 0, y = 0;                // its top-left corner in the image
    };
    // Ends the frame and submits it; returns the render target holding framebuffer `address`. With `capture`,
    // also reads the displayed region back (RGBA8, Output::width x height); this waits for the GPU.
    Output finish_frame(std::uint32_t address, std::uint32_t stride, int format,
                        nx::vector<std::uint32_t>* capture = nullptr);
    // Draws the last finished frame as it would look `t` (0..1) of the way from the one before: 3D draws found in
    // both frames (same vertex data, format, primitive and texture, in order) get their world, view, projection,
    // texture and bone matrices blended; other draws, and every draw after a camera cut, are drawn as they were.
    // Empty when there is no earlier frame. The returned image is valid like finish_frame's.
    Output render_between(float t);

private:
    struct Impl;
    nx::unique_ptr<Impl> impl_;
};

}  // namespace psprecore::platform
