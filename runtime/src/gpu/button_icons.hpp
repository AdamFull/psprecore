#pragma once

// The game's PlayStation button icons redrawn for other gamepads. The icons are small pictures inside UI textures
// (the battle HUD atlas, the text font sheet): a dark disc with a ring and a 5x5 symbol. Known textures are listed
// by their size, format and a hash of their texels; when one is decoded, each icon's symbol is replaced by the
// letter of the button at the same position (A/B/X/Y on Xbox pads, B/A/Y/X on Nintendo ones) or of the keyboard key
// that presses it, keeping the disc.
// Only positions and hashes are listed here, no game data.

#include <cstddef>
#include <cstdint>

namespace ff7cc::rt::gpu {

enum class IconStyle : std::uint8_t { PlayStation, Xbox, Nintendo, Keyboard };

struct IconAtlas;

// Stable texel hash for the table (independent of the renderers' cache hashes): FNV-1a 64.
std::uint64_t icon_texture_hash(const std::uint8_t* data, std::size_t size);

// The known texture with icons this is (level 0: texture format 0-10, size and texels), or null.
const IconAtlas* find_icon_atlas(int format, int width, int height, const std::uint8_t* data, std::size_t size);

// Redraws the atlas's icons in its decoded level 0 (RGBA8, `width` texels per row).
void draw_button_icons(const IconAtlas& atlas, std::uint32_t* rgba, int width, IconStyle style);

}  // namespace ff7cc::rt::gpu
