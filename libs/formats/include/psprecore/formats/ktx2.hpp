#pragma once

// KTX2 files (Khronos texture container) with block-compressed 2D textures and their mipmaps: what psptex bake
// writes for GPUs that sample compressed textures (BC7 on desktop, ASTC 4x4 on mobile), read back by the runtime.
// Only what that needs: one layer, one face, no supercompression, 4x4 blocks of 16 bytes.

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"

namespace psprecore::ktx2 {

// VkFormat values.
enum class Format : std::uint32_t {
    BC7 = 145,      // VK_FORMAT_BC7_UNORM_BLOCK
    ASTC4x4 = 157,  // VK_FORMAT_ASTC_4x4_UNORM_BLOCK
};

struct Texture {
    Format format = Format::BC7;
    int width = 0, height = 0;
    std::uint32_t levels = 1;
    nx::vector<std::uint8_t> data;  // level 0, then each mipmap (half the size, down to 1x1), blocks in rows
};

// Bytes of one level: 16 per 4x4 block, partial blocks at the edges counted whole.
inline std::size_t level_size(int width, int height) {
    return static_cast<std::size_t>((width + 3) / 4) * static_cast<std::size_t>((height + 3) / 4) * 16;
}

nx::vector<std::byte> write(const Texture& texture);
// False when the file is not a KTX2 file of that kind.
bool read(std::span<const std::byte> file, Texture& texture);

}  // namespace psprecore::ktx2
