#include "psprecore/gpu/button_icons.hpp"

namespace psprecore::rt::gpu {

namespace {

std::span<const IconAtlas> g_atlases;

// 5x5 letters, one byte per row, bit 4 the left column.
constexpr std::uint8_t kLetterA[5] = {0b01110, 0b10001, 0b11111, 0b10001, 0b10001};
constexpr std::uint8_t kLetterB[5] = {0b11110, 0b10001, 0b11110, 0b10001, 0b11110};
constexpr std::uint8_t kLetterX[5] = {0b10001, 0b01010, 0b00100, 0b01010, 0b10001};
constexpr std::uint8_t kLetterY[5] = {0b10001, 0b01010, 0b00100, 0b00100, 0b00100};
constexpr std::uint8_t kLetterK[5] = {0b10001, 0b10010, 0b11100, 0b10010, 0b10001};
constexpr std::uint8_t kLetterL[5] = {0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
constexpr std::uint8_t kLetterJ[5] = {0b00111, 0b00010, 0b00010, 0b10010, 0b01100};
constexpr std::uint8_t kLetterI[5] = {0b01110, 0b00100, 0b00100, 0b00100, 0b01110};
// PlayStation shapes (the game's own icons are pictures; these are for icons drawn from scratch).
constexpr std::uint8_t kShapeCross[5] = {0b10001, 0b01010, 0b00100, 0b01010, 0b10001};
constexpr std::uint8_t kShapeCircle[5] = {0b01110, 0b10001, 0b10001, 0b10001, 0b01110};
constexpr std::uint8_t kShapeSquare[5] = {0b11111, 0b10001, 0b10001, 0b10001, 0b11111};
constexpr std::uint8_t kShapeTriangle[5] = {0b00100, 0b00100, 0b01010, 0b01010, 0b11111};
// By face (Cross, Circle, Square, Triangle): the button at the same position.
constexpr const std::uint8_t* kPlayStationShapes[4] = {kShapeCross, kShapeCircle, kShapeSquare, kShapeTriangle};
constexpr const std::uint8_t* kXboxLetters[4] = {kLetterA, kLetterB, kLetterX, kLetterY};
constexpr const std::uint8_t* kNintendoLetters[4] = {kLetterB, kLetterA, kLetterY, kLetterX};
constexpr const std::uint8_t* kKeyboardLetters[4] = {kLetterK, kLetterL, kLetterJ, kLetterI};  // display_vulkan.cpp keys
// Xbox button colours (RGB, R in the low byte), light enough to read on the dark discs.
constexpr std::uint32_t kXboxColors[4] = {0x46D26E, 0x4650F0, 0xFFA05A, 0x28D2FA};

int luminance(std::uint32_t c) { return static_cast<int>((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)); }

}  // namespace

void set_icon_atlases(std::span<const IconAtlas> atlases) { g_atlases = atlases; }

std::uint64_t icon_texture_hash(const std::uint8_t* data, std::size_t size) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

const IconAtlas* find_icon_atlas(int format, int width, int height, const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0;
    bool hashed = false;
    for (const IconAtlas& a : g_atlases) {
        if (a.format != format || a.width != width || a.height != height) continue;
        if (!hashed) {
            hash = icon_texture_hash(data, size);
            hashed = true;
        }
        if (a.hash == hash) return &a;
    }
    return nullptr;
}

void draw_button_icons(const IconAtlas& atlas, std::uint32_t* rgba, int width, IconStyle style) {
    if (style == IconStyle::PlayStation) return;  // the game's own icons
    for (const Icon& icon : atlas.icons) draw_icon(rgba, width, icon.x, icon.y, icon.face, style);
}

void draw_icon(std::uint32_t* rgba, int width, int cx, int cy, IconFace face_, IconStyle style) {
    const auto face = static_cast<std::size_t>(face_);
    const auto at = [&](int x, int y) -> std::uint32_t& { return rgba[y * width + x]; };
    // The disc: the darkest texel just outside the symbol. The ink: the symbol's brightest texel.
    std::uint32_t disc = at(cx - 3, cy);
    for (const std::uint32_t c : {at(cx + 3, cy), at(cx, cy - 3), at(cx, cy + 3)})
        if (luminance(c) < luminance(disc)) disc = c;
    std::uint32_t ink = disc;
    for (int y = cy - 2; y <= cy + 2; ++y)
        for (int x = cx - 2; x <= cx + 2; ++x)
            if (luminance(at(x, y)) > luminance(ink)) ink = at(x, y);
    const std::uint8_t* symbol = style == IconStyle::Xbox       ? kXboxLetters[face]
                                 : style == IconStyle::Nintendo ? kNintendoLetters[face]
                                 : style == IconStyle::Keyboard ? kKeyboardLetters[face]
                                                                : kPlayStationShapes[face];
    if (style == IconStyle::Xbox) ink = (ink & 0xFF000000u) | kXboxColors[face];
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) at(cx - 2 + x, cy - 2 + y) = (symbol[y] >> (4 - x)) & 1 ? ink : disc;
}

}  // namespace psprecore::rt::gpu
