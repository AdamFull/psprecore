#pragma once

// A game's PlayStation button icons redrawn for other controllers. The icons are small pictures inside UI textures:
// a dark disc with a ring and a 5x5 symbol. The game lists its textures with icons by size, format and a hash of
// their texels (set_icon_atlases); when one is decoded, each icon's symbol is replaced by the letter of the button
// at the same position (A/B/X/Y on Xbox pads, B/A/Y/X on Nintendo ones) or of the keyboard key that presses it,
// keeping the disc.

#include <cstddef>
#include <cstdint>
#include <span>

namespace psprecore::rt::gpu {

enum class IconStyle : std::uint8_t { PlayStation, Xbox, Nintendo, Keyboard };

enum class IconFace : std::uint8_t { Cross, Circle, Square, Triangle };

struct Icon {
    std::uint16_t x, y;  // centre of the 5x5 symbol
    IconFace face;
};

struct IconAtlas {
    int format, width, height;  // level 0: texture format (0-10) and size
    std::uint64_t hash;         // icon_texture_hash of level 0
    std::span<const Icon> icons;
};

// The game's textures with icons (the span must stay valid). Positions and hashes only: no game data.
void set_icon_atlases(std::span<const IconAtlas> atlases);

// Stable texel hash for the tables (independent of the renderers' cache hashes): FNV-1a 64.
std::uint64_t icon_texture_hash(const std::uint8_t* data, std::size_t size);

// The known texture with icons this is (level 0: texture format 0-10, size and texels), or null.
const IconAtlas* find_icon_atlas(int format, int width, int height, const std::uint8_t* data, std::size_t size);

// Redraws the atlas's icons in its decoded level 0 (RGBA8, `width` texels per row).
void draw_button_icons(const IconAtlas& atlas, std::uint32_t* rgba, int width, IconStyle style);

}  // namespace psprecore::rt::gpu
