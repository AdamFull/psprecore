#pragma once

// GE texture sampling for the software renderer: formats 5650/5551/4444/8888, CLUT4/8/16/32 and the PSP
// DXT1/3/5 variants, swizzled layouts, wrap/clamp, nearest/bilinear and mip levels.

#include <cstdint>

#include "ff7cc/runtime/ge.hpp"

namespace ff7cc::rt {
class GuestMemory;
}

namespace ff7cc::rt::gpu {

// Packed colors are RGBA8 with R in the low byte (the PSP 8888 layout).
inline std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
std::uint32_t decode_5650(std::uint32_t p);
std::uint32_t decode_5551(std::uint32_t p);
std::uint32_t decode_4444(std::uint32_t p);

class TextureSampler {
public:
    void setup(const ge::State& state, const GuestMemory& memory);

    int levels() const { return levels_; }
    int width(int level = 0) const { return level_[level].width; }
    int height(int level = 0) const { return level_[level].height; }

    // u, v are normalized to the level-0 size (1.0 = full width); the level picks the mip.
    std::uint32_t sample(float u, float v, int level, bool linear) const;

private:
    struct Level {
        const std::uint8_t* data = nullptr;
        int width = 1, height = 1, buffer_width = 0;
    };
    std::uint32_t texel(const Level& level, int x, int y) const;
    std::uint32_t clut(std::uint32_t index) const;
    int wrap(int c, int size, bool clamp) const;

    Level level_[8];
    int levels_ = 1;
    int format_ = 0;
    bool swizzled_ = false, clamp_u_ = false, clamp_v_ = false;
    std::uint32_t clut_format_ = 0;
    const std::uint8_t* clut_ = nullptr;
};

}  // namespace ff7cc::rt::gpu
