#pragma once

// Software reference renderer: draws GE primitives into guest VRAM exactly where the PSP would, so games that
// read back or reuse their framebuffers keep working. It is the correctness baseline for the Vulkan renderer.
//
// Primitives are queued and rasterized at flush() by several threads, each owning interleaved 16-row bands of
// the screen and walking the whole queue in order: every pixel sees the same operations in the same order as
// drawing one primitive at a time, so the output does not depend on the thread count.

#include <memory>

#include "ff7cc/runtime/ge.hpp"

namespace ff7cc::rt {
class GuestMemory;
}

namespace ff7cc::rt::gpu {

std::unique_ptr<ge::Renderer> make_soft_renderer(GuestMemory& memory);
// Rasterizer threads for renderers made after the call (0: one per hardware thread, at most 8).
void set_soft_renderer_threads(int threads);

}  // namespace ff7cc::rt::gpu
