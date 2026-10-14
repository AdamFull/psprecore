#pragma once

// Software reference renderer: draws GE primitives into guest VRAM exactly where the PSP would, so games that
// read back or reuse their framebuffers keep working. It is the correctness baseline for the Vulkan renderer.

#include <memory>

#include "ff7cc/runtime/ge.hpp"

namespace ff7cc::rt {
class GuestMemory;
}

namespace ff7cc::rt::gpu {

std::unique_ptr<ge::Renderer> make_soft_renderer(GuestMemory& memory);

}  // namespace ff7cc::rt::gpu
