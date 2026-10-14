#pragma once

// The Vulkan objects the display owns and shares with the GE renderer. Include after volk / vk_mem_alloc.

#include <volk.h>

#include <cstdint>

typedef struct VmaAllocator_T* VmaAllocator;

namespace psprecore::platform {

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VmaAllocator allocator = nullptr;
    std::uint32_t api_version = 0;
    bool logic_op = false;           // VkPhysicalDeviceFeatures::logicOp enabled
    bool sampler_anisotropy = false;
    bool depth_clamp = false;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;  // depth + stencil attachment format
    // Dynamic rendering (core in 1.3, else VK_KHR_dynamic_rendering).
    PFN_vkCmdBeginRendering begin_rendering = nullptr;
    PFN_vkCmdEndRendering end_rendering = nullptr;
};

}  // namespace psprecore::platform
