#pragma once

// A game's text font, so that what the port adds to the picture (e.g. "Hold to skip" over movies) reads like the
// game's own text: fixed-size glyph cells in one of its icon atlases (button_icons.hpp), with one of its button
// icons, whose disc is reused for any button. When the game says where it keeps the sheet in memory, it is decoded
// from there the first time it is needed (a game may load its font long before it first draws text); otherwise the
// Vulkan renderer keeps a copy when it decodes that texture. Until either, the port's own UI font is used instead.
// Cell layout, character map and addresses only: no game data.

#include <array>
#include <cstdint>

#include "psprecore/gpu/button_icons.hpp"
#include "core/foundation/core/foundation.h"

namespace psprecore::rt::gpu {

struct UiFont {
    std::uint64_t atlas_hash = 0;                      // the IconAtlas holding the glyph sheet
    int cell_width = 0, cell_height = 0, columns = 0;  // glyph n: cell (n % columns, n / columns)
    std::array<std::int16_t, 128> ascii{};             // glyph per ASCII character, -1: none
    Icon button{};                                     // a button icon in the sheet
    // Where the game keeps the sheet (0: unknown): its texels and, for an indexed format, its palette, as the GE
    // would read them. Used only while the texels there match atlas_hash.
    std::uint32_t texels = 0, palette = 0;
    int format = 0, width = 0, height = 0;  // GE texture format (0-10), size
    int buffer_width = 0;                   // texels per row
    bool swizzled = false;
    int palette_format = 3;  // GE CLUT format: 0 5650, 1 5551, 2 4444, 3 8888
};

void set_ui_font(const UiFont& font);
const UiFont* ui_font();  // null when the game set none

// Renderers: whether the font's atlas is still to be kept, and keeping it (level 0 as decoded, RGBA8).
bool ui_font_wanted();
void keep_ui_font_sheet(const std::uint32_t* rgba, int width, int height);

// The kept sheet: false until the game has drawn text with it.
bool ui_font_sheet(nx::vector<std::uint32_t>& rgba, int& width, int& height);

}  // namespace psprecore::rt::gpu
