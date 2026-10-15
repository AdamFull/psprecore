#pragma once

// GE texture sampling for the software renderer: formats 5650/5551/4444/8888, CLUT4/8/16/32 and the PSP
// DXT1/3/5 variants, swizzled layouts, wrap/clamp, nearest/bilinear and mip levels.

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/foundation/core/foundation.h"
#include "psprecore/runtime/ge.hpp"

namespace psprecore::rt {
class GuestMemory;
}

namespace psprecore::rt::gpu {

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

    // Decodes a whole level to RGBA8, width(level) * height(level) texels.
    void decode(int level, std::uint32_t* out) const;
    // Palette textures with 4- or 8-bit indices: each texel's index (false for other formats).
    bool decode_indices(int level, std::uint8_t* out) const;
    // The colour palette entry `index` stands for (the CLUT format's shift, mask and start applied).
    std::uint32_t palette_color(std::uint32_t index) const { return clut(index); }
    // Guest memory a level occupies (for change detection), and whether the CLUT affects the texels.
    const std::uint8_t* level_data(int level) const { return level_[level].data; }
    std::size_t level_size(int level) const;
    bool uses_clut() const { return format_ >= 4 && format_ <= 7; }
    int format() const { return format_; }

    // u, v are normalized to the level-0 size (1.0 = full width); the level picks the mip.
    std::uint32_t sample(float u, float v, int level, bool linear) const;

private:
    struct Level {
        const std::uint8_t* data = nullptr;
        int width = 1, height = 1, buffer_width = 0;
        std::uint32_t row_bytes = 0, blocks_per_row = 1;  // plain formats: bytes per row, 16-byte swizzle blocks per row
    };
    std::uint32_t texel(const Level& level, int x, int y) const;
    std::uint32_t clut(std::uint32_t index) const { return palette_[(index >> clut_shift_) & clut_mask_]; }
    void build_palette();

    // Sampling specialised per texel format and swizzle (chosen in setup); the generic path handles the rest.
    using SampleFn = std::uint32_t (*)(const TextureSampler&, float u, float v, int level, bool linear);
    template <int Format, bool Swizzled>
    NX_FORCE_INLINE static std::uint32_t fetch(const TextureSampler& t, const Level& l, int x, int y);
    template <int Format, bool Swizzled>
    static std::uint32_t sample_fixed(const TextureSampler& t, float u, float v, int level, bool linear);
    template <int Format, bool Swizzled>
    static void decode_fixed(const TextureSampler& t, const Level& l, std::uint32_t* out);
    static std::uint32_t sample_generic(const TextureSampler& t, float u, float v, int level, bool linear);
    SampleFn sample_fn_ = &sample_generic;

    Level level_[8];
    int levels_ = 1;
    int format_ = 0;
    bool swizzled_ = false, clamp_u_ = false, clamp_v_ = false;
    std::uint32_t clut_format_ = 0;
    const std::uint8_t* clut_ = nullptr;
    // The CLUT decoded to RGBA8 by masked index ((index >> shift) & mask; the start offset is applied), rebuilt only
    // when the CLUT data or format change.
    std::uint32_t clut_shift_ = 0, clut_mask_ = 0;
    std::array<std::uint32_t, 256> palette_{};
    std::array<std::uint8_t, 1024> palette_source_{};
    std::uint32_t palette_format_ = 0, palette_count_ = 0;
};

// The texture's content: the texels of every level and, for palette formats, the palette and its format. The
// Vulkan renderer's cache, texture replacement (psprecore/gpu/texture_replacement.hpp) and --tex-dump names use it.
std::uint64_t texture_content_hash(const ge::State& state, const TextureSampler& sampler);
// The texels alone (every level), whatever the palette: palette-aware texture replacements match by it.
std::uint64_t texture_texels_hash(const TextureSampler& sampler);

}  // namespace psprecore::rt::gpu
